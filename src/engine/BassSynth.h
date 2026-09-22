// BassSynth.h — the monophonic voice that follows the note the bass is playing.
//
// One voice, because the source is one bass string at a time: two detuned
// oscillators plus a square sub, a 4-pole ladder with its own envelope, an amp
// envelope, and portamento. Oscillators are PolyBLEP-corrected, so a saw at the
// top of the neck does not fold aliases back down into the bass register where
// they would beat against the real note.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include "BlockParams.h"
#include "DspCommon.h"
#include "PitchTrack.h"

namespace lowend {

//==============================================================================
/// Band-limited oscillator. `shape`: 0 saw, 1 square, 2 triangle, 3 narrow pulse.
struct BlepOsc
{
    void setRate (float hz, double sr) noexcept
    {
        inc_ = (float) (juce::jlimit (0.01, sr * 0.45, (double) hz) / (sr > 0 ? sr : 48000.0));
    }
    void reset (float phase = 0.0f) noexcept { phase_ = phase; triState_ = 0.0f; }

    float next (int shape) noexcept
    {
        const float p = phase_;
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;

        switch (shape)
        {
            case 1: return square (p, 0.5f);
            case 3: return square (p, 0.12f);
            case 2:
            {
                // Triangle by integrating the band-limited square: no separate
                // correction needed, and the leaky integrator removes the DC the
                // integration would otherwise accumulate.
                const float sq = square (p, 0.5f);
                triState_ += 4.0f * inc_ * sq - 0.0002f * triState_;
                return juce::jlimit (-1.5f, 1.5f, triState_);
            }
            default:
            {
                float v = 2.0f * p - 1.0f;
                v -= polyBlep (p, inc_);
                return v;
            }
        }
    }

private:
    float square (float p, float width) noexcept
    {
        float v = p < width ? 1.0f : -1.0f;
        v += polyBlep (p, inc_);
        float q = p - width; if (q < 0.0f) q += 1.0f;
        v -= polyBlep (q, inc_);
        return v;
    }
    /// Two-sample polynomial correction around a discontinuity.
    static float polyBlep (float t, float dt) noexcept
    {
        if (dt <= 0.0f) return 0.0f;
        if (t < dt)            { const float x = t / dt - 1.0f; return -(x * x); }
        if (t > 1.0f - dt)     { const float x = (t - 1.0f) / dt + 1.0f; return x * x; }
        return 0.0f;
    }
    float phase_ = 0.0f, inc_ = 0.01f, triState_ = 0.0f;
};

//==============================================================================
struct Adsr
{
    void prepare (double sr) noexcept { sr_ = sr > 0 ? sr : 48000.0; }
    void setTimes (float aMs, float dMs, float sustain01, float rMs) noexcept
    {
        a_ = rate (aMs); d_ = rate (dMs); s_ = juce::jlimit (0.0f, 1.0f, sustain01); r_ = rate (rMs);
    }
    void noteOn() noexcept { stage_ = Stage::attack; }
    void noteOff() noexcept { if (stage_ != Stage::idle) stage_ = Stage::release; }
    void hardReset() noexcept { stage_ = Stage::idle; v_ = 0.0f; }
    bool active() const noexcept { return stage_ != Stage::idle || v_ > 1.0e-5f; }

    float next() noexcept
    {
        switch (stage_)
        {
            case Stage::attack:  v_ += a_; if (v_ >= 1.0f) { v_ = 1.0f; stage_ = Stage::decay; } break;
            case Stage::decay:   v_ -= d_; if (v_ <= s_)   { v_ = s_;   stage_ = Stage::sustain; } break;
            case Stage::sustain: v_ = s_; break;
            case Stage::release: v_ -= r_; if (v_ <= 0.0f) { v_ = 0.0f; stage_ = Stage::idle; } break;
            case Stage::idle:    v_ = 0.0f; break;
        }
        return v_;
    }
    float value() const noexcept { return v_; }

private:
    float rate (float ms) const noexcept { return 1.0f / juce::jmax (1.0f, (float) (sr_ * juce::jmax (0.5f, ms) * 0.001)); }
    enum class Stage { idle, attack, decay, sustain, release };
    double sr_ = 48000.0;
    Stage stage_ = Stage::idle;
    float v_ = 0.0f, a_ = 0.01f, d_ = 0.001f, s_ = 0.6f, r_ = 0.002f;
};

//==============================================================================
class BassSynth
{
public:
    /// Indices into `synthParams()`; verified in prepare.
    enum Idx { kOn = 0, kWave, kDetune, kSub, kCutoff, kReso, kEnvAmt, kAttack, kDecay,
               kSustain, kRelease, kGlide, kOctave, kLevel, kRoute };

