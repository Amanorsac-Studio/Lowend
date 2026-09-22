// DspCommon.h — small, allocation-free DSP building blocks shared by the pedals,
// the amp, the cab, the synth and the tuner.
//
// Everything here is safe to construct on the audio thread and to reconfigure
// per block: coefficients are computed with plain arithmetic, never through
// JUCE's reference-counted `IIR::Coefficients` factories (those allocate).
// The one-pole filters, the LFO, the modulated delay line, the ADAA shaper, the
// soft ceiling and the DC blocker all come from the shared Sanctuary engine via
// FxDsp.h / Antialias.h, so the two products share one set of primitives.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include "BlockParams.h"
#include "FxDsp.h"

namespace lowend {

//==============================================================================
/// Index-addressed, lock-free parameter store — the same idiom `jm::FxUnit` and
/// `lowend::Block` use, for the engine objects that are configured from a
/// `ParamValues` map instead of being a rack unit (the amp, the cab, the synth,
/// the looper).
///
/// WHY THIS EXISTS. The message thread owns the Rig and edits it as a
/// `std::map<std::string,float>`; the audio thread must never read that map —
/// not because of the strings, but because a `std::map` is being mutated
/// underneath it. So each engine object keeps its own flat array of atomics: the
/// message thread resolves ids to indices once and writes floats, and the audio
/// thread reads floats by index and never touches a container.
class ParamStore
{
public:
    static constexpr int kMaxParams = 32;

    /// MESSAGE THREAD, once (from prepare). Seeds every slot with its default.
    void configure (const std::vector<ParamSpec>& list)
    {
        list_ = &list;
        jassert ((int) list.size() <= kMaxParams);
        for (size_t i = 0; i < list.size() && i < (size_t) kMaxParams; ++i)
            v_[i].store (list[i].def, std::memory_order_relaxed);
    }

    /// MESSAGE THREAD. Copies a whole map in, ignoring ids the registry does not
    /// have (which is how a preset written against an older build stays usable).
    void setAll (const ParamValues& pv)
    {
        if (list_ == nullptr) return;
        for (size_t i = 0; i < list_->size() && i < (size_t) kMaxParams; ++i)
        {
            auto it = pv.find ((*list_)[i].id);
            if (it != pv.end())
                v_[i].store (juce::jlimit ((*list_)[i].min, (*list_)[i].max, it->second),
                             std::memory_order_relaxed);
        }
    }

    /// AUDIO THREAD.
    float operator[] (int i) const noexcept
    {
        return (i >= 0 && i < kMaxParams) ? v_[(size_t) i].load (std::memory_order_relaxed) : 0.0f;
    }
    bool flag (int i) const noexcept { return (*this)[i] >= 0.5f; }
    int choice (int i) const noexcept { return (int) std::lround ((*this)[i]); }

    /// Debug guard: the index enums in each engine class must match the order of
    /// the registry list they were written against.
    bool verify (int index, const char* id) const
    {
        return list_ != nullptr && index >= 0 && index < (int) list_->size()
               && juce::String ((*list_)[(size_t) index].id) == id;
    }

private:
    const std::vector<ParamSpec>* list_ = nullptr;
    std::array<std::atomic<float>, kMaxParams> v_ {};
};

using jm::AdaaTanh;
using jm::DcBlocker;
using jm::OnePoleHP;
using jm::OnePoleLP;
using jm::SoftCeiling;

//==============================================================================
/// RBJ biquad, transposed direct form II. Coefficients are recomputed in place,
/// so a knob move is one call and no allocation.
struct Biquad
{
    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setPeak (double sr, float freq, float q, float gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = omega (sr, freq);
        const double alpha = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        const double cosw = std::cos (w);
        norm (1.0 + alpha * A, -2.0 * cosw, 1.0 - alpha * A,
              1.0 + alpha / A, -2.0 * cosw, 1.0 - alpha / A);
    }
    void setLowShelf (double sr, float freq, float q, float gainDb) noexcept { shelf (sr, freq, q, gainDb, true); }
    void setHighShelf (double sr, float freq, float q, float gainDb) noexcept { shelf (sr, freq, q, gainDb, false); }

