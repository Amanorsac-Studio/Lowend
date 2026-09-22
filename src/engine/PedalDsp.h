// PedalDsp.h — the pedal blocks and the chain that holds them.
//
// A `Block` is one pedal. Blocks that Low End owns (drives, dynamics, filter and
// pitch, gate) are implemented here; blocks marked `reuse` in BlockParams.h are
// thin adapters over the FX units already built for Sanctuary (`FxDsp.h`), so
// the chorus in Low End IS the chorus in Sanctuary.
//
// Threading contract, identical to `jm::FxRackDsp`: the message thread mutates
// layout under a spin lock and writes parameters through relaxed atomics; the
// audio thread takes a TRY-lock and passes the block dry if it loses the race,
// which happens only during a structural edit and lasts one buffer.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "BlockParams.h"
#include "DspCommon.h"

namespace lowend {

//==============================================================================
class Block
{
public:
    explicit Block (BlockType t) : spec_ (blockSpec (t))
    {
        for (size_t i = 0; i < spec_.params.size() && i < values_.size(); ++i)
            values_[i].store (spec_.params[i].def);
    }
    virtual ~Block() = default;

    const BlockSpec& spec() const { return spec_; }
    BlockType type() const { return spec_.type; }

    int paramIndex (const std::string& id) const
    {
        for (size_t i = 0; i < spec_.params.size(); ++i)
            if (id == spec_.params[i].id) return (int) i;
        return -1;
    }
    void setParam (const std::string& id, float v)
    {
        setParamIndexed (paramIndex (id), v);
    }
    void setParamIndexed (int i, float v)
    {
        if (i < 0 || i >= (int) spec_.params.size() || i >= (int) values_.size()) return;
        const auto& p = spec_.params[(size_t) i];
        const float clamped = juce::jlimit (p.min, p.max, v);
        values_[(size_t) i].store (clamped, std::memory_order_relaxed);
        onParamChanged (i, clamped);
    }
    float getParam (int i) const
    {
        return (i >= 0 && i < (int) values_.size()) ? values_[(size_t) i].load (std::memory_order_relaxed) : 0.0f;
    }

    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() = 0;
    virtual void process (juce::AudioBuffer<float>& buffer, double bpm) = 0;
    /// Latency this block adds at the host rate (oversamplers, lookahead).
    virtual int latencySamples() const { return 0; }

protected:
    float p (int i) const { return values_[(size_t) i].load (std::memory_order_relaxed); }
    /// Hook for adapters that must forward the write somewhere else.
    virtual void onParamChanged (int, float) {}

    const BlockSpec& spec_;
    std::array<std::atomic<float>, 24> values_ {};
};

//==============================================================================
/// Adapter: a Sanctuary FX unit presented as a Low End block. The parameter list
/// is the FX unit's own (BlockParams.h copies it), so index i here is index i
/// there and the forward is a plain pass-through.
class ReusedBlock final : public Block
{
public:
    ReusedBlock (BlockType t, jm::FxType fx) : Block (t), unit_ (jm::createFxUnit (fx))
    {
        if (unit_)
            for (size_t i = 0; i < spec_.params.size(); ++i)
                unit_->setParamIndexed ((int) i, spec_.params[i].def);
    }
    void prepare (double sr, int maxBlock) override { if (unit_) unit_->prepare (sr, maxBlock); }
    void reset() override { if (unit_) unit_->reset(); }
    void process (juce::AudioBuffer<float>& b, double bpm) override { if (unit_) unit_->process (b, bpm); }

protected:
    void onParamChanged (int i, float v) override { if (unit_) unit_->setParamIndexed (i, v); }

private:
    std::unique_ptr<jm::FxUnit> unit_;
};

//==============================================================================
/// Noise gate. Detector is the mono sum so the two channels never gate apart.
class NoiseGateBlock final : public Block
{
public:
    NoiseGateBlock() : Block (BlockType::noiseGate) {}
    void prepare (double sr, int) override { gate_.prepare (sr); }
    void reset() override { gate_.reset(); }
    void process (juce::AudioBuffer<float>& b, double) override
    {
        gate_.setParams (p (0), p (1), p (2) / 100.0f);
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float det = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float g = gate_.nextGain (det);
            l[i] *= g;
            if (r) r[i] *= g;
        }
    }
private:
    NoiseGate gate_;
};

