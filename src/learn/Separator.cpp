// Separator.cpp — HT-Demucs, run as ONNX. See Separator.h.
//
// WHY ONNX. The first version of this ran demucs.cpp, a C++/Eigen port of the
// same model. It separated correctly but took about THIRTY times real time on a
// laptop — over an hour for one song — because Eigen compiled by MSVC with no
// BLAS is nowhere near a tuned inference runtime. The identical network, run
// through ONNX Runtime, takes about 0.5x real time on the same machine: a song
// in roughly two minutes. Nothing about the model changed, only who does the
// matrix multiplications, so the separation quality is the same.
//
// WHAT IS HERE. ONNX Runtime does the network; everything around it is the
// audio pipeline Demucs needs, ported from Meta's apply.py and demucs.cpp:
//
//   1. normalise the whole song by the mean and deviation of its mono mix;
//   2. shift it by a sub-second offset (time equivariance; averaging a second
//      shift is what "best" quality buys);
//   3. cut it into 7.8 s segments overlapping by 25 %;
//   4. for each segment: STFT, stack the complex spectrogram as four "channels"
//      (Demucs uses complex-as-channels, not magnitudes), normalise, run the
//      network, un-normalise, inverse STFT, and add the network's time-domain
//      branch — Demucs is hybrid, and a stem is the SUM of the two branches;
//   5. cross-fade the segments back together with triangular weights.
//
// The normalisation in step 4 is also present inside Intel's exported graph.
// That is harmless and deliberate: feeding already-normalised data through a
// normalise/denormalise pair is an identity, so this code is correct whether or
// not the export kept those operations.
//
// THREADING. One thread calls in; ONNX Runtime uses every core internally, so
// segments are processed in sequence. That is a large simplification over the
// thread-per-piece scheme the Eigen version needed, and it is also why cancel
// is simply a flag checked between segments.
//
// LOADING THE RUNTIME (Windows). Two traps, both silent until a customer hits
// them. A plug-in's own folder is not on the DLL search path — the HOST's is —
// so onnxruntime.dll sitting beside Low End.vst3 would simply not be found. And
// Windows ships its own, older onnxruntime.dll in System32, which is what a
// search by name finds instead. So the import is delay-loaded, and before
// anything touches it we load OUR copy by full path; the loader then binds the
// delayed import to the module already in memory. ORT_API_MANUAL_INIT matters
// for the same reason: without it the C++ header calls into the DLL during
// static initialisation, long before this code has had the chance to run.
#define ORT_API_MANUAL_INIT
#include "Separator.h"
#include "Stft.h"
#include <onnxruntime_cxx_api.h>
#if defined (_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace lowend::stems {

namespace {

constexpr int   kNumSources   = 4;
constexpr int   kModelFrames  = 336;
constexpr float kSegmentSecs  = 7.8f;
constexpr float kMaxShiftSecs = 0.5f;

/// Geometry of one segment, all of it forced by the network's input shape.
struct Geometry
{
    int samples = (int) (kSegmentSecs * kSampleRate);             // 343980
    int le      = (int) std::ceil ((double) samples / kHop);      // 336 frames
    int pad     = (kHop / 2) * 3;                                 // 1536
    int padEnd  = pad + le * kHop - samples;                      // 1620
    int padded  = samples + pad + padEnd;                         // 347136
};

/// Loads the ONNX Runtime that was installed beside this binary, once.
bool ensureRuntime()
{
    static const bool ok = []
    {
       #if defined (_WIN32)
        HMODULE self = nullptr;
        if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR> (&ensureRuntime), &self))
            return false;
        std::wstring path (32768, L'\0');
        const DWORD n = GetModuleFileNameW (self, path.data(), (DWORD) path.size());
        if (n == 0 || n >= path.size()) return false;
        path.resize (n);
        const auto slash = path.find_last_of (L"\\/");
        if (slash == std::wstring::npos) return false;
        path = path.substr (0, slash + 1) + L"onnxruntime.dll";
        if (LoadLibraryExW (path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
            return false;
       #endif
        Ort::InitApi();
        return true;
    }();
    return ok;
}

/// Holds the loaded network. Creating a session parses 170 MB of weights, so it
/// is done once per job, not once per segment.
class Net
{
public:
    Net (const std::string& path, int threads)
        : env_ (ORT_LOGGING_LEVEL_ERROR, "lowend")
    {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads (threads);
        so.SetInterOpNumThreads (1);
        so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
       #if defined (_WIN32)
        const std::wstring wide (path.begin(), path.end());
        session_ = std::make_unique<Ort::Session> (env_, wide.c_str(), so);
       #else
        session_ = std::make_unique<Ort::Session> (env_, path.c_str(), so);
       #endif
    }

