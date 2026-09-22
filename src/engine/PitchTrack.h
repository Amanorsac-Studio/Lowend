// PitchTrack.h — monophonic pitch and envelope tracking for the bass input.
//
// This is what lets Low End give a bass player a synth voice and a tuner
// with nothing but a jack: the played note is detected from the audio, and the
// detected note drives the synth (BassSynth.h). The tuner
// reads the same detector.
//
// ALGORITHM. YIN's cumulative-mean-normalised difference function, with
// parabolic interpolation of the chosen lag. YIN was chosen over plain
// autocorrelation because autocorrelation's peak-picking octave-halves on a
// bass — a low E has a stronger second harmonic than fundamental on most
// pickups, and an octave error on a synth voice is not a subtle artefact.
//
// COST. The detector runs on a 4x-decimated copy of the input (bass
// fundamentals live below 500 Hz, so 12 kHz is generous) over a window of two
// periods of the lowest note. At 48 kHz that is a 800-sample window and 376
// candidate lags, evaluated once every 512 input samples — about 1 % of a core,
// on the audio thread, with no allocation after `prepare`.
//
// LATENCY. Pitch detection needs to SEE two periods before it can name a note:
// ~64 ms on a low B, ~48 ms on a low E. That is physics, not implementation, and
// it is why the tracked synth is a layer mixed alongside the direct bass path
// rather than something the dry signal is delayed to match.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>
#include "DspCommon.h"

namespace lowend {

struct PitchResult
{
    bool voiced = false;      ///< a stable, confident pitch is present
    float hz = 0.0f;          ///< fundamental
    float note = 0.0f;        ///< fractional MIDI note number
    float confidence = 0.0f;  ///< 0..1 (1 - YIN's normalised difference at the chosen lag)
    float level = 0.0f;       ///< input envelope, linear
};

/// A note boundary the synth consumes. Emitted from the audio thread
/// into their own processing, never queued across threads.
struct NoteEvent
{
    enum class Kind { on, off, retune } kind = Kind::on;
    float note = 0.0f;        ///< fractional MIDI note (retune carries a bend)
    float velocity = 0.0f;    ///< 0..1, from the attack envelope
};

class PitchTracker
{
public:
    void prepare (double sampleRate)
    {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        decim_ = juce::jmax (1, (int) std::round (sr_ / 12000.0));
        dsr_ = sr_ / decim_;

        maxLag_ = (int) std::ceil (dsr_ / kMinHz);          // lowest note we look for
        minLag_ = juce::jmax (2, (int) std::floor (dsr_ / kMaxHz));
        window_ = 2 * maxLag_;
        buf_.assign ((size_t) window_ + (size_t) maxLag_ + 4, 0.0f);
        diff_.assign ((size_t) maxLag_ + 2, 0.0f);
        cmnd_.assign ((size_t) maxLag_ + 2, 0.0f);

        aa_.setCutoff (4500.0f, sr_);
        preHp_.setHighPass (sr_, 28.0f, 0.707f);
        env_.prepare (sr_);
        env_.setTimes (2.0f, 90.0f);
        fastEnv_.prepare (sr_);
        fastEnv_.setTimes (0.5f, 25.0f);
        hop_ = juce::jmax (64, (int) std::round (sr_ / 94.0));   // ~10.7 ms
        reset();
    }

    void reset()
    {
        std::fill (buf_.begin(), buf_.end(), 0.0f);
        write_ = 0; filled_ = 0; sinceHop_ = 0; decimCount_ = 0; decimAcc_ = 0.0f;
        env_.reset(); fastEnv_.reset(); aa_.reset(); preHp_.reset();
        result_ = {};
        stableCount_ = 0; lastNote_ = 0.0f; sounding_ = false; peakSinceOn_ = 0.0f;
    }

