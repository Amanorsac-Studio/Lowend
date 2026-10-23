// Metronome.h — the click, and the looper's count-in.
//
// The click is generated, not sampled: a short decaying tone (or a burst of
// noise for "Hat"), so it costs nothing to ship and never drifts from the
// tempo. It is mixed in AFTER the looper and after the limiter, so it is never
// recorded into a loop and never pumps the limiter on the bass.
//
// TIMING. Ticks are counted in samples from the last tick, so the click is
// exact at any buffer size. Inside a DAW that is playing, the position comes
// from the host's beat position every block instead, so the click sits on the
// DAW's grid rather than on its own.
//
// COUNT-IN. `beginCountIn` restarts the bar so beat one lands on the stomp,
// counts N bars, and reports (through `countInDue`) the exact sample inside a
// block at which the looper should start recording. The count-in plays even
// when the click itself is switched off.
//
// THREADS. Settings are atomics written by the message thread and read once per
// block. Everything else is audio-thread state.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

namespace lowend {

/// The player's click settings. Global (not per preset): a scene change on
/// stage must never switch the click off.
struct ClickSettings
{
    enum Sound { click = 0, wood, hat };
    enum Output { mainMix = 0, separate, both };

    bool on = false;
    float levelDb = -6.0f;
    int sound = click;
    bool accent = true;
    int beatsPerBar = 4;     // 1..12: the top of the time signature
    int beatUnit = 4;        // 4 or 8: the bottom (6/8 counts eighth notes)
    bool eighths = false;    // add the off-beat
    int countInBars = 0;     // 0, 1 or 2: bars counted in before the looper records
    int loopBars = 0;        // 0 = free, else 1, 2, 4, 8 or 16: the loop closes itself at that length
    int output = mainMix;
};

class Metronome
{
public:
    void prepare (double sr) noexcept
    {
        sr_ = sr > 0 ? sr : 48000.0;
        stop();
        voiceAge_ = kSilent;
    }

    //------------------------------------------------------------ settings
    /// MESSAGE THREAD.
    void setSettings (const ClickSettings& s) noexcept
    {
        on_.store (s.on, std::memory_order_relaxed);
        levelDb_.store (juce::jlimit (-60.0f, 6.0f, s.levelDb), std::memory_order_relaxed);
        sound_.store (juce::jlimit (0, 2, s.sound), std::memory_order_relaxed);
        accent_.store (s.accent, std::memory_order_relaxed);
        beats_.store (juce::jlimit (1, 12, s.beatsPerBar), std::memory_order_relaxed);
        unit_.store (s.beatUnit == 8 ? 8 : 4, std::memory_order_relaxed);
        eighths_.store (s.eighths, std::memory_order_relaxed);
    }
    bool isOn() const noexcept { return on_.load (std::memory_order_relaxed); }
    int beatsPerBar() const noexcept { return beats_.load (std::memory_order_relaxed); }

    //------------------------------------------------------------ the grid
    // The looper reads the click's bar from here, so a loop's "1" is the
    // click's "1". Audio thread, before `render` for the same block.
    double barSamples (double bpm) const noexcept { return tickSamples (bpm) * ticksPerBar(); }
    /// One pulse: a quarter note in x/4, an eighth note in x/8.
    double pulseSamples (double bpm) const noexcept
    {
        const double b = bpm > 1.0 ? bpm : 120.0;
        return sr_ * 60.0 / b / (unit_.load (std::memory_order_relaxed) == 8 ? 2.0 : 1.0);
    }
    /// Samples since the current bar began.
    double barPhase (double bpm) const noexcept
    {
        const double bar = barSamples (bpm);
        if (bar <= 0.0) return 0.0;
        double ph = std::fmod (tickIndex_ * tickSamples (bpm) - toNextTick_, bar);
        if (ph < 0.0) ph += bar;
        return ph;
    }
    /// Puts the click at `phase` samples into its bar (a loop already playing
    /// when the click is switched on: the click joins the loop, not the reverse).
    void setBarPhase (double phase, double bpm) noexcept
    {
        const double tick = tickSamples (bpm);
        const int perBar = ticksPerBar();
        const double k = std::floor (phase / tick);
        if (phase - k * tick < 1.0) { tickIndex_ = (int) k % perBar; toNextTick_ = 0.0; }
        else { tickIndex_ = ((int) k + 1) % perBar; toNextTick_ = (k + 1.0) * tick - phase; }
    }

