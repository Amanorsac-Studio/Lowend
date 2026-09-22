// AmpDsp.h — the four amp models and the cabinet stage.
//
// Chain inside the amp:
//     input filter -> preamp gain stages -> tone stack -> power amp (sag, bias,
//     presence) -> master
// and then the cab, which is either an analytic speaker+mic model or a user IR.
//
// ON THE TONE STACK. A real Fender-style Bass/Mid/Treble network is one passive
// RC section, so its three controls INTERACT: turning Mid down lifts the
// apparent Bass and Treble and cuts a notch around 400 Hz, and that notch is the
// sound people mean when they say "Bassman". This is modelled as a voiced,
// coupled filter network (a low shelf, a mid peak and a high shelf whose gains,
// corners and Q are functions of all three controls) rather than as a
// component-level simulation of the actual RC ladder. The coupling is the part
// that matters musically and it is testable: `tests/LowEndTests.cpp` asserts the
// scoop appears when Mid goes to zero and that the corners move the right way.
// A SPICE-accurate stack was not worth the extra order and the extra risk of a
// silently wrong coefficient. See DECISIONS-lowend.md.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "BlockParams.h"
#include "DspCommon.h"

namespace lowend {

//==============================================================================
/// Coupled three-band passive-style tone stack. All positions are 0..1.
class ToneStack
{
public:
    void prepare (double sr) { sr_ = sr; reset(); }
    void reset() { lo_.reset(); mid_.reset(); hi_.reset(); }

    /// `midFreqHz` only applies to the models with a sweepable mid; the others
    /// pass their fixed centre.
    void setControls (float bass, float mid, float treble, float midFreqHz, bool deep, bool bright)
    {
        bass = juce::jlimit (0.0f, 1.0f, bass);
        mid = juce::jlimit (0.0f, 1.0f, mid);
        treble = juce::jlimit (0.0f, 1.0f, treble);

        // Mid at noon is unity; at zero it is a 15 dB scoop. The scoop also
        // WIDENS as it deepens (Q falls), which is why a scooped stack sounds
        // hollow rather than notched.
        const float midDb = (mid - 0.5f) * 2.0f * (mid < 0.5f ? 15.0f : 8.0f);
        const float midQ = 0.5f + mid * 0.9f;

        // The coupling: as the mid is scooped, the shelves gain reach and their
        // corners move outward, exactly as they do in the passive network.
        const float scoop = 1.0f - mid;                     // 0 at full mid, 1 at zero
        const float bassDb = (bass - 0.5f) * 2.0f * 12.0f + scoop * 5.0f;
        const float trebDb = (treble - 0.5f) * 2.0f * 12.0f + scoop * 4.0f;
        // The shelf CORNERS stay put; it is the notch between them that opens up.
        // (An earlier version walked the bass corner down as the mid was cut,
        // which cancelled most of the lift at 60 Hz — the scoop was measurable
        // in the mids and inaudible in the lows, which is the wrong sound.)
        const float bassHz = deep ? 60.0f : 90.0f;
        const float trebHz = (bright ? 1600.0f : 2400.0f) * (1.0f + 0.35f * scoop);

        lo_.setLowShelf (sr_, bassHz, 0.7f, bassDb);
        mid_.setPeak (sr_, juce::jlimit (150.0f, 3000.0f, midFreqHz), midQ, midDb);
        hi_.setHighShelf (sr_, trebHz, 0.7f, trebDb);

        // A passive stack always loses level; keeping that loss (and making it
        // up after the stack) is what makes the interaction audible instead of
        // being normalised away.
        makeup_ = juce::Decibels::decibelsToGain (-0.35f * (bassDb + trebDb) * 0.25f);
    }

