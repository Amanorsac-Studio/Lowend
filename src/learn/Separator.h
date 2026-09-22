// Separator.h — the only doorway into the separation network.
//
// Plain types only (no ONNX Runtime, no JUCE) on purpose: the ONNX headers are
// heavy and the library is loaded at runtime, so keeping them behind this
// interface means the rest of the app neither includes nor links against them
// directly, and an unavailable model is an error return rather than a crash.
#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace lowend::stems {

/// Progress 0..1 and a short stage name. Return false to cancel.
using Progress = std::function<bool (float progress, const std::string& stage)>;

struct Options
{
    /// Demucs' time-equivariance trick: separate the song again at a different
    /// sub-second offset and average. Each extra shift costs another full pass
    /// and buys a few tenths of a dB.
    int shifts = 1;
    /// Fraction of each 7.8 s segment shared with its neighbour. More overlap
    /// means more passes but fewer seams.
    float overlap = 0.25f;
    /// 0 = every core.
    int threads = 0;
};

/// Separates a 44.1 kHz stereo song into drums, bass, other, vocals — the order
/// HT-Demucs emits them in. `out[2*s]` / `out[2*s+1]` receive stem `s`'s left
/// and right channels, each `frames` long.
///
/// `onnxPath` is the exported HT-Demucs graph; its weights file must sit beside
/// it. Returns false with `error` set on failure or cancellation ("cancelled").
bool separate (const std::string& onnxPath,
               const float* left, const float* right, int frames,
               const Options& options, const Progress& progress,
               std::array<std::vector<float>, 8>& out, std::string& error);

} // namespace lowend::stems
