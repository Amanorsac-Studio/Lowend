// Stft.h — the short-time Fourier transform HT-Demucs was trained with.
//
// Demucs is a HYBRID model: one branch looks at the waveform, the other at a
// complex spectrogram, and the two are summed. The spectrogram branch only
// works if our STFT matches the one used in training bin for bin, so every
// constant here comes from Meta's demucs/spec.py + demucs/hdemucs.py:
//
//   n_fft 4096, hop 1024 (75 % overlap), periodic Hann window,
//   normalised=True (a 1/sqrt(n_fft) scaling on the forward transform),
//   centred with reflect padding, and then — Demucs' own quirk — the first two
//   and last two frames are thrown away and the Nyquist bin is dropped, which
//   is what turns 340 frames x 2049 bins into the 336 x 2048 the network wants.
//
// A 7.8 s segment therefore becomes exactly 4 x 2048 x 336: two channels, each
// contributing its real and imaginary parts as separate "channels"
// (complex-as-channels — Demucs does not use magnitude spectrograms).
//
// The transform is its own inverse to about -130 dB (LowEndTests checks this):
// the forward scaling is undone, the window is applied a second time, and the
// overlapping frames are divided by the sum of squared windows, which is the
// standard least-squares reconstruction.
//
// NOTE ON PADDING. demucs.cpp reflects the edge sample twice (x[0] x[0] x[1]);
// torch.stft's reflect mode does not (x[1] x[0] x[1]). We follow torch, since
// that is what the weights were trained against. It only affects the first and
// last frame of each segment, and segments overlap, but correctness is free.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>
#include <vector>

namespace lowend::stems {

constexpr int kSampleRate  = 44100;
constexpr int kFftSize     = 4096;
constexpr int kHop         = 1024;
constexpr int kBins        = kFftSize / 2 + 1;   // 2049 from the FFT
constexpr int kModelBins   = 2048;               // Nyquist dropped for the model
constexpr int kEdgeFrames  = 2;                  // frames trimmed each side

/// Frames produced for `paddedSamples` samples of input.
inline int frameCount (int paddedSamples) { return paddedSamples / kHop + 1; }

/// A periodic Hann window, as torch.hann_window(4096) produces it.
inline const std::vector<float>& hann()
{
    static const std::vector<float> w = []
    {
        std::vector<float> v ((size_t) kFftSize);
        for (int n = 0; n < kFftSize; ++n)
            v[(size_t) n] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * (float) n / (float) kFftSize));
        return v;
    }();
    return w;
}

/// Reusable scratch for one channel's transform. Sized once; the transform
/// itself allocates nothing.
class Stft
{
public:
    /// `paddedSamples` is the length of the signal handed to `forward`.
    explicit Stft (int paddedSamples)
        : frames_ (frameCount (paddedSamples)),
          fft_ ((int) std::log2 (kFftSize))
    {
        centred_.assign ((size_t) paddedSamples + kFftSize, 0.0f);
        scratch_.assign (2 * (size_t) kFftSize, 0.0f);
        // Sum of squared windows across the overlap, for the inverse. Computed
        // once because it depends only on the frame count.
        winSq_.assign ((size_t) (kFftSize + kHop * (frames_ - 1)), 0.0f);
        const auto& w = hann();
        for (int f = 0; f < frames_; ++f)
            for (int i = 0; i < kFftSize; ++i)
            {
                const size_t j = (size_t) (f * kHop + i);
                if (j < winSq_.size()) winSq_[j] += w[(size_t) i] * w[(size_t) i];
            }
    }

    int frames() const noexcept { return frames_; }

    /// `in` holds `paddedSamples` samples. `out` receives frames_ * kBins
    /// complex values, frame-major.
    void forward (const float* in, int paddedSamples, std::complex<float>* out) noexcept
    {
        // Centre the signal: reflect kFftSize/2 samples onto each end, so the
        // first frame is centred on sample 0.
        const int p = kFftSize / 2;
        std::copy (in, in + paddedSamples, centred_.begin() + p);
        for (int i = 0; i < p; ++i)
        {
            centred_[(size_t) (p - 1 - i)]                  = in[juce::jmin (i + 1, paddedSamples - 1)];
            centred_[(size_t) (p + paddedSamples + i)]      = in[juce::jmax (0, paddedSamples - 2 - i)];
        }

        const auto& w = hann();
        const float scale = 1.0f / std::sqrt ((float) kFftSize);
        for (int f = 0; f < frames_; ++f)
        {
            float* s = scratch_.data();
            const float* src = centred_.data() + (size_t) f * kHop;
            for (int i = 0; i < kFftSize; ++i) s[i] = src[i] * w[(size_t) i];
            std::fill (s + kFftSize, s + 2 * kFftSize, 0.0f);
            fft_.performRealOnlyForwardTransform (s, true);
            // JUCE lays the result out as interleaved re/im pairs.
            auto* dst = out + (size_t) f * kBins;
            for (int b = 0; b < kBins; ++b)
                dst[b] = { s[2 * b] * scale, s[2 * b + 1] * scale };
        }
    }

    /// Inverse of `forward`. `out` receives `paddedSamples` samples.
    void inverse (const std::complex<float>* in, int paddedSamples, float* out) noexcept
    {
        std::fill (centred_.begin(), centred_.end(), 0.0f);
        const auto& w = hann();
        const float scale = std::sqrt ((float) kFftSize);

        for (int f = 0; f < frames_; ++f)
        {
            float* s = scratch_.data();
            const auto* src = in + (size_t) f * kBins;
            for (int b = 0; b < kBins; ++b) { s[2 * b] = src[b].real() * scale; s[2 * b + 1] = src[b].imag() * scale; }
            fft_.performRealOnlyInverseTransform (s);
            const size_t base = (size_t) f * kHop;
            for (int i = 0; i < kFftSize; ++i)
            {
                const size_t j = base + (size_t) i;
                if (j >= centred_.size()) break;
                const float norm = j < winSq_.size() ? winSq_[j] : 1.0f;
                centred_[j] += s[i] * w[(size_t) i] / (norm + 1.0e-8f);
            }
        }
        std::copy (centred_.begin() + kFftSize / 2,
                   centred_.begin() + kFftSize / 2 + paddedSamples, out);
    }

private:
    int frames_;
    juce::dsp::FFT fft_;
    std::vector<float> centred_, scratch_, winSq_;
};

} // namespace lowend::stems
