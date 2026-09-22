// StemSeparator.cpp — see header.
#include "StemSeparator.h"
#include "Separator.h"
#include "../engine/RigModel.h"

namespace lowend {

namespace {
    // Meta's HT-Demucs v4, exported to ONNX by Intel (MIT licensed, and the same
    // weights as the original). Two files: the graph, and the tensor data it
    // references by name — ONNX Runtime needs them side by side under exactly
    // these names.
    //
    // THE MODEL IS INSTALLED, NOT DOWNLOADED. An earlier build fetched it on first
    // use; the studio's privacy policy says a product makes no network request
    // other than licensing, and a 171 MB fetch from a third party is exactly the
    // kind of thing that promise rules out. The installer places it (as a
    // component the buyer can untick) and this code only ever reads it.
    struct ModelFile { const char* name; juce::int64 bytes; };
    constexpr ModelFile kModelFiles[] = {
        { "htdemucs_fwd.onnx",        2385507 },
        { "htdemucs_fwd.onnx.data", 168361984 },
    };
    constexpr const char* kGraphFile = "htdemucs_fwd.onnx";

    bool holdsModel (const juce::File& dir)
    {
        for (auto& f : kModelFiles)
            if (dir.getChildFile (f.name).getSize() != f.bytes) return false;
        return true;
    }

    /// What each quality actually costs. Standard is one pass at Demucs' default
    /// overlap; best averages two time-shifted passes at double the overlap,
    /// which is roughly four times the work for a few tenths of a dB.
    stems::Options qualityOptions (StemSeparator::Quality q)
    {
        stems::Options o;
        if (q == StemSeparator::Quality::best) { o.shifts = 2; o.overlap = 0.5f; }
        return o;
    }
}

const char* StemSeparator::stemName (int i)
{
    static const char* names[kNumStems] = { "drums", "bass", "other", "vocals" };
    return (i >= 0 && i < kNumStems) ? names[i] : "";
}

//==============================================================================
StemSeparator::StemSeparator() : juce::Thread ("Low End stem separation") {}

StemSeparator::~StemSeparator()
{
    cancel_.store (true);
    stopThread (30000);
}

juce::File StemSeparator::modelsDir()
{
    // Where the installer put it first; then beside the binary and the old
    // Documents location, which is where a development machine keeps it.
    const juce::File candidates[] = {
        factoryDir().getChildFile ("Models"),
        juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("Models"),
        appDataDir().getChildFile ("Models") };
    for (auto& c : candidates) if (holdsModel (c)) return c;
    return candidates[0];
}
juce::File StemSeparator::learnDir()  { return appDataDir().getChildFile ("Learn"); }
juce::File StemSeparator::stemDir (const juce::String& id) { return learnDir().getChildFile (id); }

bool StemSeparator::validId (const juce::String& id)
{
    return id.isNotEmpty() && id.length() <= 40 && id.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789");
}

bool StemSeparator::cpuSupported()
{
    // ONNX Runtime selects its kernels from the instruction sets it finds at run
    // time, so there is no longer a CPU that cannot separate — only ones that
    // take longer. Kept so the UI has one place to ask.
    return true;
}

bool StemSeparator::modelReady() const { return holdsModel (modelsDir()); }

//==============================================================================
bool StemSeparator::beginUpload (const juce::String& id, int frames)
{
    if (! validId (id) || frames <= 0 || frames > kSampleRate * 60 * 20) return false;   // 20 minutes at most
    if (isThreadRunning()) return false;
    const juce::ScopedLock sl (lock_);
    audio_.setSize (2, frames, false, true, false);
    audio_.clear();
    expectedFrames_ = frames;
    receivedFrames_ = 0;
    status_ = {};
    status_.jobId = id;
    status_.state = State::receiving;
    status_.stage = "Preparing";
    return true;
}

bool StemSeparator::appendChunk (const juce::String& id, int offset, const juce::String& b64)
{
    const juce::ScopedLock sl (lock_);
    if (status_.jobId != id || status_.state != State::receiving) return false;
    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, b64)) return false;
    const auto* s = static_cast<const juce::int16*> (decoded.getData());
    const int frames = (int) (decoded.getDataSize() / 4);          // stereo int16
    if (offset < 0 || offset + frames > expectedFrames_) return false;
    auto* L = audio_.getWritePointer (0);
    auto* R = audio_.getWritePointer (1);
    // The page sends little-endian int16; swap on a big-endian host, then read signed.
    for (int i = 0; i < frames; ++i)
    {
        L[offset + i] = (float) (juce::int16) juce::ByteOrder::swapIfBigEndian ((juce::uint16) s[2 * i]) / 32768.0f;
        R[offset + i] = (float) (juce::int16) juce::ByteOrder::swapIfBigEndian ((juce::uint16) s[2 * i + 1]) / 32768.0f;
    }
    receivedFrames_ = juce::jmax (receivedFrames_, offset + frames);
    status_.progress = 0.0f;
    return true;
}

