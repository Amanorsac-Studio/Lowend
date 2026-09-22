// StemSeparator.h — splits a song into drums, bass, other and vocals.
//
// The model is Meta's HT-Demucs (hybrid transformer Demucs v4), run on ONNX
// Runtime from Intel's MIT-licensed export of the original weights — see
// Separator.cpp for why, and for what the surrounding audio pipeline does. Two
// qualities, both using the same 171 MB model:
//
//   Standard — one pass, 25 % segment overlap. About 0.5x real time, so a
//              four-minute song takes two or three minutes.
//   Best     — two time-shifted passes averaged, 50 % overlap: roughly four
//              times the work for a few tenths of a dB.
//
// The model is placed by the installer (a component the buyer can untick) and
// only ever read here: the product makes no network request for it.
//
// THREADING. Everything here runs on its own background thread; nothing touches
// the audio thread. ONNX Runtime uses the cores it is given internally, so the
// song's segments are separated one after another, and cancelling is simply a
// flag the progress callback reports between segments.
//
// The page hands the audio over in chunks (stereo, 44.1 kHz, 16-bit, base64),
// because the song lives in the web view, not on disk. The stems are written as
// WAV files to Documents/Amanorsac Studio/Low End/Learn/<lesson>/ and served back
// to the page by the editor's resource provider.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>

namespace lowend {

class StemSeparator final : private juce::Thread
{
public:
    enum class Quality { standard = 0, best = 1 };
    enum class State { idle, receiving, loading, separating, writing, done, error, cancelled };

    /// Demucs output order. The WAV files are named after these.
    static const char* stemName (int i);
    static constexpr int kNumStems = 4;
    static constexpr int kSampleRate = 44100;

    struct Status
    {
        State state = State::idle;
        juce::String jobId, stage, error;
        float progress = 0.0f;              // 0..1 over the whole job
        double seconds = 0.0;               // wall time of the finished job
    };

    StemSeparator();
    ~StemSeparator() override;

    static juce::File modelsDir();
    static juce::File learnDir();
    static juce::File stemDir (const juce::String& lessonId);
    static bool validId (const juce::String& id);
    /// Whether this machine can separate at all. Always true now that ONNX
    /// Runtime picks its kernels at run time; kept as the UI's single check.
    static bool cpuSupported();

    /// True when the installed model is present and complete.
    bool modelReady() const;

    /// MESSAGE THREAD. The page sends the decoded song in chunks.
    bool beginUpload (const juce::String& lessonId, int frames);
    bool appendChunk (const juce::String& lessonId, int offsetFrames, const juce::String& base64Int16Stereo);
    /// MESSAGE THREAD. Runs the job on the background thread.
    bool start (const juce::String& lessonId, Quality);
    void cancel();

    Status status() const;
    /// True if this lesson already has all four stems on disk.
    static bool hasStems (const juce::String& lessonId);
    static void deleteStems (const juce::String& lessonId);

private:
    void run() override;
    void setStage (State s, const juce::String& stage, float progress);
    void fail (const juce::String& why);

    mutable juce::CriticalSection lock_;
    Status status_;
    juce::AudioBuffer<float> audio_;       // the song being separated, 44.1 kHz stereo
    int expectedFrames_ = 0, receivedFrames_ = 0;
    Quality quality_ = Quality::standard;
    std::atomic<bool> cancel_ { false };
    std::atomic<float> progressAtomic_ { 0.0f };
};

} // namespace lowend