    /// One segment. `x` is 4 x 2048 x 336, `xt` is 2 x samples; the outputs are
    /// 16 x 2048 x 336 and 8 x samples.
    void run (std::vector<float>& x, std::vector<float>& xt, int samples,
              std::vector<float>& xOut, std::vector<float>& xtOut)
    {
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> xShape  { 1, 4, kModelBins, kModelFrames };
        const std::array<int64_t, 3> xtShape { 1, 2, samples };

        std::array<Ort::Value, 2> inputs {
            Ort::Value::CreateTensor<float> (mem, x.data(), x.size(), xShape.data(), xShape.size()),
            Ort::Value::CreateTensor<float> (mem, xt.data(), xt.size(), xtShape.data(), xtShape.size())
        };
        static const char* inNames[]  { "x", "xt" };
        static const char* outNames[] { "x_out", "xt_out" };

        auto result = session_->Run (Ort::RunOptions { nullptr }, inNames, inputs.data(), 2, outNames, 2);
        const auto* xo  = result[0].GetTensorData<float>();
        const auto* xto = result[1].GetTensorData<float>();
        xOut.assign  (xo,  xo  + (size_t) 4 * kNumSources * kModelBins * kModelFrames);
        xtOut.assign (xto, xto + (size_t) 2 * kNumSources * samples);
    }

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
};

/// Mean and unbiased deviation over everything, then normalise in place.
void normalise (std::vector<float>& v, float& mean, float& sd)
{
    const double n = (double) v.size();
    mean = (float) (std::accumulate (v.begin(), v.end(), 0.0) / n);
    double sq = 0.0;
    for (auto f : v) { const double d = f - mean; sq += d * d; }
    sd = (float) std::sqrt (sq / std::max (1.0, n - 1.0));
    const float inv = 1.0f / (sd + 1.0e-5f);
    for (auto& f : v) f = (f - mean) * inv;
}

/// Step 4: one 7.8 s segment, both branches, in and out of the frequency
/// domain. `mix` is 2 x g.samples; `out` receives kNumSources x 2 x g.samples.
void separateSegment (Net& net, Stft& stft, const Geometry& g,
                      const std::vector<float>& mix, std::vector<float>& out)
{
    const int frames = stft.frames();
    std::vector<float> padded ((size_t) g.padded, 0.0f);
    std::vector<std::complex<float>> spec ((size_t) frames * kBins);
    std::vector<float> x ((size_t) 4 * kModelBins * kModelFrames);

    for (int ch = 0; ch < 2; ++ch)
    {
        // Reflect the segment out to the padded length Demucs transforms.
        const float* src = mix.data() + (size_t) ch * g.samples;
        for (int i = 0; i < g.pad; ++i)    padded[(size_t) (g.pad - 1 - i)] = src[std::min (i + 1, g.samples - 1)];
        std::copy (src, src + g.samples, padded.begin() + g.pad);
        for (int i = 0; i < g.padEnd; ++i) padded[(size_t) (g.pad + g.samples + i)] = src[std::max (0, g.samples - 2 - i)];

        stft.forward (padded.data(), g.padded, spec.data());

        // Drop two frames at each end and the Nyquist bin, then stack real and
        // imaginary parts as separate channels (complex-as-channels).
        for (int b = 0; b < kModelBins; ++b)
            for (int t = 0; t < kModelFrames; ++t)
            {
                const auto z = spec[(size_t) (t + kEdgeFrames) * kBins + (size_t) b];
                x[((size_t) (2 * ch)     * kModelBins + (size_t) b) * kModelFrames + (size_t) t] = z.real();
                x[((size_t) (2 * ch + 1) * kModelBins + (size_t) b) * kModelFrames + (size_t) t] = z.imag();
            }
    }

    float xMean = 0, xSd = 0, tMean = 0, tSd = 0;
    normalise (x, xMean, xSd);
    std::vector<float> xt = mix;
    normalise (xt, tMean, tSd);

    std::vector<float> xOut, xtOut;
    net.run (x, xt, g.samples, xOut, xtOut);

    // Back to audio, one source at a time.
    out.assign ((size_t) kNumSources * 2 * g.samples, 0.0f);
    std::vector<float> wave ((size_t) g.padded);
    for (int s = 0; s < kNumSources; ++s)
        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (spec.begin(), spec.end(), std::complex<float> {});
            for (int b = 0; b < kModelBins; ++b)
                for (int t = 0; t < kModelFrames; ++t)
                {
                    const size_t re = ((size_t) (s * 4 + 2 * ch)     * kModelBins + (size_t) b) * kModelFrames + (size_t) t;
                    const size_t im = ((size_t) (s * 4 + 2 * ch + 1) * kModelBins + (size_t) b) * kModelFrames + (size_t) t;
                    spec[(size_t) (t + kEdgeFrames) * kBins + (size_t) b] =
                        { xOut[re] * xSd + xMean, xOut[im] * xSd + xMean };
                }
            stft.inverse (spec.data(), g.padded, wave.data());

            // Trim the reflection padding, then add the time branch: a Demucs
            // stem is the sum of the spectral and waveform branches.
            float* dst = out.data() + ((size_t) s * 2 + (size_t) ch) * g.samples;
            const float* tb = xtOut.data() + ((size_t) s * 2 + (size_t) ch) * g.samples;
            for (int i = 0; i < g.samples; ++i)
                dst[i] = wave[(size_t) (g.pad + i)] + (tb[i] * tSd + tMean);
        }
}

