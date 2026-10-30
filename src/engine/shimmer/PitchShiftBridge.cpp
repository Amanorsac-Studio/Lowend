// Compiled only into the product's third-party DSP library (see CMakeLists.txt).
#include "PitchShiftBridge.h"

#include <algorithm>
#include <vector>

#include "signalsmith-stretch.h"

namespace amshift {

namespace {

class StretchShifter final : public Shifter
{
public:
    void prepare (double rate, int channels, int maxBlock) override
    {
        channels_ = std::max (1, std::min (2, channels));
        // A 50 ms analysis block every 12.5 ms, with the computation spread
        // across the calls in between. The library's default (120 ms every
        // 30 ms) does one big FFT every few audio blocks, which lands as a
        // spike on whichever block it falls in; a smaller block costs about the
        // same on average and never spikes. Plenty for an octave-up wash that
        // sits inside a reverb tail.
        stretch_.configure (channels_, (int) (rate * 0.05), (int) (rate * 0.0125), true);
        setSemitones (semitones_);
        scratch_.assign ((size_t) (channels_ * std::max (1, maxBlock)), 0.0f);
        stretch_.reset();
    }
    void setSemitones (float semitones) override
    {
        semitones_ = semitones;
        // The tonality limit keeps the top of the spectrum from being spread
        // into noise: Signalsmith's own recommendation for natural results.
        stretch_.setTransposeSemitones (semitones, 8000.0f / 48000.0f);
    }
    void reset() override { stretch_.reset(); }
    void process (const float* const* in, float* const* out, int n) noexcept override
    {
        if (n <= 0) return;
        stretch_.process (in, n, out, n);
    }
    int latency() const override { return stretch_.inputLatency() + stretch_.outputLatency(); }

private:
    signalsmith::stretch::SignalsmithStretch<float> stretch_ { 12345 };   // seeded: the same every run
    std::vector<float> scratch_;
    int channels_ = 2;
    float semitones_ = 12.0f;
};

} // namespace

std::unique_ptr<Shifter> createShifter() { return std::make_unique<StretchShifter>(); }

} // namespace amshift