    float process (float x) noexcept { return hi_.process (mid_.process (lo_.process (x))) * makeup_; }

private:
    double sr_ = 48000.0;
    Biquad lo_, mid_, hi_;
    float makeup_ = 1.0f;
};

//==============================================================================
/// Power amp: supply sag, bias shift and the negative-feedback presence control.
///
/// Sag is the reason a tube amp "breathes": a loud passage pulls the supply
/// down, the stage loses headroom for a moment, and the note blooms back as the
/// supply recovers. It is modelled as an envelope-driven gain and headroom
/// reduction with a fast pull-down and a slow (300 ms) recovery.
class PowerAmp
{
public:
    void prepare (double sr)
    {
        sr_ = sr;
        env_.prepare (sr);
        env_.setTimes (12.0f, 320.0f);
        for (auto& d : dc_) d.prepare (sr);
        reset();
    }
    void reset()
    {
        env_.reset();
        supply_ = 1.0f;
        for (auto& s : shaper_) s.reset();
        for (auto& f : pres_) f.reset();
        for (auto& d : dc_) d.reset();
    }

    void setParams (float sag01, float presence01, bool tube)
    {
        sag_ = juce::jlimit (0.0f, 1.0f, sag01);
        presDb_ = (juce::jlimit (0.0f, 1.0f, presence01) - 0.4f) * 10.0f;
        tube_ = tube;
    }

    /// `detector` is the mono sum; both channels share one supply, as they would.
    float supply() const noexcept { return supply_; }

    void process (juce::AudioBuffer<float>& b)
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        for (int ch = 0; ch < nCh; ++ch) pres_[(size_t) ch].setHighShelf (sr_, 3000.0f, 0.7f, presDb_);

        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float det = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float e = env_.process (det);
            // Supply falls toward 1 - sag*e and recovers with the envelope's own
            // release, so the recovery time is a property of the amp, not of the
            // note. Floor at 0.35 so the amp never chokes to silence.
            supply_ = juce::jmax (0.35f, 1.0f - sag_ * juce::jmin (1.0f, e * 1.6f));
            const float headroom = tube_ ? supply_ : 0.6f + 0.4f * supply_;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = b.getWritePointer (ch);
                float x = s[i] / juce::jmax (0.2f, headroom);
                x = tube_ ? shaper_[(size_t) ch].process (x)                 // soft, ADAA
                          : juce::jlimit (-1.6f, 1.6f, x - 0.06f * x * x * x); // solid state: stiffer, later
                s[i] = dc_[(size_t) ch].process (pres_[(size_t) ch].process (x * headroom));
            }
        }
    }

private:
    double sr_ = 48000.0;
    EnvFollower env_;
    std::array<AdaaTanh, 2> shaper_;
    std::array<Biquad, 2> pres_;
    std::array<DcBlocker, 2> dc_;
    float sag_ = 0.3f, presDb_ = 0.0f, supply_ = 1.0f;
    bool tube_ = false;
};

//==============================================================================
/// The amp head. One object; the model selects gain structure, tone-stack
/// voicing and power-amp behaviour.
class AmpHead
{
public:
    /// Parameter indices — MUST match the order of `ampParams()`. `prepare`
    /// asserts this in debug builds, so reordering the registry fails loudly
    /// rather than silently turning Gain into Bass.
    enum Idx { kModel = 0, kGain, kBass, kMid, kMidFreq, kTreble, kPresence, kContour,
               kBright, kDeep, kUltraLo, kUltraHi, kSag, kMaster };