//==============================================================================
/// Modern Bass OD. The clean path is what separates a bass drive from a guitar
/// drive: the fundamental of a low B is 31 Hz, and a waveshaper turns that into
/// harmonics and takes the floor out of the sound. So the drive path is
/// high-passed at 120 Hz BEFORE the shaper, the clean path is untouched, and
/// `blend` sets how much of each you hear. At blend = 0 the pedal is a wire
/// with a level knob.
class BassOdBlock final : public Block
{
public:
    BassOdBlock() : Block (BlockType::bassOd) {}
    void prepare (double sr, int maxBlock) override
    {
        sr_ = sr; maxBlock_ = juce::jmax (1, maxBlock);
        for (auto& f : hp_) f.setHighPass (sr, 120.0f, 0.707f);
        for (auto& d : dc_) d.prepare (sr);
        wet_.setSize (2, maxBlock_);
        os_ = std::make_unique<juce::dsp::Oversampling<float>> (
                  2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os_->initProcessing ((size_t) maxBlock_);
        reset();
    }
    void reset() override
    {
        for (auto& f : hp_) f.reset();
        for (auto& f : tone_) f.reset();
        for (auto& f : bass_) f.reset();
        for (auto& f : treb_) f.reset();
        for (auto& s : shaper_) s.reset();
        for (auto& d : dc_) d.reset();
        if (os_) os_->reset();
        wet_.clear();
    }
    int latencySamples() const override { return os_ ? (int) os_->getLatencyInSamples() : 0; }

    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float drive = p (0) / 100.0f;
        const float tone = p (1) / 100.0f;
        const float blend = p (2) / 100.0f;
        const float level = juce::Decibels::decibelsToGain ((p (5) / 100.0f - 0.5f) * 24.0f);
        const float pre = juce::Decibels::decibelsToGain (6.0f + drive * 30.0f);
        // Level-match the driven path so the blend knob is a blend and not a
        // volume knob: a tanh at `pre` raises perceived loudness roughly with
        // sqrt(pre) once it is well into compression.
        const float post = 1.0f / std::sqrt (juce::jmax (1.0f, pre));

        if (wet_.getNumSamples() < n || wet_.getNumChannels() < nCh) wet_.setSize (nCh, n, false, false, true);
        for (int ch = 0; ch < nCh; ++ch) wet_.copyFrom (ch, 0, b, ch, 0, n);

        for (int ch = 0; ch < nCh; ++ch)                         // high-pass + input gain
        {
            auto* s = wet_.getWritePointer (ch);
            auto& hp = hp_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = hp.process (s[i]) * pre;
        }

        juce::dsp::AudioBlock<float> blk (wet_.getArrayOfWritePointers(), (size_t) nCh, 0, (size_t) n);
        if (os_ != nullptr && n <= maxBlock_)
        {
            auto up = os_->processSamplesUp (blk);
            const int un = (int) up.getNumSamples();
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d = up.getChannelPointer ((size_t) ch);
                auto& sh = shaper_[(size_t) ch];
                for (int i = 0; i < un; ++i) d[i] = sh.process (d[i]);
            }
            os_->processSamplesDown (blk);
        }
        else
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = wet_.getWritePointer (ch);
                auto& sh = shaper_[(size_t) ch];
                for (int i = 0; i < n; ++i) s[i] = sh.process (s[i]);
            }
        }

        const float toneHz = 700.0f * std::pow (10.0f, tone * 1.1f);        // 700 Hz .. 8.8 kHz
        for (int ch = 0; ch < nCh; ++ch)
        {
            tone_[(size_t) ch].setLowPass (sr_, toneHz, 0.6f);
            bass_[(size_t) ch].setLowShelf (sr_, 120.0f, 0.7f, p (3));
            treb_[(size_t) ch].setHighShelf (sr_, 2500.0f, 0.7f, p (4));
        }

        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* dry = b.getWritePointer (ch);
            const auto* w = wet_.getReadPointer (ch);
            auto& tf = tone_[(size_t) ch]; auto& bf = bass_[(size_t) ch]; auto& tr = treb_[(size_t) ch];
            auto& dc = dc_[(size_t) ch];
            for (int i = 0; i < n; ++i)
            {
                const float driven = dc.process (tf.process (w[i]) * post);
                const float mixed = dry[i] * (1.0f - blend) + driven * blend;
                dry[i] = tr.process (bf.process (mixed)) * level;
            }
        }
    }
private:
    double sr_ = 48000; int maxBlock_ = 512;
    std::array<Biquad, 2> hp_, tone_, bass_, treb_;
    std::array<AdaaTanh, 2> shaper_;
    std::array<DcBlocker, 2> dc_;
    std::unique_ptr<juce::dsp::Oversampling<float>> os_;
    juce::AudioBuffer<float> wet_;
};