    void prepare (double sr, int maxBlock)
    {
        sr_ = sr > 0 ? sr : 48000.0;
        juce::ignoreUnused (maxBlock);
        params.configure (synthParams());
        jassert (params.verify (kOn, "on") && params.verify (kRoute, "route"));
        filt_.prepare (sr_);
        ampEnv_.prepare (sr_);
        filtEnv_.prepare (sr_);
        dc_.prepare (sr_);
        reset();
    }
    void reset()
    {
        osc1_.reset (0.0f); osc2_.reset (0.37f); sub_.reset (0.0f);
        filt_.reset(); ampEnv_.hardReset(); filtEnv_.hardReset(); dc_.reset();
        note_ = targetNote_ = 45.0f; vel_ = 0.0f; gliding_ = false;
    }

    /// MESSAGE THREAD. Envelope times are NOT applied here — they are read on
    /// the audio thread at the top of `render`, so the message thread never
    /// writes into the envelopes while they are being stepped.
    void setParams (const ParamValues& v) { params.setAll (v); }

    bool enabled() const noexcept { return params.flag (kOn); }
    SynthRoute route() const noexcept { return (SynthRoute) juce::jlimit (0, 1, params.choice (kRoute)); }
    bool sounding() const noexcept { return ampEnv_.active(); }
    float currentNote() const noexcept { return note_; }

    ParamStore params;

    void handle (const NoteEvent& e) noexcept
    {
        const float octave_ = std::round (params[kOctave]);
        const float glideMs_ = params[kGlide];
        switch (e.kind)
        {
            case NoteEvent::Kind::on:
                targetNote_ = e.note + octave_ * 12.0f;
                vel_ = e.velocity;
                if (glideMs_ <= 0.0f || ! ampEnv_.active()) note_ = targetNote_;
                gliding_ = glideMs_ > 0.0f;
                ampEnv_.noteOn();
                filtEnv_.noteOn();
                break;
            case NoteEvent::Kind::off:
                ampEnv_.noteOff();
                filtEnv_.noteOff();
                break;
            case NoteEvent::Kind::retune:
                targetNote_ = e.note + octave_ * 12.0f;
                if (! gliding_) note_ = targetNote_;   // follow bends without a portamento lag
                break;
        }
    }

    void allNotesOff() noexcept { ampEnv_.noteOff(); filtEnv_.noteOff(); }
    void panic() noexcept { ampEnv_.hardReset(); filtEnv_.hardReset(); filt_.reset(); }

    /// Renders into `out` (mono, added). Returns the peak it wrote, for metering.
    float render (float* out, int n) noexcept
    {
        const int wave_ = juce::jlimit (0, 3, params.choice (kWave));
        const float detune_ = params[kDetune];
        const float subLevel_ = params[kSub] / 100.0f;
        const float cutoff_ = params[kCutoff];
        const float reso_ = params[kReso] / 100.0f;
        const float envAmt_ = params[kEnvAmt] / 100.0f;
        const float glideMs_ = params[kGlide];
        const float level_ = juce::Decibels::decibelsToGain (params[kLevel]);
        const float atk = params[kAttack], dec = params[kDecay];
        const float sus = params[kSustain] / 100.0f, rel = params[kRelease];
        ampEnv_.setTimes (atk, dec, sus, rel);
        filtEnv_.setTimes (atk * 0.7f, dec, sus * 0.5f, rel);

        if (! params.flag (kOn) || ! ampEnv_.active()) return 0.0f;
        const float glideCoef = glideMs_ > 0.0f
            ? (float) (1.0 - std::exp (-1.0 / (0.001 * glideMs_ * sr_))) : 1.0f;
        const float detuneRatio = std::pow (2.0f, detune_ / 1200.0f);
        filt_.setResonance (reso_);
        float peak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            note_ += glideCoef * (targetNote_ - note_);
            if (std::abs (targetNote_ - note_) < 0.001f) { note_ = targetNote_; gliding_ = false; }
            const float hz = hzFromNote (note_);
            osc1_.setRate (hz, sr_);
            osc2_.setRate (hz * detuneRatio, sr_);
            sub_.setRate (hz * 0.5f, sr_);

            const float e = ampEnv_.next();
            const float fe = filtEnv_.next();
            // Envelope amount is bipolar and scaled in OCTAVES, not hertz, so
            // the sweep sounds the same on a low E and on a high G.
            const float fc = cutoff_ * std::pow (2.0f, envAmt_ * 4.0f * fe) * (0.5f + 0.5f * vel_);
            filt_.setCutoff (fc);

            float s = 0.5f * (osc1_.next (wave_) + osc2_.next (wave_));
            s += sub_.next (1) * subLevel_;
            s = filt_.process (s * 0.6f) * e * (0.35f + 0.65f * vel_);
            s = dc_.process (s) * level_;
            out[i] += s;
            peak = juce::jmax (peak, std::abs (s));
        }
        return peak;
    }

private:
    double sr_ = 48000.0;
    BlepOsc osc1_, osc2_, sub_;
    LadderFilter filt_;
    Adsr ampEnv_, filtEnv_;
    DcBlocker dc_;

    bool gliding_ = false;
    float note_ = 45.0f, targetNote_ = 45.0f, vel_ = 0.0f;
};

} // namespace lowend