    void prepare (double sr, int maxBlock)
    {
        sr_ = sr; maxBlock_ = juce::jmax (1, maxBlock);
        params.configure (ampParams());
        jassert (params.verify (kGain, "gain") && params.verify (kMaster, "master")
                 && params.verify (kUltraHi, "ultraHi"));
        stack_.prepare (sr);
        power_.prepare (sr);
        for (auto& f : inHp_) f.setHighPass (sr, 25.0f, 0.707f);
        for (auto& f : contour_) f.reset();
        for (auto& d : dc_) d.prepare (sr);
        os_ = std::make_unique<juce::dsp::Oversampling<float>> (
                  2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os_->initProcessing ((size_t) maxBlock_);
        reset();
    }
    void reset()
    {
        stack_.reset(); power_.reset();
        for (auto& f : inHp_) f.reset();
        for (auto& f : contour_) f.reset();
        for (auto& f : ultraLo_) f.reset();
        for (auto& f : ultraHi_) f.reset();
        for (auto& s : pre_) s.reset();
        for (auto& d : dc_) d.reset();
        if (os_) os_->reset();
    }
    int latencySamples() const { return os_ ? (int) os_->getLatencyInSamples() : 0; }

    /// MESSAGE THREAD.
    void setParams (const ParamValues& v) { params.setAll (v); }

    void process (juce::AudioBuffer<float>& b)
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;

        // One read of each parameter per block: the values cannot change under
        // the loops below, and the audio thread never touches the model.
        const auto model_ = (AmpModel) juce::jlimit (0, kNumAmpModels - 1, params.choice (kModel));
        const float gain_ = params[kGain] / 100.0f;
        const float bass_ = params[kBass] / 100.0f;
        const float mid_ = params[kMid] / 100.0f;
        const float midFreq_ = params[kMidFreq];
        const float treble_ = params[kTreble] / 100.0f;
        const float presence_ = params[kPresence] / 100.0f;
        const float contourAmt_ = params[kContour] / 100.0f;
        const bool bright_ = params.flag (kBright);
        const bool deep_ = params.flag (kDeep);
        const bool ultraLoOn_ = params.flag (kUltraLo);
        const bool ultraHiOn_ = params.flag (kUltraHi);
        const float sag_ = params[kSag] / 100.0f;
        const float master_ = params[kMaster] / 100.0f;
        modelSeen_.store ((int) model_, std::memory_order_relaxed);

        // ---- per-model gain structure
        struct Voicing { float preDb; float stages; bool tube; float fixedMidHz; };
        const Voicing voice[kNumAmpModels] = {
            { 26.0f, 1.0f, false, 600.0f },    // Studio Clean: lots of clean headroom
            { 34.0f, 2.0f, true,  500.0f },    // Classic Tube: two stages, breaks up
            { 38.0f, 2.0f, true,  700.0f },    // Flip Top: earliest breakup
            { 30.0f, 1.0f, false, 800.0f } };  // Monolith: hi-fi, tight
        const auto& vo = voice[(int) model_];
        const bool sweepMid = (model_ == AmpModel::rumbleClean || model_ == AmpModel::monolith);
        const float midHz = sweepMid ? midFreq_ : vo.fixedMidHz;
        const float pre = juce::Decibels::decibelsToGain (gain_ * vo.preDb);
        const float out = juce::Decibels::decibelsToGain ((master_ - 0.7f) * 30.0f)
                        / std::sqrt (juce::jmax (1.0f, pre * 0.25f));

        stack_.setControls (bass_, model_ == AmpModel::flipTop ? 0.5f : mid_, treble_, midHz, deep_, bright_);
        power_.setParams (sag_, presence_, vo.tube);

        // ---- input filter + preamp gain
        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& hp = inHp_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = hp.process (s[i]) * pre;
        }

        // ---- preamp saturation, oversampled
        juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), (size_t) nCh, 0, (size_t) n);
        const int stages = (int) vo.stages;
        if (os_ != nullptr && n <= maxBlock_)
        {
            auto up = os_->processSamplesUp (blk);
            const int un = (int) up.getNumSamples();
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d = up.getChannelPointer ((size_t) ch);
                auto& sh = pre_[(size_t) ch];
                for (int i = 0; i < un; ++i)
                {
                    float x = sh.process (d[i]);
                    if (stages > 1) x = std::tanh (x * 1.3f);   // second stage, already band-limited
                    d[i] = x;
                }
            }
            os_->processSamplesDown (blk);
        }
        else
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = b.getWritePointer (ch);
                auto& sh = pre_[(size_t) ch];
                for (int i = 0; i < n; ++i) s[i] = sh.process (s[i]);
            }
        }

        // ---- tone stack, contour / ultra switches
        for (int ch = 0; ch < nCh; ++ch)
        {
            contour_[(size_t) ch].setPeak (sr_, 500.0f, 0.8f, -contourAmt_ * 12.0f);
            ultraLo_[(size_t) ch].setLowShelf (sr_, 70.0f, 0.7f, ultraLoOn_ ? 6.0f : 0.0f);
            ultraHi_[(size_t) ch].setHighShelf (sr_, 4000.0f, 0.7f, ultraHiOn_ ? 6.0f : 0.0f);
        }
        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& co = contour_[(size_t) ch];
            auto& ul = ultraLo_[(size_t) ch];
            auto& uh = ultraHi_[(size_t) ch];
            for (int i = 0; i < n; ++i)
                s[i] = uh.process (ul.process (co.process (stack_.process (s[i]))));
        }

        // ---- power amp + master
        power_.process (b);
        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& dc = dc_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = dc.process (s[i]) * out;
        }
    }

    AmpModel model() const { return (AmpModel) modelSeen_.load (std::memory_order_relaxed); }
    float supply() const { return power_.supply(); }

    ParamStore params;