//==============================================================================
/// Tube OD — two cascaded asymmetric stages. Asymmetry is what makes a valve
/// stage sound different from a symmetric diode clipper: it makes EVEN harmonics
/// (a warm, thick second) instead of only odd ones, and `bias` sets how far the
/// operating point sits from the middle of the curve.
class TubeOdBlock final : public Block
{
public:
    TubeOdBlock() : Block (BlockType::tubeOd) {}
    void prepare (double sr, int maxBlock) override
    {
        sr_ = sr; maxBlock_ = juce::jmax (1, maxBlock);
        for (auto& f : coupling_) f.setHighPass (sr, 30.0f, 0.707f);
        for (auto& d : dc_) d.prepare (sr);
        os_ = std::make_unique<juce::dsp::Oversampling<float>> (
                  2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os_->initProcessing ((size_t) maxBlock_);
        reset();
    }
    void reset() override
    {
        for (auto& f : coupling_) f.reset();
        for (auto& f : tone_) f.reset();
        for (auto& d : dc_) d.reset();
        if (os_) os_->reset();
    }
    int latencySamples() const override { return os_ ? (int) os_->getLatencyInSamples() : 0; }

    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float drive = p (0) / 100.0f;
        const int tube = (int) p (3);
        // 12AX7 has the most gain and the sharpest knee, 12AU7 the least,
        // 6L6 sits between and stays clean longest before it folds.
        const float tubeGain[3] = { 30.0f, 16.0f, 22.0f };
        const float tubeKnee[3] = { 1.0f, 0.65f, 0.8f };
        const float pre = juce::Decibels::decibelsToGain (4.0f + drive * tubeGain[juce::jlimit (0, 2, tube)]);
        const float knee = tubeKnee[juce::jlimit (0, 2, tube)];
        const float bias = (p (4) / 100.0f - 0.5f) * 0.6f;
        const float level = juce::Decibels::decibelsToGain ((p (2) / 100.0f - 0.5f) * 24.0f)
                          / std::sqrt (juce::jmax (1.0f, pre));
        const float toneHz = 600.0f * std::pow (10.0f, (p (1) / 100.0f) * 1.15f);

        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& cp = coupling_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = cp.process (s[i]) * pre;
        }

        juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), (size_t) nCh, 0, (size_t) n);
        if (os_ != nullptr && n <= maxBlock_)
        {
            auto up = os_->processSamplesUp (blk);
            const int un = (int) up.getNumSamples();
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d = up.getChannelPointer ((size_t) ch);
                for (int i = 0; i < un; ++i) d[i] = valve (d[i], bias, knee);
            }
            os_->processSamplesDown (blk);
        }
        else
        {
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = b.getWritePointer (ch);
                for (int i = 0; i < n; ++i) s[i] = valve (s[i], bias, knee);
            }
        }

        for (int ch = 0; ch < nCh; ++ch) tone_[(size_t) ch].setLowPass (sr_, toneHz, 0.6f);
        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& tf = tone_[(size_t) ch]; auto& dc = dc_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = dc.process (tf.process (s[i])) * level;
        }
    }
private:
    /// Asymmetric soft clip: the positive half compresses sooner than the
    /// negative one, which is the audible signature of a single-ended stage.
    static float valve (float x, float bias, float knee) noexcept
    {
        const float v = x + bias;
        const float up = std::tanh (v * knee);
        const float dn = std::tanh (v * knee * 0.7f) * 1.15f;
        return (v >= 0.0f ? up : dn) - std::tanh (bias * knee);
    }
    double sr_ = 48000; int maxBlock_ = 512;
    std::array<Biquad, 2> coupling_, tone_;
    std::array<DcBlocker, 2> dc_;
    std::unique_ptr<juce::dsp::Oversampling<float>> os_;
};

//==============================================================================
/// Green Box fuzz — hard, square, and gated. Fuzz on bass only works with a
/// blend, so this one has the same clean path as the OD.
class FuzzBlock final : public Block
{
public:
    FuzzBlock() : Block (BlockType::fuzz) {}
    void prepare (double sr, int maxBlock) override
    {
        sr_ = sr; maxBlock_ = juce::jmax (1, maxBlock);
        for (auto& d : dc_) d.prepare (sr);
        for (auto& f : hp_) f.setHighPass (sr, 80.0f, 0.707f);
        gateEnv_.prepare (sr);
        gateEnv_.setTimes (2.0f, 120.0f);
        wet_.setSize (2, maxBlock_);
        os_ = std::make_unique<juce::dsp::Oversampling<float>> (
                  2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        os_->initProcessing ((size_t) maxBlock_);
        reset();
    }
    void reset() override
    {
        for (auto& f : hp_) f.reset();
        for (auto& f : tone_) f.reset();
        for (auto& d : dc_) d.reset();
        gateEnv_.reset();
        if (os_) os_->reset();
        wet_.clear();
    }
    int latencySamples() const override { return os_ ? (int) os_->getLatencyInSamples() : 0; }

    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float fuzz = p (0) / 100.0f;
        const float toneHz = 500.0f * std::pow (10.0f, (p (1) / 100.0f) * 1.2f);
        const float level = juce::Decibels::decibelsToGain ((p (2) / 100.0f - 0.5f) * 24.0f) * 0.5f;
        const float gateThr = juce::Decibels::decibelsToGain (-60.0f + (p (3) / 100.0f) * 40.0f);
        const float blend = p (4) / 100.0f;
        const float pre = juce::Decibels::decibelsToGain (12.0f + fuzz * 40.0f);

        if (wet_.getNumSamples() < n || wet_.getNumChannels() < nCh) wet_.setSize (nCh, n, false, false, true);
        for (int ch = 0; ch < nCh; ++ch) wet_.copyFrom (ch, 0, b, ch, 0, n);
        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = wet_.getWritePointer (ch);
            auto& hp = hp_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = hp.process (s[i]) * pre;
        }

