// Compiled only into the product's NAM library (see CMakeLists.txt).
#include "NamBridge.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <type_traits>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/model_config.h"
#include "NAM/container.h"
#include "NAM/convnet.h"
#include "NAM/linear.h"
#include "NAM/lstm.h"
#include "NAM/sequential.h"
#include "NAM/wavenet/model.h"

static_assert (std::is_same_v<NAM_SAMPLE, float>, "the NAM library is built with NAM_SAMPLE_FLOAT");

namespace amnam {

namespace {

class NamModel final : public Model
{
public:
    explicit NamModel (std::unique_ptr<nam::DSP> d) : dsp_ (std::move (d)) {}
    void reset (double rate, int maxBlock) override
    {
        dsp_->Reset (rate, maxBlock);
        outs_.assign ((size_t) dsp_->NumOutputChannels(), std::vector<float> ((size_t) maxBlock, 0.0f));
        ptrs_.clear();
        for (auto& o : outs_) ptrs_.push_back (o.data());
        in_.assign ((size_t) maxBlock, 0.0f);
        max_ = maxBlock;
    }
    void process (const float* in, float* out, int n) noexcept override
    {
        if (n <= 0 || n > max_ || ptrs_.empty()) return;
        std::memcpy (in_.data(), in, sizeof (float) * (size_t) n);   // NAM wants a non-const input
        float* ip = in_.data();
        dsp_->process (&ip, ptrs_.data(), n);
        std::memcpy (out, outs_[0].data(), sizeof (float) * (size_t) n);
    }
private:
    std::unique_ptr<nam::DSP> dsp_;
    std::vector<std::vector<float>> outs_;
    std::vector<float*> ptrs_;
    std::vector<float> in_;
    int max_ = 0;
};

std::string clip (std::string s, std::size_t n) { if (s.size() > n) s.resize (n); return s; }

/// NAM registers each architecture from a static initialiser in its own source
/// file. Linked through the plug-in's static SharedCode library, those files are
/// dropped (nothing names them), and every capture is "not a known architecture".
/// Naming each one here keeps it, and registers it if it has not done so itself.
void registerArchitectures()
{
    static const bool done = []
    {
        auto& reg = nam::ConfigParserRegistry::instance();
        const std::pair<const char*, nam::ConfigParserFunction> all[] {
            { "WaveNet", nam::wavenet::create_config },
            { "LSTM", nam::lstm::create_config },
            { "ConvNet", nam::convnet::create_config },
            { "Linear", nam::linear::create_config },
            { "SlimmableContainer", nam::container::create_config },
            { "Sequential", nam::sequential::create_config } };
        for (auto& [name, fn] : all)
            if (! reg.has (name)) reg.registerParser (name, fn);
        return true;
    }();
    (void) done;
}

} // namespace

std::unique_ptr<Model> build (const std::string& text, Info& info, std::string& error)
{
    info = {};
    try { registerArchitectures(); }
    catch (...) { error = "The amp capture engine could not start."; return {}; }
    if (text.size() > kMaxTextBytes) { error = "This file is too big to be an amp capture."; return {}; }
    try
    {
        auto j = nlohmann::json::parse (text, nullptr, false);    // no exceptions from the parser itself
        if (j.is_discarded() || ! j.is_object())
        {
            error = "This is not a NAM capture (it is not readable as one).";
            return {};
        }
        if (! j.contains ("architecture") || ! j["architecture"].is_string() || ! j.contains ("weights") || ! j["weights"].is_array())
        {
            error = "This is not a NAM capture (it has no model in it).";
            return {};
        }
        // A capture is thousands to a few hundred thousand weights. Millions would not
        // run in real time on anything, so they are refused before anything is built.
        if (j["weights"].size() > 4000000) { error = "This capture is far too large to run live."; return {}; }

        double rate = 48000.0;
        if (j.contains ("sample_rate") && j["sample_rate"].is_number()) rate = j["sample_rate"].get<double>();
        info.rate = (rate >= 8000.0 && rate <= 384000.0) ? rate : 48000.0;
        if (j.contains ("metadata") && j["metadata"].is_object())
        {
            const auto& m = j["metadata"];
            if (m.contains ("name") && m["name"].is_string()) info.name = clip (m["name"].get<std::string>(), 80);
            if (m.contains ("gear_type") && m["gear_type"].is_string()) info.gear = clip (m["gear_type"].get<std::string>(), 40);
            if (m.contains ("loudness") && m["loudness"].is_number())
            {
                info.hasLoudness = true;
                info.loudness = std::clamp (m["loudness"].get<double>(), -60.0, 20.0);
            }
        }

        nam::dspData data;
        auto dsp = nam::get_dsp (j, data);
        if (! dsp) { error = "This capture could not be built."; return {}; }
        if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() < 1) { error = "This capture is not a mono amp."; return {}; }
        return std::make_unique<NamModel> (std::move (dsp));
    }
    catch (const std::bad_alloc&) { error = "This capture is too large to load."; }
    catch (const std::exception& e) { error = "This capture could not be read: " + clip (e.what(), 160); }
    catch (...) { error = "This capture could not be read."; }
    return {};
}

} // namespace amnam
