#pragma once
// A player's own amp: a Neural Amp Modeler capture (.nam), run inside the rig.
//
// A .nam file is a stranger's file. It is read as text, size-capped, parsed by a
// JSON parser that reports instead of throwing, and built inside try/catch (all in
// nam/NamBridge.cpp). The built model then has to keep up: it is timed on a few
// blocks before it is accepted, so a capture too heavy for this machine is
// refused with a reason, never run.
//
// Captures are trained at one rate (nearly always 48 kHz). When the session runs
// at another, audio is resampled into the model's rate and back.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>
#include "nam/NamBridge.h"

namespace lowend {

class NamModel
{
public:
    struct Info
    {
        juce::String name;          ///< from the file's metadata, else the file name
        juce::String gear;          ///< "amp", "amp_cab", "pedal"... when the file says
        double modelRate = 48000.0;
        bool hasLoudness = false;
        double loudnessDb = 0.0;
    };

    /// A capture's file name, as a whole number a preset can keep in a float
    /// parameter (24 bits, exact in a float). 0 means "no capture".
    static int keyFor (const juce::String& fileName)
    {
        juce::uint32 h = 2166136261u;                         // FNV-1a over the lower-cased name
        for (auto c : fileName.toLowerCase()) { h ^= (juce::uint32) c; h *= 16777619u; }
        const int k = (int) (h & 0xffffffu);
        return k == 0 ? 1 : k;
    }
    static juce::File fileFor (int key, const juce::File& dir)
    {
        if (key <= 0) return {};
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.nam"))
            if (keyFor (f.getFileName()) == key) return f;
        return {};
    }

    /// Check a capture's text without keeping it: for an import, before it is copied in.
    static bool check (const juce::String& text, juce::String& error, Info* info = nullptr)
    {
        amnam::Info ni; std::string err;
        auto m = amnam::build (text.toStdString(), ni, err);
        if (! m) { error = err; return false; }
        if (info) *info = convert (ni, {});
        return true;
    }

    /// Build a capture to run at `hostRate` in blocks of up to `maxBlock`.
    /// NOT the audio thread: parses, allocates, pre-warms and times it.
    static std::unique_ptr<NamModel> fromText (const juce::String& text, const juce::String& fallbackName,
                                              double hostRate, int maxBlock, juce::String& error)
    {
        std::unique_ptr<NamModel> m (new NamModel());
        amnam::Info ni; std::string err;
        m->model_ = amnam::build (text.toStdString(), ni, err);
        if (! m->model_) { error = err; return {}; }
        m->info_ = convert (ni, fallbackName);
        m->setUp (hostRate > 0 ? hostRate : 48000.0, juce::jmax (16, maxBlock));

        // It has to keep up. Warm it up, then time a run of blocks and judge it by the
        // typical one (the median), so a busy moment elsewhere on the computer does
        // not refuse a capture that runs fine: refuse it if a typical block needs more
        // than 40% of the time the audio lasts (the rest of the rig needs the rest).
        std::vector<float> probe ((size_t) m->maxBlock_);
        std::vector<double> times;
        const int warm = 4, runs = 24;
        for (int r = 0; r < warm + runs; ++r)
        {
            for (int i = 0; i < m->maxBlock_; ++i) probe[(size_t) i] = 0.05f * std::sin (0.07f * (float) (r * m->maxBlock_ + i));
            const auto t0 = std::chrono::steady_clock::now();
            m->process (probe.data(), m->maxBlock_);
            if (r >= warm) times.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count());
        }
        std::nth_element (times.begin(), times.begin() + runs / 2, times.end());
        const double typical = times[(size_t) runs / 2], budget = 1000.0 * m->maxBlock_ / m->hostRate_;
        if (typical > 0.4 * budget)
        {
            error = "This capture is too heavy for this computer to run live (it needs "
                  + juce::String (juce::roundToInt (100.0 * typical / budget)) + "% of the time it has).";
            return {};
        }
        m->setUp (m->hostRate_, m->maxBlock_);                // forget the probe
        return m;
    }
    static std::unique_ptr<NamModel> fromFile (const juce::File& f, double hostRate, int maxBlock, juce::String& error)
    {
        if (! f.existsAsFile()) { error = "The capture file is missing."; return {}; }
        if (f.getSize() > (juce::int64) amnam::kMaxTextBytes) { error = "This file is too big to be an amp capture."; return {}; }
        return fromText (f.loadFileAsString(), f.getFileNameWithoutExtension(), hostRate, maxBlock, error);
    }