bool StemSeparator::start (const juce::String& id, Quality q)
{
    {
        const juce::ScopedLock sl (lock_);
        if (status_.jobId != id || status_.state != State::receiving) return false;
        if (receivedFrames_ < expectedFrames_) return false;
        if (! modelReady()) { status_.state = State::error; status_.error = "Stem separation is not installed. Run the Low End installer again and tick Stem separation."; return false; }
        quality_ = q;
        status_.state = State::loading;
        status_.stage = "Loading model";
    }
    cancel_.store (false);
    startThread (juce::Thread::Priority::low);   // a long job; never compete with the audio thread
    return true;
}

void StemSeparator::cancel()
{
    cancel_.store (true);
    const juce::ScopedLock sl (lock_);
    // A job still TAKING IN audio has no thread yet, so there is nothing to
    // notice the flag: it has to be dropped here, or the app shows "Preparing"
    // for ever and keeps hold of a few hundred megabytes of song. This is the
    // path taken when the player changes lesson mid-upload.
    if (status_.state == State::receiving)
    {
        status_.state = State::cancelled;
        status_.stage = {};
        status_.jobId = {};
        audio_.setSize (0, 0);
        expectedFrames_ = receivedFrames_ = 0;
    }
}

StemSeparator::Status StemSeparator::status() const
{
    const juce::ScopedLock sl (lock_);
    auto s = status_;
    if (s.state == State::loading || s.state == State::separating) s.progress = progressAtomic_.load();
    return s;
}

bool StemSeparator::hasStems (const juce::String& id)
{
    if (! validId (id)) return false;
    for (int i = 0; i < kNumStems; ++i)
        if (! stemDir (id).getChildFile (juce::String (stemName (i)) + ".wav").existsAsFile()) return false;
    return true;
}

void StemSeparator::deleteStems (const juce::String& id)
{
    if (validId (id)) stemDir (id).deleteRecursively();
}

void StemSeparator::setStage (State s, const juce::String& stage, float p)
{
    const juce::ScopedLock sl (lock_);
    status_.state = s;
    status_.stage = stage;
    status_.progress = p;
    progressAtomic_.store (p);
}

void StemSeparator::fail (const juce::String& why)
{
    const juce::ScopedLock sl (lock_);
    status_.state = why == "cancelled" ? State::cancelled : State::error;
    status_.error = why == "cancelled" ? juce::String() : why;
    audio_.setSize (0, 0);
}

//==============================================================================
void StemSeparator::run()
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    juce::String id;
    int frames = 0;
    {
        const juce::ScopedLock sl (lock_);
        id = status_.jobId;
        frames = expectedFrames_;
    }

    const auto graph = modelsDir().getChildFile (kGraphFile).getFullPathName().toStdString();

    // Leave a couple of cores free: separation is heavy, and the app — including
    // its audio — has to stay responsive while it runs in the background.
    auto options = qualityOptions (quality_);
    options.threads = juce::jmax (1, juce::SystemStats::getNumCpus() - 2);

    std::array<std::vector<float>, 8> out;
    std::string error;
    setStage (State::separating, "Separating", 0.0f);
    const bool ok = stems::separate (graph, audio_.getReadPointer (0), audio_.getReadPointer (1), frames, options,
        [this] (float p, const std::string& stage)
        {
            progressAtomic_.store (juce::jlimit (0.0f, 1.0f, p) * 0.97f);
            {
                const juce::ScopedLock sl (lock_);
                status_.stage = stage;
            }
            return ! cancel_.load() && ! threadShouldExit();
        },
        out, error);

    if (! ok) { fail (error); return; }

    setStage (State::writing, "Saving stems", 0.97f);
    const auto dir = stemDir (id);
    dir.createDirectory();
    juce::WavAudioFormat wav;
    for (int s = 0; s < kNumStems; ++s)
    {
        const auto file = dir.getChildFile (juce::String (stemName (s)) + ".wav");
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate (kSampleRate)
                                                      .withNumChannels (2)
                                                      .withBitsPerSample (16));
        if (writer == nullptr) { fail ("Could not write " + file.getFileName()); return; }
        const float* chans[2] = { out[(size_t) (2 * s)].data(), out[(size_t) (2 * s + 1)].data() };
        writer->writeFromFloatArrays (chans, 2, frames);
    }

    const double secs = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    auto manifest = juce::var (new juce::DynamicObject());
    manifest.getDynamicObject()->setProperty ("stems", juce::StringArray { "drums", "bass", "other", "vocals" });
    manifest.getDynamicObject()->setProperty ("sampleRate", kSampleRate);
    manifest.getDynamicObject()->setProperty ("frames", frames);
    manifest.getDynamicObject()->setProperty ("model", quality_ == Quality::best ? "htdemucs (2 shifts)" : "htdemucs");
    manifest.getDynamicObject()->setProperty ("seconds", secs);
    dir.getChildFile ("stems.json").replaceWithText (juce::JSON::toString (manifest));

    const juce::ScopedLock sl (lock_);
    audio_.setSize (0, 0);
    status_.state = State::done;
    status_.stage = "Done";
    status_.progress = 1.0f;
    status_.seconds = secs;
    progressAtomic_.store (1.0f);
}

} // namespace lowend