    void setLowPass (double sr, float freq, float q) noexcept
    {
        const double w = omega (sr, freq), cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        const double b1c = 1.0 - cosw;
        norm (b1c * 0.5, b1c, b1c * 0.5, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }
    void setHighPass (double sr, float freq, float q) noexcept
    {
        const double w = omega (sr, freq), cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        const double b1c = -(1.0 + cosw);
        norm ((1.0 + cosw) * 0.5, b1c, (1.0 + cosw) * 0.5, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }
    void setBandPass (double sr, float freq, float q) noexcept
    {
        const double w = omega (sr, freq), cosw = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * juce::jmax (0.05f, q));
        norm (alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
    }
    void setBypass() noexcept { b0 = 1; b1 = b2 = a1 = a2 = 0; }

private:
    static double omega (double sr, float freq) noexcept
    {
        const double s = sr > 0.0 ? sr : 48000.0;
        // Keep the design frequency below Nyquist with a margin; a peak placed at
        // or above Nyquist warps into nonsense and can make the section unstable.
        return juce::MathConstants<double>::twoPi * juce::jlimit (10.0, s * 0.45, (double) freq) / s;
    }
    void shelf (double sr, float freq, float q, float gainDb, bool low) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = omega (sr, freq), cosw = std::cos (w), sinw = std::sin (w);
        const double alpha = sinw / 2.0 * std::sqrt ((A + 1.0 / A) * (1.0 / juce::jmax (0.05f, q) - 1.0) + 2.0);
        const double tsa = 2.0 * std::sqrt (A) * alpha;
        if (low)
            norm (A * ((A + 1) - (A - 1) * cosw + tsa),
                  2 * A * ((A - 1) - (A + 1) * cosw),
                  A * ((A + 1) - (A - 1) * cosw - tsa),
                  (A + 1) + (A - 1) * cosw + tsa,
                  -2 * ((A - 1) + (A + 1) * cosw),
                  (A + 1) + (A - 1) * cosw - tsa);
        else
            norm (A * ((A + 1) + (A - 1) * cosw + tsa),
                  -2 * A * ((A - 1) + (A + 1) * cosw),
                  A * ((A + 1) + (A - 1) * cosw - tsa),
                  (A + 1) - (A - 1) * cosw + tsa,
                  2 * ((A - 1) - (A + 1) * cosw),
                  (A + 1) - (A - 1) * cosw - tsa);
    }
    void norm (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        const double inv = 1.0 / (std::abs (na0) < 1.0e-12 ? 1.0e-12 : na0);
        b0 = (float) (nb0 * inv); b1 = (float) (nb1 * inv); b2 = (float) (nb2 * inv);
        a1 = (float) (na1 * inv); a2 = (float) (na2 * inv);
    }

    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
};

//==============================================================================
/// Peak/RMS envelope follower with separate attack and release times.
struct EnvFollower
{
    void prepare (double sr) noexcept { sr_ = sr > 0 ? sr : 48000.0; setTimes (attackMs_, releaseMs_); env = 0; }
    void setTimes (float attackMs, float releaseMs) noexcept
    {
        attackMs_ = attackMs; releaseMs_ = releaseMs;
        aA = coef (attackMs); aR = coef (releaseMs);
    }
    float process (float x) noexcept
    {
        const float a = std::abs (x);
        env += (a > env ? aA : aR) * (a - env);
        return env;
    }
    /// dB of the current envelope, floored so callers never see -inf.
    float db() const noexcept { return juce::Decibels::gainToDecibels (env, -100.0f); }
    void reset() noexcept { env = 0; }

    float env = 0;
private:
    float coef (float ms) const noexcept
    {
        const double t = juce::jmax (0.01, (double) ms) * 0.001;
        return (float) (1.0 - std::exp (-1.0 / (t * sr_)));
    }
    double sr_ = 48000.0;
    float attackMs_ = 5, releaseMs_ = 80, aA = 0.1f, aR = 0.01f;
};

//==============================================================================
/// Zero-delay-feedback 4-pole ladder (Moog-style), used by the tracked synth and
/// the envelope filter. Topology-preserving transform, one Newton-free pass per
/// sample; resonance is bounded so self-oscillation stays musical rather than
/// exploding when the cutoff is modulated fast by an envelope.
struct LadderFilter
{
    void prepare (double sr) noexcept { sr_ = sr > 0 ? sr : 48000.0; reset(); setCutoff (cutoff_); }
    void reset() noexcept { for (auto& s : z) s = 0.0f; }

    void setCutoff (float hz) noexcept
    {
        cutoff_ = juce::jlimit (20.0f, (float) (sr_ * 0.45), hz);
        const double wd = juce::MathConstants<double>::twoPi * cutoff_;
        const double T = 1.0 / sr_;
        const double wa = (2.0 / T) * std::tan (wd * T / 2.0);   // bilinear pre-warp
        g = (float) (wa * T / 2.0);
        G = g / (1.0f + g);
    }
    /// 0..1; 1.0 sits just under self-oscillation.
    void setResonance (float r) noexcept { k = juce::jlimit (0.0f, 1.0f, r) * 3.9f; }