        juce::dsp::AudioBlock<float> blk (wet_.getArrayOfWritePointers(), (size_t) nCh, 0, (size_t) n);
        // 8x here, not 2x: a near-square wave is broadband by construction, and
        // the aliasing of a hard clip folds straight back into the bass register
        // where it sounds like a ring modulator rather than like fuzz.
        if (os_ != nullptr && n <= maxBlock_)
        {
            auto up = os_->processSamplesUp (blk);
            const int un = (int) up.getNumSamples();
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* d = up.getChannelPointer ((size_t) ch);
                for (int i = 0; i < un; ++i) d[i] = std::tanh (d[i] * 6.0f);
            }
            os_->processSamplesDown (blk);
        }

        for (int ch = 0; ch < nCh; ++ch) tone_[(size_t) ch].setLowPass (sr_, toneHz, 0.7f);
        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float det = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float g = gateEnv_.process (det) > gateThr ? 1.0f : 0.0f;
            gateGain_ += (g > gateGain_ ? 0.3f : 0.005f) * (g - gateGain_);
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* dry = b.getWritePointer (ch);
                const float w = dc_[(size_t) ch].process (tone_[(size_t) ch].process (wet_.getSample (ch, i)))
                              * level * gateGain_;
                dry[i] = dry[i] * (1.0f - blend) + w * blend;
            }
        }
    }
private:
    double sr_ = 48000; int maxBlock_ = 512;
    std::array<Biquad, 2> hp_, tone_;
    std::array<DcBlocker, 2> dc_;
    EnvFollower gateEnv_;
    float gateGain_ = 0.0f;
    std::unique_ptr<juce::dsp::Oversampling<float>> os_;
    juce::AudioBuffer<float> wet_;
};

//==============================================================================
/// Clean boost with a presence shelf and a low shelf. No shaper at all — its job
/// is to push the amp, and anything it adds itself gets in the way of that.
class BoostBlock final : public Block
{
public:
    BoostBlock() : Block (BlockType::boost) {}
    void prepare (double sr, int) override { sr_ = sr; reset(); }
    void reset() override { for (auto& f : hi_) f.reset(); for (auto& f : lo_) f.reset(); }
    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float g = juce::Decibels::decibelsToGain (p (0));
        const float presDb = (p (1) / 100.0f - 0.5f) * 12.0f;
        for (int ch = 0; ch < nCh; ++ch)
        {
            hi_[(size_t) ch].setHighShelf (sr_, 2000.0f, 0.7f, presDb);
            lo_[(size_t) ch].setLowShelf (sr_, 100.0f, 0.7f, p (2));
            auto* s = b.getWritePointer (ch);
            auto& h = hi_[(size_t) ch]; auto& l = lo_[(size_t) ch];
            for (int i = 0; i < n; ++i) s[i] = l.process (h.process (s[i])) * g;
        }
    }
private:
    double sr_ = 48000;
    std::array<Biquad, 2> hi_, lo_;
};