/// Steps 3 and 5: the song in overlapping segments, cross-faded back together.
bool splitPass (Net& net, Stft& stft, const Geometry& g, const std::vector<float>& audio, int length,
                float overlap, const Progress& progress, float base, float span,
                std::vector<float>& out, std::string& error)
{
    const int stride = std::max (1, (int) ((1.0f - overlap) * (float) g.samples));
    out.assign ((size_t) kNumSources * 2 * length, 0.0f);
    std::vector<float> weightSum ((size_t) length, 0.0f);

    // Triangular weights: a segment contributes most at its centre, least at
    // its seams, so the joins are inaudible.
    std::vector<float> weight ((size_t) g.samples);
    for (int i = 0; i < g.samples; ++i) weight[(size_t) i] = (float) std::min (i + 1, g.samples - i);
    const float wMax = *std::max_element (weight.begin(), weight.end());
    for (auto& w : weight) w /= wMax;

    const int totalChunks = std::max (1, (int) std::ceil ((float) length / (float) stride));
    std::vector<float> chunk ((size_t) 2 * g.samples), chunkOut;
    int done = 0;

    for (int offset = 0; offset < length; offset += stride)
    {
        if (! progress (base + span * (float) done / (float) totalChunks, "Separating")) { error = "cancelled"; return false; }

        // A short final chunk is centred in an otherwise silent segment, and
        // the same offset finds the result again.
        const int chunkLength = std::min (g.samples, length - offset);
        const int leftPad = (g.samples - chunkLength) / 2;
        std::fill (chunk.begin(), chunk.end(), 0.0f);
        for (int ch = 0; ch < 2; ++ch)
            std::copy (audio.begin() + (size_t) ch * length + offset,
                       audio.begin() + (size_t) ch * length + offset + chunkLength,
                       chunk.begin() + (size_t) ch * g.samples + leftPad);

        separateSegment (net, stft, g, chunk, chunkOut);

        for (int s = 0; s < kNumSources; ++s)
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* src = chunkOut.data() + ((size_t) s * 2 + (size_t) ch) * g.samples + leftPad;
                float* dst = out.data() + ((size_t) s * 2 + (size_t) ch) * length + offset;
                for (int k = 0; k < chunkLength; ++k) dst[k] += weight[(size_t) k] * src[k];
            }
        for (int k = 0; k < chunkLength; ++k) weightSum[(size_t) (offset + k)] += weight[(size_t) k];
        ++done;
    }

    for (int s = 0; s < kNumSources; ++s)
        for (int ch = 0; ch < 2; ++ch)
        {
            float* dst = out.data() + ((size_t) s * 2 + (size_t) ch) * length;
            for (int k = 0; k < length; ++k)
                if (weightSum[(size_t) k] > 0.0f) dst[k] /= weightSum[(size_t) k];
        }
    return true;
}

} // namespace