    //------------------------------------------------------------ count-in
    /// AUDIO THREAD. Beat one of the count-in falls at `offset` in this block.
    void beginCountIn (int bars, int offset, double bpm) noexcept
    {
        const double tick = tickSamples (bpm);
        tickIndex_ = 0;
        toNextTick_ = (double) juce::jmax (0, offset);
        countInLeft_ = (double) juce::jmax (0, offset) + juce::jmax (1, bars) * ticksPerBar() * tick;
        countingIn_ = true;
    }
    void cancelCountIn() noexcept { countingIn_ = false; countInLeft_ = 0; }
    bool countingIn() const noexcept { return countingIn_; }

    /// AUDIO THREAD, before `render` for the same block: the sample at which
    /// the count-in ends inside a block of `n`, or -1 if it does not end in it.
    int countInDue (int n) const noexcept
    {
        if (! countingIn_ || countInLeft_ >= n) return -1;
        return juce::jlimit (0, n - 1, (int) std::ceil (countInLeft_));
    }

    //-------------------------------------------------------------- render
    /// AUDIO THREAD. Writes (not adds) the click for this block into `l`/`r`.
    /// `hostPpq` is the host's position in quarter notes at the block start,
    /// or a negative number when there is no playing host.
    void render (float* l, float* r, int n, double bpm, double hostPpq = -1.0) noexcept
    {
        const bool counting = countingIn_;
        const bool on = on_.load (std::memory_order_relaxed);
        const bool audible = on || counting;
        const double tick = tickSamples (bpm);
        const int perBar = ticksPerBar();
        const int sub = ticksPerPulse();

        if (! audible && voiceAge_ >= kSilent)
        {
            // Off: rest at the top of a bar so switching on starts on beat one.
            stop();
            juce::FloatVectorOperations::clear (l, n);
            if (r != nullptr) juce::FloatVectorOperations::clear (r, n);
            return;
        }

        // A playing host owns the grid (ignored during a count-in, which is
        // the player's own bar).
        if (hostPpq >= 0.0 && ! counting)
        {
            const double t = hostPpq * sub;
            double next = std::ceil (t - 1.0e-9);
            toNextTick_ = (next - t) * tick;
            tickIndex_ = (int) ((long long) next % perBar);
        }

        const float level = juce::Decibels::decibelsToGain (levelDb_.load (std::memory_order_relaxed));
        const int sound = sound_.load (std::memory_order_relaxed);
        const bool accent = accent_.load (std::memory_order_relaxed);

        for (int i = 0; i < n; ++i)
        {
            // The count-in stops sounding at its own last sample, not at the
            // end of the block it ends in.
            const bool clicking = on || (counting && i < countInLeft_);
            if (clicking && toNextTick_ <= 0.0)
            {
                const bool one = tickIndex_ == 0;
                const bool onBeat = isPulse (tickIndex_, perBar, sub);
                trigger (sound, one && accent ? 2 : (onBeat ? 1 : 0),
                         level * (one && accent ? 1.0f : onBeat ? 0.62f : 0.4f));
                tickIndex_ = (tickIndex_ + 1) % perBar;
                toNextTick_ += tick;
            }
            toNextTick_ -= 1.0;
            const float s = voiceSample();
            l[i] = s;
            if (r != nullptr) r[i] = s;
        }

        if (counting)
        {
            countInLeft_ -= n;
            if (countInLeft_ <= 0) countingIn_ = false;
        }
    }

    /// For the UI: which beat of the bar sounded last (0-based), and whether a
    /// count-in is running.
    int beatForUi() const noexcept
    {
        const int sub = ticksPerPulse();
        const int perBar = ticksPerBar();
        return ((tickIndex_ + perBar - 1) % perBar) / sub;
    }

private:
    static constexpr int kSilent = 1 << 30;