//==============================================================================
/// Studio Comp — opto. One knob (`sustain`) drives threshold and ratio together,
/// and the release is program-dependent: the more gain reduction is happening,
/// the slower it lets go, which is exactly what an optical cell does and why
/// these are the compressors bass players leave on all night.
class OptoCompBlock final : public Block
{
public:
    OptoCompBlock() : Block (BlockType::optoComp) {}
    void prepare (double sr, int) override
    {
        sr_ = sr;
        det_.prepare (sr);
        det_.setTimes (8.0f, 120.0f);
        reset();
    }
    void reset() override { det_.reset(); gr_ = 0.0f; for (auto& f : tone_) f.reset(); }
    float gainReductionDb() const { return gr_; }

    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float sustain = p (0) / 100.0f;
        const float thr = -6.0f - (1.0f - sustain) * 24.0f;      // -30 .. -6 dBFS
        const float ratio = 1.5f + sustain * 8.5f;               // 1.5:1 .. 10:1
        const float makeup = juce::Decibels::decibelsToGain (sustain * 12.0f
                             + (p (2) / 100.0f - 0.5f) * 18.0f);
        const float mix = p (3) / 100.0f;
        const float toneDb = (p (1) / 100.0f - 0.5f) * 8.0f;
        for (int ch = 0; ch < nCh; ++ch) tone_[(size_t) ch].setHighShelf (sr_, 2500.0f, 0.7f, toneDb);

        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float det = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float db = juce::Decibels::gainToDecibels (det_.process (det), -100.0f);
            const float over = juce::jmax (0.0f, db - thr);
            const float targetGr = -over * (1.0f - 1.0f / ratio);
            // Program-dependent release: 60 ms of "fast" plus up to 900 ms that
            // only engages when the cell is already dark.
            const float rel = 60.0f + juce::jmin (900.0f, std::abs (gr_) * 90.0f);
            const float coefA = 1.0f - std::exp (-1.0f / (0.010f * (float) sr_));
            const float coefR = 1.0f - std::exp (-1.0f / (0.001f * rel * (float) sr_));
            gr_ += (targetGr < gr_ ? coefA : coefR) * (targetGr - gr_);
            const float g = juce::Decibels::decibelsToGain (gr_) * makeup;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = b.getWritePointer (ch);
                const float wet = tone_[(size_t) ch].process (s[i] * g);
                s[i] = s[i] * (1.0f - mix) + wet * mix;
            }
        }
    }
private:
    double sr_ = 48000;
    EnvFollower det_;
    std::array<Biquad, 2> tone_;
    float gr_ = 0.0f;
};

//==============================================================================
/// FET Squeeze — 1176-flavoured. Fast attack, ratio switch (including "All
/// buttons in", which is a much softer knee at a very high ratio), parallel mix.
class FetCompBlock final : public Block
{
public:
    FetCompBlock() : Block (BlockType::fetComp) {}
    void prepare (double sr, int) override { sr_ = sr; det_.prepare (sr); reset(); }
    void reset() override { det_.reset(); gr_ = 0.0f; }
    float gainReductionDb() const { return gr_; }

    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float thr = p (0);
        const int ratioIdx = (int) p (1);
        const float ratios[5] = { 4.0f, 8.0f, 12.0f, 20.0f, 30.0f };
        const float knees[5]  = { 6.0f, 4.0f, 3.0f, 2.0f, 12.0f };   // "All" = high ratio, wide knee
        const float ratio = ratios[juce::jlimit (0, 4, ratioIdx)];
        const float knee = knees[juce::jlimit (0, 4, ratioIdx)];
        const float makeup = juce::Decibels::decibelsToGain (p (4));
        const float mix = p (5) / 100.0f;
        det_.setTimes (p (2), p (3));
        const float coefA = 1.0f - std::exp (-1.0f / (0.001f * juce::jmax (0.1f, p (2)) * (float) sr_));
        const float coefR = 1.0f - std::exp (-1.0f / (0.001f * juce::jmax (5.0f, p (3)) * (float) sr_));

        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float det = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float db = juce::Decibels::gainToDecibels (det_.process (det), -100.0f);
            // Soft knee: quadratic interpolation across `knee` dB around the
            // threshold, so the onset of compression is not a corner.
            const float over = db - thr;
            float reduce = 0.0f;
            if (over >= knee * 0.5f)        reduce = over * (1.0f - 1.0f / ratio);
            else if (over > -knee * 0.5f)
            {
                const float x = over + knee * 0.5f;
                reduce = (1.0f - 1.0f / ratio) * x * x / (2.0f * knee);
            }
            const float target = -reduce;
            gr_ += (target < gr_ ? coefA : coefR) * (target - gr_);
            const float g = juce::Decibels::decibelsToGain (gr_) * makeup;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = b.getWritePointer (ch);
                s[i] = s[i] * (1.0f - mix) + s[i] * g * mix;
            }
        }
    }
private:
    double sr_ = 48000;
    EnvFollower det_;
    float gr_ = 0.0f;
};

//==============================================================================
/// Bass EQ — the five bands that are actually on a bass amp's slider strip.
class GraphicEqBlock final : public Block
{
public:
    GraphicEqBlock() : Block (BlockType::graphicEq) {}
    void prepare (double sr, int) override { sr_ = sr; reset(); }
    void reset() override { for (auto& ch : bands_) for (auto& f : ch) f.reset(); }
    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        static constexpr float freqs[kBands] = { 50.0f, 120.0f, 400.0f, 800.0f, 4000.0f };
        const float level = juce::Decibels::decibelsToGain (p (5));
        for (int ch = 0; ch < nCh; ++ch)
            for (int k = 0; k < kBands; ++k)
                bands_[(size_t) ch][(size_t) k].setPeak (sr_, freqs[k], 1.2f, p (k));
        for (int ch = 0; ch < nCh; ++ch)
        {
            auto* s = b.getWritePointer (ch);
            auto& row = bands_[(size_t) ch];
            for (int i = 0; i < n; ++i)
            {
                float x = s[i];
                for (auto& f : row) x = f.process (x);
                s[i] = x * level;
            }
        }
    }
