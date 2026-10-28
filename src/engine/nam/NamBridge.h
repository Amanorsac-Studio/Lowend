#pragma once
// The one doorway to NeuralAmpModelerCore (MIT). NamBridge.cpp is compiled into
// its own library together with NAM, Eigen and nlohmann/json; this header names
// none of them, so nothing else in the product is built against their headers.
// Plain C++ only (no JUCE) on purpose: the library links without the JUCE modules.

#include <memory>
#include <string>

namespace amnam {

struct Info
{
    std::string name;               ///< metadata "name", empty when the file has none
    std::string gear;               ///< metadata "gear_type"
    double rate = 48000.0;          ///< the rate the capture was trained at
    bool hasLoudness = false;
    double loudness = 0.0;          ///< dB, when the file knows
};

class Model
{
public:
    virtual ~Model() = default;
    /// Allocates and pre-warms. Not the audio thread.
    virtual void reset (double rate, int maxBlock) = 0;
    /// Mono in, mono out (the first output), n <= maxBlock. Audio-thread safe.
    virtual void process (const float* in, float* out, int n) noexcept = 0;
};

constexpr std::size_t kMaxTextBytes = 32u << 20;

/// Parse and build a capture from the text of a .nam file. Never throws: on any
/// problem it returns nullptr and puts a sentence a player can read in `error`.
std::unique_ptr<Model> build (const std::string& text, Info& info, std::string& error);

} // namespace amnam