    /// Clicks per counted beat: 2 when quarter notes are subdivided; x/8 already
    /// counts eighths, so it is never subdivided further.
    int ticksPerPulse() const noexcept
    {
        return unit_.load (std::memory_order_relaxed) == 8 ? 1 : (eighths_.load (std::memory_order_relaxed) ? 2 : 1);
    }
    int ticksPerBar() const noexcept
    {
        return beats_.load (std::memory_order_relaxed) * ticksPerPulse();
    }
    double tickSamples (double bpm) const noexcept
    {
        // The tempo is in quarter notes (as in every DAW): an eighth is half of one.
        const double b = bpm > 1.0 ? bpm : 120.0;
        const bool eighthGrid = unit_.load (std::memory_order_relaxed) == 8 || eighths_.load (std::memory_order_relaxed);
        return sr_ * 60.0 / b / (eighthGrid ? 2.0 : 1.0);
    }
    /// Which ticks are the felt beats. x/4: every quarter. x/8: the start of each
    /// group (6/8, 9/8, 12/8 in threes; 5/8 as 3+2; 7/8 as 2+2+3).
    bool isPulse (int idx, int perBar, int sub) const noexcept
    {
        if (unit_.load (std::memory_order_relaxed) != 8) return idx % sub == 0;
        if (perBar % 3 == 0 && perBar > 3) return idx % 3 == 0;
        if (perBar == 5) return idx == 0 || idx == 3;
        if (perBar == 7) return idx == 0 || idx == 2 || idx == 4;
        return idx % 2 == 0;
    }
    void stop() noexcept { tickIndex_ = 0; toNextTick_ = 0.0; countingIn_ = false; countInLeft_ = 0; }

    /// strength: 2 = the one, 1 = a beat, 0 = an off-beat.
    void trigger (int sound, int strength, float gain) noexcept
    {
        voiceSound_ = sound;
        voiceGain_ = gain;
        voiceAge_ = 0;
        phase_ = 0.0;
        // The "1" is an octave above the beats, so the bar is heard, not counted.
        static constexpr float clickHz[] = { 880.0f, 1175.0f, 2350.0f };
        static constexpr float woodHz[]  = { 560.0f, 760.0f, 1520.0f };
        voiceHz_ = sound == ClickSettings::wood ? woodHz[strength] : clickHz[strength];
        hp_ = 0.0f; prevNoise_ = 0.0f;
    }

    float voiceSample() noexcept
    {
        if (voiceAge_ >= kSilent) return 0.0f;
        const double t = voiceAge_ / sr_;
        if (t > 0.06) { voiceAge_ = kSilent; return 0.0f; }
        ++voiceAge_;
        // 0.5 ms attack so the first sample is not a step (and not zero).
        const float atk = (float) juce::jmin (1.0, (voiceAge_ / sr_) / 0.0005);
        float s = 0.0f;
        switch (voiceSound_)
        {
            case ClickSettings::wood:
            {
                // A struck block: the pitch falls fast as the strike settles,
                // with an inharmonic partial for the knock.
                const double f = voiceHz_ * (1.0 + 0.6 * std::exp (-t / 0.002));
                phase_ += f / sr_;
                const double p = juce::MathConstants<double>::twoPi * phase_;
                s = (float) ((std::sin (p) + 0.35 * std::sin (2.76 * p)) * std::exp (-t / 0.012));
                break;
            }
            case ClickSettings::hat:
            {
                // Bright noise: white noise through a one-pole high-pass.
                rng_ = rng_ * 1664525u + 1013904223u;
                const float w = (float) ((rng_ >> 9) & 0x7fffff) / (float) 0x7fffff * 2.0f - 1.0f;
                hp_ = 0.86f * (hp_ + w - prevNoise_);
                prevNoise_ = w;
                s = hp_ * (float) std::exp (-t / 0.010);
                break;
            }
            default:
            {
                phase_ += voiceHz_ / sr_;
                s = (float) (std::sin (juce::MathConstants<double>::twoPi * phase_) * std::exp (-t / 0.008));
                break;
            }
        }
        return s * atk * voiceGain_;
    }

    double sr_ = 48000.0;
    std::atomic<bool> on_ { false }, accent_ { true }, eighths_ { false };
    std::atomic<float> levelDb_ { -6.0f };
    std::atomic<int> sound_ { 0 }, beats_ { 4 }, unit_ { 4 };

    int tickIndex_ = 0;
    double toNextTick_ = 0.0;
    bool countingIn_ = false;
    double countInLeft_ = 0.0;

    int voiceSound_ = 0, voiceAge_ = kSilent;
    float voiceGain_ = 0.0f, voiceHz_ = 1000.0f, hp_ = 0.0f, prevNoise_ = 0.0f;
    double phase_ = 0.0;
    juce::uint32 rng_ = 0x1234567u;
};

} // namespace lowend