private:
    static constexpr int kBands = 5;
    double sr_ = 48000;
    std::array<std::array<Biquad, kBands>, 2> bands_;
};

//==============================================================================
/// Envelope filter (auto-wah). The cutoff follows the playing envelope through a
/// resonant ladder. `direction` flips the sweep — down is the "sat on" sound.
class EnvFilterBlock final : public Block
{
public:
    EnvFilterBlock() : Block (BlockType::envFilter) {}
    void prepare (double sr, int) override
    {
        sr_ = sr;
        det_.prepare (sr);
        for (auto& f : filt_) f.prepare (sr);
        reset();
    }
    void reset() override { det_.reset(); for (auto& f : filt_) f.reset(); }
    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float sens = p (0) / 100.0f;
        const float range = p (1) / 100.0f;
        const float q = p (2) / 100.0f;
        const bool down = p (3) >= 0.5f;
        const float mix = p (5) / 100.0f;
        det_.setTimes (3.0f, p (4));
        for (auto& f : filt_) f.setResonance (0.2f + q * 0.75f);

        const float base = 90.0f;
        const float span = 200.0f + range * 3200.0f;

        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float det = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float e = juce::jlimit (0.0f, 1.0f, det_.process (det) * (1.0f + sens * 20.0f));
            const float f = base + span * (down ? (1.0f - e) : e);
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto& flt = filt_[(size_t) ch];
                flt.setCutoff (f);
                auto* s = b.getWritePointer (ch);
                const float wet = flt.process (s[i]);
                s[i] = s[i] * (1.0f - mix) + wet * mix;
            }
        }
    }
private:
    double sr_ = 48000;
    EnvFollower det_;
    std::array<LadderFilter, 2> filt_;
};

//==============================================================================
/// Sub Octave. This is the analog design, not a pitch shifter: the input is
/// low-passed hard, squared into a comparator, and a flip-flop divides that
/// square by two (one octave down) and by four (two octaves). It tracks
/// instantly and it only works on single notes — which is exactly the behaviour
/// bass players expect from an octave pedal, and it costs almost nothing.
///
/// The dividers are re-armed by a hysteresis comparator so a noisy zero crossing
/// cannot double-trigger and drop the sub an extra octave mid-note.
class OctaverBlock final : public Block
{
public:
    OctaverBlock() : Block (BlockType::octaver) {}
    void prepare (double sr, int) override
    {
        sr_ = sr;
        det_.prepare (sr);
        det_.setTimes (5.0f, 60.0f);
        reset();
    }
    void reset() override
    {
        det_.reset();
        for (auto& f : trig_) f.reset();
        for (auto& f : shape_) f.reset();
        div2_ = div4_ = 1.0f; count_ = 0; armed_ = false;
    }
    void process (juce::AudioBuffer<float>& b, double) override
    {
        const int n = b.getNumSamples(), nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        const float sub1 = p (0) / 100.0f;
        const float sub2 = p (1) / 100.0f;
        const float dry = p (2) / 100.0f;
        const float toneHz = 200.0f + (p (3) / 100.0f) * 1400.0f;
        const float growl = p (4) / 100.0f;

        for (int ch = 0; ch < nCh; ++ch)
        {
            trig_[(size_t) ch].setLowPass (sr_, 220.0f, 0.707f);   // comparator input: fundamental only
            shape_[(size_t) ch].setLowPass (sr_, toneHz, 0.8f);
        }

        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float in = r ? 0.5f * (l[i] + r[i]) : l[i];
            const float amp = det_.process (in);
            const float t = trig_[0].process (in);

            // Hysteresis comparator, thresholds scaled to the note's own level so
            // it keeps tracking as the note decays.
            const float h = juce::jmax (0.002f, amp * 0.25f);
            if (! armed_ && t > h) { armed_ = true; div2_ = -div2_; if (++count_ >= 2) { count_ = 0; div4_ = -div4_; } }
            else if (armed_ && t < -h) armed_ = false;

            // The squares are shaped by the note's own envelope, so the sub
            // breathes with the playing instead of sitting there as a constant
            // buzz once the string has died.
            const float env = juce::jmin (1.0f, amp * 3.0f);
            float s1 = div2_ * env;
            float s2 = div4_ * env;
            if (growl > 0.0f)   // a touch of the raw square through, un-smoothed
            {
                s1 = s1 * (1.0f - growl) + std::tanh (s1 * 4.0f) * growl;
                s2 = s2 * (1.0f - growl) + std::tanh (s2 * 4.0f) * growl;
            }
            const float sub = shape_[0].process (s1 * sub1 + s2 * sub2) * 0.5f;
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* s = b.getWritePointer (ch);
                s[i] = s[i] * dry + sub;
            }
        }
    }
