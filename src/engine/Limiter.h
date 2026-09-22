// Limiter.h — master-bus safety limiter (CLIENT REVIEW ROUND 3, item D).
//
// JM-BUILD-SPEC.md §5.3 lists a master section but no limiter; the approved
// reference UI shows a LIMITER plate next to the master knob, and round 2
// shipped that plate wired to a parameter that did not exist, so it was dead.
// This adds the missing DSP as a master-bus SAFETY limiter (logged in
// DECISIONS-signature.md under "Client review round 3").
//
// Design: soft-knee brick wall at a -0.3 dBFS ceiling.
//   * lookahead delay (2 ms) so gain is already down when the peak arrives
//   * sliding-minimum of the target gain across the lookahead window
//     (monotonic deque, O(1) amortised) — the classic no-overshoot trick
//   * gain smoothed by a one-pole with a fast attack (tau = lookahead/3) and a
//     PROGRAM-DEPENDENT release: two release limbs (fast 60 ms / slow 550 ms)
//     whose minimum is used, so transients recover quickly while sustained
//     material releases slowly and never pumps
//   * a final ceiling stage catches the sub-0.5 dB residue the one-pole leaves,
//     so the output is a true brick wall
// Everything is per-sample and allocation-free; prepare() sizes the buffers.
//
// DSP STANDARDS PASS (guide §1.3) — the final ceiling stage used to be a hard
// `juce::jlimit`. A hard clip is a discontinuous derivative: the sharpest
// nonlinearity in the whole plugin, on the master bus, downstream of
// everything, generating harmonics that extend to infinity and therefore
// aliasing at whatever level it engaged. It is now `jm::SoftCeiling`:
//
//   * identity below 90 % of the ceiling, so nothing that was passing through
//     untouched before is coloured now — bit-exact below -1.2 dB of ceiling,
//     which is what keeps the plugin nulling against its own bypassed path,
//   * a C1-continuous tanh bend above that, so there is no derivative
//     discontinuity to radiate: its harmonics fall away as 1/n^3 where a hard
//     clip's fall away as 1/n^2,
//   * asymptotic to the ceiling, so |out| < ceiling STRICTLY — this is still a
//     brick wall, and the round-3 "no overshoot" assertions still hold.
//
// ADAA was tried here as well and REJECTED on measurement; the reasoning is in
// Antialias.h next to `SoftCeiling`. Short version: averaging over a sample
// period is a savage low-pass at the frequencies where aliasing matters, and
// the variant that avoids that is not bounded — which a brick wall must be.
//
// Latency is unchanged: this is a memoryless shaper, and the 96-sample
// lookahead that round 6 pinned down and reports via setLatencySamples() is
// untouched.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "Antialias.h"
#include <cmath>
#include <vector>

namespace jm {

class MasterLimiter
{
public:
    static constexpr float kCeilingDb = -0.3f;

    void prepare (double sampleRate, int /*maxBlock*/)
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        look_ = juce::jmax (8, (int) std::lround (sr_ * 0.002));       // 2 ms lookahead
        delay_.assign ((size_t) look_ * 2, 0.0f);                      // interleaved L/R
        win_.assign ((size_t) look_ + 1, 1.0f);
        dqIdx_.assign ((size_t) look_ + 1, 0);
        clip_.set (juce::Decibels::decibelsToGain (kCeilingDb), 0.90f);
        reset();

        const double attackTau = 0.002 / 3.0;                          // lookahead / 3
        aAtk_  = (float) std::exp (-1.0 / (attackTau * sr_));
        aRelF_ = (float) std::exp (-1.0 / (0.060 * sr_));               // fast limb  60 ms
        aRelS_ = (float) std::exp (-1.0 / (0.550 * sr_));               // slow limb 550 ms
    }

    void reset()
    {
        std::fill (delay_.begin(), delay_.end(), 0.0f);
        std::fill (win_.begin(), win_.end(), 1.0f);
        std::fill (dqIdx_.begin(), dqIdx_.end(), 0);
        
        dqHead_ = dqTail_ = dqCount_ = 0;
        t_ = 0;
        dPos_ = 0;
        winMin_ = 1.0f;
        gEnv_ = gFast_ = gSlow_ = 1.0f;
        gr_.store (0.0f);
    }

    /// Peak gain reduction of the last block, in dB (>= 0). UI/meter read.
    float gainReductionDb() const noexcept { return gr_.load(); }