private:
    double sr_ = 48000.0; int maxBlock_ = 512;
    ToneStack stack_;
    PowerAmp power_;
    std::array<Biquad, 2> inHp_, contour_, ultraLo_, ultraHi_;
    std::array<AdaaTanh, 2> pre_;
    std::array<DcBlocker, 2> dc_;
    std::unique_ptr<juce::dsp::Oversampling<float>> os_;
    std::atomic<int> modelSeen_ { 0 };
};

//==============================================================================
/// Cabinet. Either an analytic speaker+mic model (ships with the app, starts
/// instantly, no audio data) or a user impulse response convolved with JUCE's
/// partitioned FFT convolution.
///
/// The analytic model is four things a real bass cab does:
///   * a resonant low corner — the port/box tuning, a 2nd-order high-pass with
///     a peak just above it, which is where the "thump" of a 15 comes from;
///   * cone breakup — a broad peak in the upper mids that gets higher and
///     smaller as the driver gets smaller (a 10 breaks up above a 15);
///   * a steep top rolloff — a paper cone simply stops above 4-5 kHz;
///   * mic colour and placement — proximity lift close in, top loss off axis.
class CabSim
{
public:
    /// Indices into `cabParams()`; verified in prepare.
    enum Idx { kModel = 0, kMic, kDistance, kAxis, kLowCut, kHighCut, kIr, kLevel };