    const Info& info() const noexcept { return info_; }
    double hostRate() const noexcept { return hostRate_; }

    /// Mono, at the host rate, in place, n <= maxBlock. Audio-thread safe.
    void process (float* io, int n) noexcept
    {
        if (! model_ || n <= 0) return;
        n = juce::jmin (n, maxBlock_);
        if (! resample_) { model_->process (io, io, n); return; }
        if (antiAlias_)
            for (int i = 0; i < n; ++i) io[i] = aa_[1].processSample (aa_[0].processSample (io[i]));
        down_.push (io, n);
        owed_ += n * info_.modelRate / hostRate_;
        const int m = juce::jlimit (0, (int) mid_.size(), (int) owed_);
        owed_ -= m;
        if (m > 0)
        {
            down_.pull (mid_.data(), m);
            model_->process (mid_.data(), mid_.data(), m);
            up_.push (mid_.data(), m);
        }
        up_.pull (io, n);
    }

    /// Extra delay the resampling adds; 0 when the session runs at the capture's rate.
    int latencySamples() const noexcept
    {
        if (! resample_) return 0;
        const double each = juce::Interpolators::WindowedSinc::getBaseLatency() + Stage::kPrime;
        return (int) std::round (each * (1.0 + hostRate_ / info_.modelRate));
    }

private:
    NamModel() = default;

    static Info convert (const amnam::Info& ni, const juce::String& fallback)
    {
        Info i;
        i.name = juce::String (ni.name).trim();
        if (i.name.isEmpty()) i.name = fallback;
        i.gear = ni.gear; i.modelRate = ni.rate; i.hasLoudness = ni.hasLoudness; i.loudnessDb = ni.loudness;
        return i;
    }

    void setUp (double hostRate, int maxBlock)
    {
        hostRate_ = hostRate; maxBlock_ = maxBlock;
        resample_ = std::abs (hostRate_ - info_.modelRate) > 0.5;
        const int modelMax = (int) std::ceil (maxBlock_ * info_.modelRate / hostRate_) + 8;
        model_->reset (info_.modelRate, resample_ ? modelMax : maxBlock_);
        mid_.assign ((size_t) modelMax, 0.0f);
        owed_ = 0.0;
        antiAlias_ = false;
        if (resample_)
        {
            down_.prepare (hostRate_, info_.modelRate, maxBlock_);
            up_.prepare (info_.modelRate, hostRate_, modelMax);
            if (hostRate_ > info_.modelRate)
            {
                // the sinc stage does not band-limit on the way down; this does
                auto c = juce::dsp::IIR::Coefficients<float>::makeLowPass (hostRate_, 0.45 * info_.modelRate, 0.707);
                for (auto& f : aa_) { f.coefficients = c; f.reset(); }
                antiAlias_ = true;
            }
        }
    }

    /// One direction of the stream at a fixed ratio. The FIFO starts primed so the
    /// interpolator always has the input it needs; it is sized once, in setUp.
    struct Stage
    {
        static constexpr int kPrime = 64;
        juce::Interpolators::WindowedSinc interp;
        std::vector<float> fifo;
        int fill = 0;
        double speed = 1.0;                       // input samples per output sample

        void prepare (double inRate, double outRate, int maxIn)
        {
            speed = inRate / outRate;
            fifo.assign ((size_t) (maxIn * 4 + kPrime * 4 + 256), 0.0f);
            fill = kPrime;
            interp.reset();
        }
        void push (const float* x, int n) noexcept
        {
            n = juce::jmin (n, (int) fifo.size() - fill);
            if (n <= 0) return;
            std::memcpy (fifo.data() + fill, x, sizeof (float) * (size_t) n);
            fill += n;
        }
        void pull (float* y, int m) noexcept
        {
            const int used = juce::jlimit (0, fill, interp.process (speed, fifo.data(), y, m, fill, 0));
            fill -= used;
            if (fill > 0) std::memmove (fifo.data(), fifo.data() + used, sizeof (float) * (size_t) fill);
        }
    };

    std::unique_ptr<amnam::Model> model_;
    Info info_;
    double hostRate_ = 48000.0, owed_ = 0.0;
    int maxBlock_ = 512;
    bool resample_ = false, antiAlias_ = false;
    Stage down_, up_;
    std::array<juce::dsp::IIR::Filter<float>, 2> aa_;
    std::vector<float> mid_;
};

} // namespace lowend