private:
    double sr_ = 48000;
    EnvFollower det_;
    std::array<Biquad, 2> trig_, shape_;
    float div2_ = 1.0f, div4_ = 1.0f;
    int count_ = 0;
    bool armed_ = false;
};

//==============================================================================
inline std::unique_ptr<Block> createBlock (BlockType t)
{
    const auto& s = blockSpec (t);
    if (s.reuse.has_value()) return std::make_unique<ReusedBlock> (t, *s.reuse);
    switch (t)
    {
        case BlockType::noiseGate: return std::make_unique<NoiseGateBlock>();
        case BlockType::bassOd:    return std::make_unique<BassOdBlock>();
        case BlockType::tubeOd:    return std::make_unique<TubeOdBlock>();
        case BlockType::fuzz:      return std::make_unique<FuzzBlock>();
        case BlockType::boost:     return std::make_unique<BoostBlock>();
        case BlockType::optoComp:  return std::make_unique<OptoCompBlock>();
        case BlockType::fetComp:   return std::make_unique<FetCompBlock>();
        case BlockType::graphicEq: return std::make_unique<GraphicEqBlock>();
        case BlockType::envFilter: return std::make_unique<EnvFilterBlock>();
        case BlockType::octaver:   return std::make_unique<OctaverBlock>();
        default:                   return nullptr;
    }
}

//==============================================================================
// Serializable chain state
struct BlockState
{
    BlockType type = BlockType::none;
    bool bypassed = false;
    ParamValues params;
};
using ChainState = std::vector<BlockState>;

inline BlockState defaultBlockState (BlockType t, bool bypassed = false)
{
    BlockState s;
    s.type = t;
    s.bypassed = bypassed;
    s.params = defaultBlockParams (t);
    return s;
}

//==============================================================================
/// BlockChain — an ordered, reorderable list of pedals with clickless bypass.
/// Same shape and same threading discipline as `jm::FxRackDsp`, with two
/// differences that matter on a pedalboard: the chain reports its own latency
/// (the oversampled drives add some), and a structural edit is applied by the
/// owner through `applyStructuralChange`, which hands the chain dry for 25 ms so
/// dragging a pedal to a new position never produces a step in the output.
class BlockChain
{
public:
    void setCapacity (int n) { capacity_ = juce::jlimit (1, 16, n); }
    int capacity() const { return capacity_; }