    void prepare (double sr, int maxBlock)
    {
        sr_ = sr; maxBlock_ = juce::jmax (1, maxBlock);
        params.configure (cabParams());
        jassert (params.verify (kModel, "model") && params.verify (kLevel, "level"));
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlock_, 2 };
        conv_.prepare (spec);
        reset();
    }
    void reset()
    {
        for (auto& f : hp_) f.reset();
        for (auto& f : res_) f.reset();
        for (auto& f : breakup_) f.reset();
        for (auto& f : lp1_) f.reset();
        for (auto& f : lp2_) f.reset();
        for (auto& f : mic_) f.reset();
        for (auto& f : userLo_) f.reset();
        for (auto& f : userHi_) f.reset();
        conv_.reset();
    }
    int latencySamples() const { return irLoaded_.load() ? (int) conv_.getLatency() : 0; }

    /// MESSAGE THREAD. Hands the IR to the convolution engine, which loads it on
    /// its own background thread; `irLoaded_` flips when it is safe to use.
    void loadIr (const juce::File& f)
    {
        if (! f.existsAsFile()) { irLoaded_.store (false); return; }
        conv_.loadImpulseResponse (f, juce::dsp::Convolution::Stereo::yes,
                                   juce::dsp::Convolution::Trim::yes, 0,
                                   juce::dsp::Convolution::Normalise::yes);
        irLoaded_.store (true);
    }
    void clearIr() { irLoaded_.store (false); }
    bool hasIr() const { return irLoaded_.load(); }

    /// MESSAGE THREAD.
    void setParams (const ParamValues& v) { params.setAll (v); }

    void process (juce::AudioBuffer<float>& b)
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;

        const auto model_ = (CabModel) juce::jlimit (0, (int) CabModel::Count - 1, params.choice (kModel));
        const int mic_type_ = juce::jlimit (0, 3, params.choice (kMic));
        const float distance_ = params[kDistance] / 100.0f;
        const float axis_ = params[kAxis] / 100.0f;
        const float lowCut_ = params[kLowCut];
        const float highCut_ = params[kHighCut];
        const float level_ = juce::Decibels::decibelsToGain (params[kLevel]);
        modelSeen_.store ((int) model_, std::memory_order_relaxed);

        if (model_ == CabModel::userIr && irLoaded_.load())
        {
            juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), (size_t) nCh, 0, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            conv_.process (ctx);
            for (int ch = 0; ch < nCh; ++ch)
            {
                userLo_[(size_t) ch].setHighPass (sr_, lowCut_, 0.707f);
                userHi_[(size_t) ch].setLowPass (sr_, highCut_, 0.707f);
                auto* s = b.getWritePointer (ch);
                auto& lo = userLo_[(size_t) ch]; auto& hi = userHi_[(size_t) ch];
                for (int i = 0; i < n; ++i) s[i] = hi.process (lo.process (s[i])) * level_;
            }
            return;
        }
        if (model_ == CabModel::di)
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                userLo_[(size_t) ch].setHighPass (sr_, lowCut_, 0.707f);
                auto* s = b.getWritePointer (ch);
                auto& lo = userLo_[(size_t) ch];
                for (int i = 0; i < n; ++i) s[i] = lo.process (s[i]) * level_;
            }
            return;
        }

        //                       1x15   2x10   4x10   8x10
        static constexpr float fLow[4]      = { 55.0f, 72.0f, 62.0f, 48.0f };
        static constexpr float resQ[4]      = { 1.5f,  1.1f,  1.3f,  1.7f  };
        static constexpr float fBreak[4]    = { 1300.0f, 2200.0f, 1900.0f, 1100.0f };
        static constexpr float breakDb[4]   = { 4.5f,  6.0f,  5.5f,  3.5f  };
        static constexpr float fTop[4]      = { 3200.0f, 4800.0f, 4200.0f, 2800.0f };
        const int m = juce::jlimit (0, 3, (int) model_);

        // Placement. Close to the cone: proximity lift and more top. Off axis:
        // the top goes first, the lows stay.
        const float proximityDb = (1.0f - distance_) * 5.0f;
        const float topScale = (1.0f - axis_ * 0.55f) * (1.0f - distance_ * 0.25f);
        //                                  dyn    cond    ribbon   di
        static constexpr float micPresDb[4] = { 3.0f, 1.5f, -2.5f, 0.0f };
        static constexpr float micTop[4]    = { 1.0f, 1.35f, 0.7f, 2.0f };

        const float topHz = juce::jlimit (800.0f, (float) (sr_ * 0.45),
                                          fTop[m] * topScale * micTop[juce::jlimit (0, 3, mic_type_)]);

        for (int ch = 0; ch < nCh; ++ch)
        {
            hp_[(size_t) ch].setHighPass (sr_, juce::jmax (lowCut_, fLow[m] * 0.8f), 0.707f);
            res_[(size_t) ch].setPeak (sr_, fLow[m] * 1.15f, resQ[m], 3.0f + proximityDb);
            breakup_[(size_t) ch].setPeak (sr_, fBreak[m], 1.4f, breakDb[m]);
            lp1_[(size_t) ch].setLowPass (sr_, topHz, 0.54f);      // two cascaded 2nd-order
            lp2_[(size_t) ch].setLowPass (sr_, topHz, 1.31f);      // = Butterworth 4th order
            mic_[(size_t) ch].setHighShelf (sr_, 3500.0f, 0.7f, micPresDb[juce::jlimit (0, 3, mic_type_)]);
        }

        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& hp = hp_[(size_t) ch]; auto& rs = res_[(size_t) ch]; auto& bk = breakup_[(size_t) ch];
            auto& l1 = lp1_[(size_t) ch]; auto& l2 = lp2_[(size_t) ch]; auto& mc = mic_[(size_t) ch];
            for (int i = 0; i < n; ++i)
                s[i] = mc.process (l2.process (l1.process (bk.process (rs.process (hp.process (s[i])))))) * level_;
        }
    }

    CabModel model() const { return (CabModel) modelSeen_.load (std::memory_order_relaxed); }

    ParamStore params;

private:
    double sr_ = 48000.0; int maxBlock_ = 512;
    std::array<Biquad, 2> hp_, res_, breakup_, lp1_, lp2_, mic_, userLo_, userHi_;
    juce::dsp::Convolution conv_;
    std::atomic<bool> irLoaded_ { false };
    std::atomic<int> modelSeen_ { (int) CabModel::c410 };
};

} // namespace lowend