    /// Feed one input sample (mono sum). Cheap: the expensive analysis only runs
    /// on hop boundaries.
    void pushSample (float x) noexcept
    {
        const float f = preHp_.process (x);
        env_.process (f);
        fastEnv_.process (f);
        peakSinceOn_ = juce::jmax (peakSinceOn_, fastEnv_.env);

        decimAcc_ += aa_.process (f);
        if (++decimCount_ >= decim_)
        {
            buf_[(size_t) write_] = decimAcc_ / (float) decim_;
            decimAcc_ = 0.0f; decimCount_ = 0;
            if (++write_ >= (int) buf_.size()) write_ = 0;
            if (filled_ < (int) buf_.size()) ++filled_;
        }
        ++sinceHop_;
    }

    /// True when `pushSample` has taken in a full hop and analysis is due.
    bool hopDue() const noexcept { return sinceHop_ >= hop_; }

    /// Run the detector. Call from the audio thread when `hopDue()`.
    const PitchResult& analyse() noexcept
    {
        sinceHop_ = 0;
        result_.level = env_.env;
        if (filled_ < window_ + maxLag_)
            return result_;

        // Copy the newest window out of the ring into a linear view. `linear_`
        // is sized in prepare, so this is a memcpy, not an allocation.
        if ((int) linear_.size() < window_ + maxLag_ + 2)
            linear_.assign ((size_t) window_ + (size_t) maxLag_ + 2, 0.0f);
        const int need = window_ + maxLag_;
        int r = write_ - need;
        while (r < 0) r += (int) buf_.size();
        for (int i = 0; i < need; ++i)
        {
            linear_[(size_t) i] = buf_[(size_t) r];
            if (++r >= (int) buf_.size()) r = 0;
        }

        // ---- YIN step 1: squared difference function over the window
        for (int tau = minLag_; tau <= maxLag_; ++tau)
        {
            float sum = 0.0f;
            const float* a = linear_.data();
            const float* b = linear_.data() + tau;
            for (int i = 0; i < window_; ++i) { const float d = a[i] - b[i]; sum += d * d; }
            diff_[(size_t) tau] = sum;
        }

        // ---- YIN step 2: cumulative mean normalisation. Without this the
        // difference function is smallest at tau = 0 and every octave-down lag
        // looks just as good as the true period.
        cmnd_[(size_t) minLag_] = 1.0f;
        float running = 0.0f;
        for (int tau = minLag_; tau <= maxLag_; ++tau)
        {
            running += diff_[(size_t) tau];
            cmnd_[(size_t) tau] = running > 0.0f ? diff_[(size_t) tau] * (float) (tau - minLag_ + 1) / running : 1.0f;
        }

        // ---- YIN step 3: absolute threshold, first local minimum below it.
        // Taking the FIRST dip below threshold rather than the global minimum is
        // what stops the octave-down error: the true period dips first.
        int best = -1;
        for (int tau = minLag_ + 1; tau < maxLag_; ++tau)
        {
            if (cmnd_[(size_t) tau] < kThreshold)
            {
                while (tau + 1 < maxLag_ && cmnd_[(size_t) (tau + 1)] < cmnd_[(size_t) tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)   // nothing under threshold: take the global minimum and be honest about confidence
        {
            float lo = 1.0e9f;
            for (int tau = minLag_ + 1; tau < maxLag_; ++tau)
                if (cmnd_[(size_t) tau] < lo) { lo = cmnd_[(size_t) tau]; best = tau; }
        }

        float periodOut = (float) best;
        if (best > minLag_ && best < maxLag_)   // parabolic interpolation of the dip
        {
            const float a = cmnd_[(size_t) (best - 1)], b = cmnd_[(size_t) best], c = cmnd_[(size_t) (best + 1)];
            const float denom = a + c - 2.0f * b;
            if (std::abs (denom) > 1.0e-9f) periodOut += 0.5f * (a - c) / denom;
        }

        const float conf = best > 0 ? juce::jlimit (0.0f, 1.0f, 1.0f - cmnd_[(size_t) best]) : 0.0f;
        const float hz = periodOut > 0.0f ? (float) (dsr_ / periodOut) : 0.0f;

        result_.hz = hz;
        result_.note = noteFromHz (hz);
        result_.confidence = conf;
        result_.voiced = conf >= kMinConfidence && hz >= kMinHz && hz <= kMaxHz
                         && env_.env > kSilenceLinear;
        return result_;
    }

    /// Turn the detector's state into note boundaries. Call after `analyse()`.
    /// Returns the number of events written into `out` (at most 2 per hop).
    int collectEvents (NoteEvent* out, int maxOut, float sensitivity01 = 0.5f) noexcept
    {
        int n = 0;
        const float onLevel = kSilenceLinear * (20.0f - 15.0f * juce::jlimit (0.0f, 1.0f, sensitivity01));
        const float offLevel = onLevel * 0.35f;              // hysteresis, as in the gate

        if (result_.voiced)
        {
            const float quantised = std::round (result_.note);
            if (! sounding_ && result_.level > onLevel)
            {
                // Stability gate: two consecutive hops naming the same semitone.
                // One hop is 10.7 ms, so this costs 11 ms and removes the
                // wrong-note blip at the very start of a pluck, where the string
                // is still inharmonic.
                if (stableCount_ > 0 && std::abs (quantised - lastNote_) < 0.5f) ++stableCount_;
                else { stableCount_ = 1; lastNote_ = quantised; }
                if (stableCount_ >= 2 && n < maxOut)
                {
                    const float vel = juce::jlimit (0.05f, 1.0f, std::sqrt (peakSinceOn_ * 6.0f));
                    out[n++] = { NoteEvent::Kind::on, result_.note, vel };
                    sounding_ = true;
                    soundingNote_ = quantised;
                    peakSinceOn_ = 0.0f;
                }
            }
            else if (sounding_)
            {
                if (std::abs (quantised - soundingNote_) >= 0.5f)
                {
                    // A new note on the same string: hammer-on, slide, or just
                    // the next note. Off then on, so envelopes retrigger.
                    if (n < maxOut) out[n++] = { NoteEvent::Kind::off, soundingNote_, 0.0f };
                    if (n < maxOut)
                    {
                        const float vel = juce::jlimit (0.05f, 1.0f, std::sqrt (peakSinceOn_ * 6.0f));
                        out[n++] = { NoteEvent::Kind::on, result_.note, vel };
                    }
                    soundingNote_ = quantised;
                    peakSinceOn_ = 0.0f;
                }
                else if (n < maxOut)
                    out[n++] = { NoteEvent::Kind::retune, result_.note, 0.0f };   // vibrato / bends
            }
        }
        else if (sounding_ && result_.level < offLevel)
        {
            if (n < maxOut) out[n++] = { NoteEvent::Kind::off, soundingNote_, 0.0f };
            sounding_ = false;
            stableCount_ = 0;
            peakSinceOn_ = 0.0f;
        }
        return n;
    }

    const PitchResult& result() const noexcept { return result_; }
    bool isSounding() const noexcept { return sounding_; }
    int hopSamples() const noexcept { return hop_; }

private:
    static constexpr float kMinHz = 30.0f;         // below a low B (30.87 Hz), with margin
    static constexpr float kMaxHz = 500.0f;        // above the 24th fret of the G string
    static constexpr float kThreshold = 0.15f;     // YIN's absolute threshold
    static constexpr float kMinConfidence = 0.72f;
    static constexpr float kSilenceLinear = 0.0015f;   // about -56 dBFS

    double sr_ = 48000.0, dsr_ = 12000.0;
    int decim_ = 4, window_ = 800, minLag_ = 24, maxLag_ = 400, hop_ = 512;
    std::vector<float> buf_, linear_, diff_, cmnd_;
    int write_ = 0, filled_ = 0, sinceHop_ = 0, decimCount_ = 0;
    float decimAcc_ = 0.0f;

    OnePoleLP aa_;
    Biquad preHp_;
    EnvFollower env_, fastEnv_;

    PitchResult result_;
    int stableCount_ = 0;
    float lastNote_ = 0.0f, soundingNote_ = 0.0f, peakSinceOn_ = 0.0f;
    bool sounding_ = false;
};

} // namespace lowend