    void prepare (double sr, int maxBlock)
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        sr_ = sr; maxBlock_ = maxBlock;
        // Size the dry buffer HERE. It used to be grown lazily inside process(),
        // which meant the very first block after prepare called the allocator on
        // the audio thread — a measurable spike in the soak test and exactly the
        // thing the no-allocation rule exists to prevent.
        dry_.setSize (2, juce::jmax (1, maxBlock), false, true, true);
        for (auto& s : slots_)
            if (s.block) { s.block->prepare (sr, maxBlock); s.mix.reset (sr, 0.02); }
    }
    void reset()
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        for (auto& s : slots_) if (s.block) s.block->reset();
    }

    static constexpr int kRestructureDelayMs = 25;

    void beginRestructure()
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        for (auto& s : slots_) s.mix.setTargetValue (0.0f);
    }

    /// MESSAGE THREAD ONLY. Hands the chain dry, swaps, then ramps back.
    void applyStructuralChange (const ChainState& st, std::function<void()> onApplied = {})
    {
        beginRestructure();
        juce::Timer::callAfterDelay (kRestructureDelayMs,
                                     [this, weak = std::weak_ptr<int> (alive_), st, onApplied]
                                     {
                                         if (weak.expired()) return;
                                         setState (st, true);
                                         if (onApplied) onApplied();
                                     });
    }

    /// Rebuild from state. Keeps an existing block instance when the type at a
    /// slot index is unchanged, so delay and reverb tails survive a knob edit.
    void setState (const ChainState& st, bool rampIn = false)
    {
        std::vector<Slot> next ((size_t) juce::jmin ((int) st.size(), capacity_));
        {
            const juce::SpinLock::ScopedLockType sl (lock_);
            for (size_t i = 0; i < next.size(); ++i)
                for (auto& old : slots_)
                    if (old.block && old.block->type() == st[i].type) { next[i].block = std::move (old.block); break; }
        }
        for (size_t i = 0; i < next.size(); ++i)          // heavy work outside the lock
        {
            const auto& u = st[i];
            if (! next[i].block)
            {
                next[i].block = createBlock (u.type);
                if (next[i].block && sr_ > 0) next[i].block->prepare (sr_, maxBlock_);
            }
            if (next[i].block)
                for (auto& [id, v] : u.params) next[i].block->setParam (id, v);
            next[i].bypassed = u.bypassed;
            next[i].mix.reset (sr_ > 0 ? sr_ : 48000.0, 0.02);
            if (rampIn)
            {
                next[i].mix.setCurrentAndTargetValue (0.0f);
                next[i].mix.setTargetValue (u.bypassed ? 0.0f : 1.0f);
            }
            else
                next[i].mix.setCurrentAndTargetValue (u.bypassed ? 0.0f : 1.0f);
        }
        const juce::SpinLock::ScopedLockType sl (lock_);
        slots_ = std::move (next);
    }

    void setParam (int slot, const std::string& id, float v)
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        if (slot >= 0 && slot < (int) slots_.size() && slots_[(size_t) slot].block)
            slots_[(size_t) slot].block->setParam (id, v);
    }
    void setBypassed (int slot, bool b)
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        if (slot >= 0 && slot < (int) slots_.size())
        {
            slots_[(size_t) slot].bypassed = b;
            slots_[(size_t) slot].mix.setTargetValue (b ? 0.0f : 1.0f);
        }
    }
    /// Audio-thread-safe toggle (a footswitch arrives on the MIDI/audio thread).
    void setBypassedRt (int slot, bool b)
    {
        const juce::SpinLock::ScopedTryLockType sl (lock_);
        if (! sl.isLocked()) return;
        if (slot >= 0 && slot < (int) slots_.size() && slots_[(size_t) slot].bypassed != b)
        {
            slots_[(size_t) slot].bypassed = b;
            slots_[(size_t) slot].mix.setTargetValue (b ? 0.0f : 1.0f);
        }
    }
    int numBlocks() const
    {
        const juce::SpinLock::ScopedTryLockType sl (lock_);
        return sl.isLocked() ? (int) slots_.size() : 0;
    }
    int latencySamples() const
    {
        const juce::SpinLock::ScopedTryLockType sl (lock_);
        if (! sl.isLocked()) return 0;
        int t = 0;
        for (auto& s : slots_) if (s.block) t += s.block->latencySamples();
        return t;
    }

    void process (juce::AudioBuffer<float>& buffer, double bpm)
    {
        const juce::SpinLock::ScopedTryLockType sl (lock_);
        if (! sl.isLocked()) return;                       // mid-rebuild: pass dry
        const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
        if (n <= 0 || nCh <= 0) return;
        if (dry_.getNumSamples() < n || dry_.getNumChannels() < nCh) dry_.setSize (nCh, n, false, false, true);

        for (auto& s : slots_)
        {
            if (! s.block) continue;
            const bool silent = ! s.mix.isSmoothing() && s.mix.getCurrentValue() <= 0.0f;
            // A fully bypassed time-based block still gets fed so its tail is
            // there the moment it is switched back on — but a bypassed block
            // whose mix has settled at zero costs nothing to skip except that
            // tail, and skipping it is what keeps a big board cheap. Time
            // blocks (delay/reverb) are the exception: they keep running.
            if (silent && ! isTimeBlock (s.block->type())) continue;
            for (int ch = 0; ch < nCh; ++ch) dry_.copyFrom (ch, 0, buffer, ch, 0, n);
            s.block->process (buffer, bpm);
            for (int i = 0; i < n; ++i)
            {
                const float m = s.mix.getNextValue();
                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto* w = buffer.getWritePointer (ch);
                    w[i] = dry_.getSample (ch, i) * (1.0f - m) + w[i] * m;
                }
            }
        }
    }

private:
    static bool isTimeBlock (BlockType t)
    {
        return t == BlockType::analogDelay || t == BlockType::digitalDelay
            || t == BlockType::roomVerb || t == BlockType::ambientVerb;
    }
    struct Slot
    {
        std::unique_ptr<Block> block;
        bool bypassed = false;
        juce::SmoothedValue<float> mix { 1.0f };
    };
    mutable juce::SpinLock lock_;
    std::vector<Slot> slots_;
    juce::AudioBuffer<float> dry_;
    double sr_ = 0;
    int maxBlock_ = 512, capacity_ = kMaxPreBlocks;
    /// Lifetime token for the deferred half of `applyStructuralChange`.
    std::shared_ptr<int> alive_ { std::make_shared<int> (0) };
};

} // namespace lowend