bool separate (const std::string& onnxPath,
               const float* left, const float* right, int frames,
               const Options& options, const Progress& progress,
               std::array<std::vector<float>, 8>& out, std::string& error)
{
    if (frames <= 0) { error = "nothing to separate"; return false; }
    const Geometry g;

    if (! ensureRuntime())
    {
        error = "Stem separation is not installed correctly. Run the Low End installer again.";
        return false;
    }

    try
    {
        if (! progress (0.0f, "Loading model")) { error = "cancelled"; return false; }
        Net net (onnxPath, std::max (0, options.threads));   // 0 lets ONNX Runtime choose
        Stft stft (g.padded);

        // Normalise the song by its own mono mix, as apply.py does.
        double sum = 0.0;
        std::vector<float> mono ((size_t) frames);
        for (int i = 0; i < frames; ++i) { mono[(size_t) i] = 0.5f * (left[i] + right[i]); sum += mono[(size_t) i]; }
        const float refMean = (float) (sum / frames);
        double sq = 0.0;
        for (auto m : mono) { const double d = m - refMean; sq += d * d; }
        const float refStd = (float) std::sqrt (sq / std::max (1.0, (double) frames - 1.0));
        const float invStd = refStd > 0.0f ? 1.0f / refStd : 1.0f;

        const int maxShift = (int) (kMaxShiftSecs * kSampleRate);
        const int shifts = std::max (1, options.shifts);

        std::vector<float> accum ((size_t) kNumSources * 2 * frames, 0.0f);
        std::mt19937 rng (12345);                 // fixed: a song separates the same way twice
        std::uniform_int_distribution<int> pick (0, maxShift - 1);

        for (int shift = 0; shift < shifts; ++shift)
        {
            // Zero-pad by maxShift on each side and start the song `offset`
            // samples in; the result is shifted back by the same amount.
            const int offset = shifts == 1 ? maxShift / 2 : pick (rng);
            const int shiftedLength = frames + maxShift - offset;
            std::vector<float> shifted ((size_t) 2 * shiftedLength, 0.0f);
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* src = ch == 0 ? left : right;
                float* dst = shifted.data() + (size_t) ch * shiftedLength;
                for (int i = 0; i < shiftedLength; ++i)
                {
                    const int j = offset + i - maxShift;       // index into the song
                    dst[i] = (j >= 0 && j < frames) ? (src[j] - refMean) * invStd : 0.0f;
                }
            }

            std::vector<float> passOut;
            if (! splitPass (net, stft, g, shifted, shiftedLength, options.overlap, progress,
                             (float) shift / (float) shifts, 1.0f / (float) shifts, passOut, error))
                return false;

            const int trim = maxShift - offset;
            for (int s = 0; s < kNumSources; ++s)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* src = passOut.data() + ((size_t) s * 2 + (size_t) ch) * shiftedLength + trim;
                    float* dst = accum.data() + ((size_t) s * 2 + (size_t) ch) * frames;
                    for (int i = 0; i < frames; ++i) dst[i] += src[i];
                }
        }

        // Undo the song normalisation and hand the stems back.
        const float scale = refStd / (float) shifts;
        for (int s = 0; s < kNumSources; ++s)
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& dst = out[(size_t) (2 * s + ch)];
                dst.assign ((size_t) frames, 0.0f);
                const float* src = accum.data() + ((size_t) s * 2 + (size_t) ch) * frames;
                for (int i = 0; i < frames; ++i) dst[(size_t) i] = src[i] * scale + refMean;
            }

        progress (1.0f, "Done");
        return true;
    }
    // What went wrong inside the runtime is of no use to the person reading the
    // message; what they can do about it is.
    catch (const std::bad_alloc&) { error = "There was not enough memory to separate this song. Close other programs and try again."; return false; }
    catch (...) { error = "Separation stopped unexpectedly. Try again, and if it keeps happening, run the Low End installer again."; return false; }
}

} // namespace lowend::stems