    /// When false the delay line still runs (so toggling is click-free and the
    /// reported latency never changes) but no gain reduction or clamp applies.
    void setEnabled (bool e) noexcept { enabled_ = e; }
    int latencySamples() const noexcept { return look_; }

    void process (juce::AudioBuffer<float>& buffer)
    {
        const int n = buffer.getNumSamples();
        if (n == 0 || delay_.empty()) return;
        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;
        float worst = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];
            const float peak = juce::jmax (std::abs (inL), std::abs (inR));

            // ---- target gain with a soft knee (3 dB) around the ceiling
            float target = 1.0f;
            if (peak > 1.0e-9f)
            {
                const float overDb = juce::Decibels::gainToDecibels (peak) - kCeilingDb;
                const float knee = 3.0f;
                float redDb = 0.0f;
                if (overDb >= knee * 0.5f)            redDb = overDb;
                else if (overDb > -knee * 0.5f)       redDb = (overDb + knee * 0.5f) * (overDb + knee * 0.5f)
                                                              / (2.0f * knee);
                if (redDb > 0.0f) target = juce::Decibels::decibelsToGain (-redDb);
            }

            // ---- sliding minimum of `target` over the lookahead window
            pushWindow (target);
            const float wMin = winMin_;

            // ---- attack / program-dependent release envelope
            if (wMin < gEnv_)
            {
                gEnv_  = wMin + (gEnv_  - wMin) * aAtk_;
                gFast_ = gEnv_;
                gSlow_ = gEnv_;
            }
            else
            {
                gFast_ = wMin + (gFast_ - wMin) * aRelF_;
                gSlow_ = wMin + (gSlow_ - wMin) * aRelS_;
                gEnv_  = juce::jmin (gFast_, gSlow_);
            }
            if (! std::isfinite (gEnv_)) gEnv_ = gFast_ = gSlow_ = 1.0f;
            gEnv_ = juce::jlimit (1.0e-4f, 1.0f, gEnv_);
            worst = juce::jmin (worst, gEnv_);

            // ---- lookahead delay, gain, brick-wall clamp
            const size_t d = (size_t) dPos_ * 2;
            const float dL = delay_[d], dR = delay_[d + 1];
            delay_[d] = inL;
            delay_[d + 1] = inR;
            dPos_ = (dPos_ + 1) % look_;

            if (enabled_)
            {
                L[i] = clip_.shape (dL * gEnv_);
                R[i] = clip_.shape (dR * gEnv_);
            }
            else
            {
                L[i] = dL;
                R[i] = dR;
            }
        }
        gr_.store (enabled_ ? -juce::Decibels::gainToDecibels (worst) : 0.0f);
    }

private:
    /// Monotonic-increasing deque over the last `look_` targets; the head holds
    /// the window minimum. Indices are a free-running sample counter, so no
    /// modular-arithmetic corner cases; storage is ring-buffered.
    void pushWindow (float v)
    {
        const int cap = look_ + 1;
        auto valueAt = [this, cap] (juce::int64 idx) { return win_[(size_t) (idx % cap)]; };
        win_[(size_t) (t_ % cap)] = v;
        while (dqCount_ > 0 && valueAt (dqIdx_[(size_t) ((dqTail_ - 1 + cap) % cap)]) >= v)
        {
            dqTail_ = (dqTail_ - 1 + cap) % cap;
            --dqCount_;
        }
        dqIdx_[(size_t) dqTail_] = t_;
        dqTail_ = (dqTail_ + 1) % cap;
        ++dqCount_;
        while (dqCount_ > 0 && dqIdx_[(size_t) dqHead_] <= t_ - look_)
        {
            dqHead_ = (dqHead_ + 1) % cap;
            --dqCount_;
        }
        winMin_ = valueAt (dqIdx_[(size_t) dqHead_]);
        ++t_;
    }

    double sr_ = 48000.0;
    int look_ = 96;
    std::vector<float> delay_, win_;
    std::vector<juce::int64> dqIdx_;
    juce::int64 t_ = 0;
    float winMin_ = 1.0f;
    int dqHead_ = 0, dqTail_ = 0, dqCount_ = 0, dPos_ = 0;
    float gEnv_ = 1.0f, gFast_ = 1.0f, gSlow_ = 1.0f;
    float aAtk_ = 0.0f, aRelF_ = 0.0f, aRelS_ = 0.0f;
    /// DSP standards pass: the brick wall, antiderivative-antialiased.
    SoftCeiling clip_;
    bool enabled_ = true;
    std::atomic<float> gr_ { 0.0f };
};

} // namespace jm
