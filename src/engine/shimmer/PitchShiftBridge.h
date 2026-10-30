#pragma once
// The one doorway to Signalsmith Stretch (MIT), the pitch shifter behind the
// Hall's Shimmer. PitchShiftBridge.cpp is compiled into the product's
// third-party DSP library next to NAM; this header names nothing of the
// library, so nothing else in the product is built against its headers.
// Plain C++ only (no JUCE) on purpose: the library links without the JUCE modules.

#include <memory>

namespace amshift {

class Shifter
{
public:
    virtual ~Shifter() = default;
    /// Allocates. Not the audio thread.
    virtual void prepare (double rate, int channels, int maxBlock) = 0;
    virtual void setSemitones (float semitones) = 0;
    virtual void reset() = 0;
    /// `in[c]` and `out[c]` must be different buffers; n <= maxBlock. Audio-thread safe.
    virtual void process (const float* const* in, float* const* out, int n) noexcept = 0;
    /// Samples from a sound going in to its shifted copy coming out.
    virtual int latency() const = 0;
};

std::unique_ptr<Shifter> createShifter();

} // namespace amshift