    float process (float x) noexcept
    {
        // Resolve the feedback loop in closed form (the ZDF part): the four
        // one-poles are cascaded, so the loop gain through them is G^4.
        const float G4 = G * G * G * G;
        const float S = (((z[0] * G + z[1]) * G + z[2]) * G + z[3]) * (1.0f - G);
        const float u = (x - k * S) / (1.0f + k * G4);
        float v = u;
        for (int i = 0; i < 4; ++i)
        {
            const float w = (v - z[(size_t) i]) * G;
            v = w + z[(size_t) i];
            z[(size_t) i] = v + w;
        }
        // Gentle saturation in the loop keeps a hard envelope sweep from ringing
        // into a screech, and it is what makes the filter sound like a filter.
        return std::tanh (v);
    }

private:
    double sr_ = 48000.0;
    float cutoff_ = 1000.0f, g = 0.1f, G = 0.09f, k = 0.0f;
    std::array<float, 4> z {};
};

//==============================================================================
/// Downward gate with hysteresis. Used both by the input stage and by the gate
/// pedal, so a player who puts a gate on the board gets identical behaviour.
///
/// Hysteresis (a 6 dB gap between the open and close thresholds) is the whole
/// trick: a single threshold chatters on a decaying bass note exactly when the
/// note is most exposed.
struct NoiseGate
{
    void prepare (double sr) noexcept
    {
        sr_ = sr > 0 ? sr : 48000.0;
        env_.prepare (sr_);
        env_.setTimes (1.0f, 30.0f);
        gain_ = 0.0f; open_ = false;
    }
    void setParams (float thresholdDb, float releaseMs, float depth01) noexcept
    {
        openDb_ = thresholdDb;
        closeDb_ = thresholdDb - 6.0f;
        floor_ = juce::jlimit (0.0f, 1.0f, 1.0f - depth01);
        attackCoef_ = coef (2.0f);
        releaseCoef_ = coef (juce::jmax (5.0f, releaseMs));
    }
    /// Returns the gain to apply; the caller applies it to both channels so a
    /// stereo signal never gates one side and not the other.
    float nextGain (float detectorSample) noexcept
    {
        const float db = juce::Decibels::gainToDecibels (env_.process (detectorSample), -120.0f);
        if (! open_ && db > openDb_) open_ = true;
        else if (open_ && db < closeDb_) open_ = false;
        const float target = open_ ? 1.0f : floor_;
        gain_ += (target > gain_ ? attackCoef_ : releaseCoef_) * (target - gain_);
        return gain_;
    }
    void reset() noexcept { env_.reset(); gain_ = 0.0f; open_ = false; }
    bool isOpen() const noexcept { return open_; }

private:
    float coef (float ms) const noexcept
    {
        const double t = juce::jmax (0.1, (double) ms) * 0.001;
        return (float) (1.0 - std::exp (-1.0 / (t * sr_)));
    }
    double sr_ = 48000.0;
    EnvFollower env_;
    float openDb_ = -70, closeDb_ = -76, floor_ = 0.0f;
    float attackCoef_ = 0.2f, releaseCoef_ = 0.01f, gain_ = 0.0f;
    bool open_ = false;
};

//==============================================================================
/// Fixed integer-delay line — used for DI path alignment and the looper's seam
/// crossfade. Sized once in `prepare`; `setDelay` never reallocates.
struct FixedDelay
{
    void prepare (double sr, float maxMs)
    {
        size_ = juce::jmax (2, (int) (sr * maxMs / 1000.0) + 2);
        buf_.assign ((size_t) size_ * 2, 0.0f);
        w_ = 0; delay_ = 0;
    }
    void setDelaySamples (int d) noexcept { delay_ = juce::jlimit (0, size_ - 1, d); }
    int delaySamples() const noexcept { return delay_; }
    void push (float l, float r) noexcept
    {
        buf_[(size_t) w_ * 2] = l; buf_[(size_t) w_ * 2 + 1] = r;
        if (++w_ >= size_) w_ = 0;
    }
    float read (int ch) const noexcept
    {
        int r = w_ - 1 - delay_;
        while (r < 0) r += size_;
        return buf_[(size_t) r * 2 + (size_t) ch];
    }
    void reset() noexcept { std::fill (buf_.begin(), buf_.end(), 0.0f); w_ = 0; }

private:
    std::vector<float> buf_;
    int size_ = 2, w_ = 0, delay_ = 0;
};

//==============================================================================
/// Equal-power crossfade pair, for the places where a click would be heard:
/// preset changes, looper state changes, cab swaps.
inline void equalPower (float t, float& gOld, float& gNew) noexcept
{
    const float x = juce::jlimit (0.0f, 1.0f, t) * juce::MathConstants<float>::halfPi;
    gOld = std::cos (x);
    gNew = std::sin (x);
}

/// MIDI note number of a frequency, and the cents error against the nearest note.
inline float noteFromHz (float hz) noexcept
{
    return hz > 0.0f ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 0.0f;
}
inline float hzFromNote (float note) noexcept
{
    return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f);
}

} // namespace lowend
