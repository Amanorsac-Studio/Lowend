// Looper.h — the stage looper.
//
// Four layers, one shared loop length, one-button transport
// (record -> play -> overdub -> play), quantised to the bar when you want it,
// with an exact undo of the last overdub.
//
// MEMORY. Every layer is allocated in `prepare` and never resized, so nothing
// the player does on stage can make the audio thread call the allocator. That
// costs 4 layers x 60 s x stereo x 4 bytes = about 92 MB at 48 kHz, plus one
// shared undo buffer. Sixty seconds is a deliberate ceiling: it covers any live
// loop, and the memory of a six-minute-per-layer looper (half a gigabyte) is not
// worth the two people who would use it.
//
// UNDO WITHOUT A MEMCPY. Copying a layer when an overdub starts would be a 23 MB
// copy on a footswitch press. Instead, each sample the overdub is about to
// change is written to the undo buffer FIRST, one sample at a time, and undo
// restores exactly the span that was actually covered — a bounded slice per
// block (see serviceUndo), so neither the stomp nor the undo costs a spike.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <vector>
#include "BlockParams.h"
#include "DspCommon.h"

namespace lowend {

class Looper
{
public:
    enum class State { idle = 0, recording, waitingForBar, playing, overdubbing, stopped };

    static constexpr double kMaxLoopSeconds = 60.0;

    //-------------------------------------------------------------- lifecycle
    /// Indices into `looperParams()`; verified in prepare.
    enum Idx { kLevel = 0, kDecay, kQuantize, kSpeed, kReverse, kFadeMs, kPlayThrough };

    void prepare (double sr, int /*maxBlock*/)
    {
        sr_ = sr > 0 ? sr : 48000.0;
        params.configure (looperParams());
        jassert (params.verify (kLevel, "level") && params.verify (kPlayThrough, "playThrough"));
        capacity_ = (int) (sr_ * kMaxLoopSeconds);
        for (auto& l : layers_) l.audio.assign ((size_t) capacity_ * 2, 0.0f);
        undo_.assign ((size_t) capacity_ * 2, 0.0f);
        clearAllImmediate();
    }

    /// MESSAGE THREAD. Values are read on the audio thread at the top of each
    /// block, so a knob move can never land halfway through a loop pass.
    void setParams (const ParamValues& v) { params.setAll (v); }

    ParamStore params;

    void setTempo (double bpm, int beatsPerBar = 4) noexcept
    {
        bpm_ = bpm > 1.0 ? bpm : 120.0;
        beatsPerBar_ = juce::jlimit (1, 16, beatsPerBar);
    }

    //-------------------------------------------------------------- transport
    // Safe from any thread: commands go into a lock-free ring and are applied on
    // the audio thread, in order, at the top of the next block.
    enum class Cmd { record = 1, stop, undo, clear, layerUp, layerDown };

    void push (Cmd c) noexcept
    {
        const auto i = writeIdx_.fetch_add (1, std::memory_order_acq_rel);
        cmdRing_[(size_t) (i % kRing)].store ((int) c, std::memory_order_release);
    }
    void recordPressed() noexcept { push (Cmd::record); }
    void stopPressed() noexcept { push (Cmd::stop); }
    void undoPressed() noexcept { push (Cmd::undo); }
    void clearPressed() noexcept { push (Cmd::clear); }
    void selectLayer (int i) noexcept { layer_ = juce::jlimit (0, kMaxLoopLayers - 1, i); }

    //------------------------------------------------------------------ state
    State state() const noexcept { return state_.load (std::memory_order_relaxed); }
    int lengthSamples() const noexcept { return length_.load (std::memory_order_relaxed); }
    double lengthSeconds() const noexcept { return lengthSamples() / sr_; }
    /// 0..1 through the loop, for the UI ring.
    double position() const noexcept
    {
        const int len = lengthSamples();
        return len > 0 ? juce::jlimit (0.0, 1.0, (double) pos_.load (std::memory_order_relaxed) / len) : 0.0;
    }
    int currentLayer() const noexcept { return layer_; }
    bool layerHasAudio (int i) const noexcept
    {
        return i >= 0 && i < kMaxLoopLayers && layers_[(size_t) i].hasAudio.load (std::memory_order_relaxed);
    }
    bool canUndo() const noexcept { return undoValid_.load (std::memory_order_relaxed); }
    /// Loop length in bars at the current tempo; 0 while free-running.
    double lengthBars() const noexcept
    {
        const double bar = barSamples();
        return bar > 0 ? lengthSamples() / bar : 0.0;
    }

    //---------------------------------------------------------------- process
    /// In place: the block carries the live signal in, and leaves with the loop
    /// mixed on top (and the live signal muted, if Play Through is off).
    void process (juce::AudioBuffer<float>& b)
    {
        level_ = juce::Decibels::decibelsToGain (params[kLevel]);
        decay_ = juce::jlimit (0.0f, 1.0f, params[kDecay] / 100.0f);
        quantize_ = juce::jlimit (0, 2, params.choice (kQuantize));
        halfSpeed_ = params.choice (kSpeed) == 1;
        reverse_ = params.flag (kReverse);
        fadeSamples_ = juce::jmax (8, (int) (sr_ * params[kFadeMs] * 0.001));
        playThrough_ = params.flag (kPlayThrough);

        applyCommands();
        serviceUndo();

        const int n = b.getNumSamples();
        const int nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0 || capacity_ <= 0) return;

        auto st = state_.load (std::memory_order_relaxed);
        const int len = length_.load (std::memory_order_relaxed);
        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;

        // Playback rate. Half speed reads at 0.5 samples per sample; reverse
        // walks backwards. Both use the same fractional reader, so a reversed
        // half-speed loop is not a special case.
        const double rate = (halfSpeed_ ? 0.5 : 1.0) * (reverse_ ? -1.0 : 1.0);

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i];
            const float inR = r ? r[i] : l[i];
            float outL = 0.0f, outR = 0.0f;

            switch (st)
            {
                case State::recording:
                {
                    const int p = recPos_;
                    if (p < capacity_)
                    {
                        auto& lay = layers_[(size_t) layer_];
                        lay.audio[(size_t) p * 2] = inL;
                        lay.audio[(size_t) p * 2 + 1] = inR;
                        recPos_ = p + 1;
                    }
                    else   // hit the ceiling: close the loop rather than stop dead
                    {
                        closeRecording();
                        st = state_.load (std::memory_order_relaxed);
                    }
                    break;
                }
                case State::waitingForBar:
                {
                    // Still recording, but only until the bar line the length is
                    // being rounded up to — so the "extra" is real playing, not
                    // silence bolted on the end.
                    const int p = recPos_;
                    if (p < capacity_)
                    {
                        auto& lay = layers_[(size_t) layer_];
                        lay.audio[(size_t) p * 2] = inL;
                        lay.audio[(size_t) p * 2 + 1] = inR;
                        recPos_ = p + 1;
                    }
                    if (recPos_ >= pendingLength_ || recPos_ >= capacity_)
                    {
                        length_.store (juce::jmax (1, juce::jmin (recPos_, capacity_)), std::memory_order_relaxed);
                        layers_[(size_t) layer_].hasAudio.store (true, std::memory_order_relaxed);
                        pos_.store (0, std::memory_order_relaxed);
                        readPos_ = 0.0;
                        state_.store (State::playing, std::memory_order_relaxed);
                        st = State::playing;
                    }
                    break;
                }
                case State::playing:
                case State::overdubbing:
                {
                    if (len <= 0) break;
                    // ---- read every layer at the shared position
                    const double rp = readPos_;
                    for (int k = 0; k < kMaxLoopLayers; ++k)
                    {
                        auto& lay = layers_[(size_t) k];
                        if (! lay.hasAudio.load (std::memory_order_relaxed)) continue;
                        outL += readInterp (lay.audio, len, rp, 0);
                        outR += readInterp (lay.audio, len, rp, 1);
                    }
                    // Seam fade: a short ramp at both ends of the loop, so the
                    // wrap can never click no matter where the player stomped.
                    const float g = seamGain ((int) rp, len);
                    outL *= g; outR *= g;

                    if (st == State::overdubbing)
                    {
                        const int p = (int) rp;
                        if (p >= 0 && p < len)
                        {
                            auto& lay = layers_[(size_t) layer_];
                            const size_t idx = (size_t) p * 2;
                            if (undoWritten_ < len)      // save-before-write, one sample at a time
                            {
                                undo_[idx] = lay.audio[idx];
                                undo_[idx + 1] = lay.audio[idx + 1];
                                ++undoWritten_;
                            }
                            // The saved span starts wherever the loop happened to
                            // be when the overdub was stomped, and wraps. Undo has
                            // to walk the SAME span, so remember where it began.
                            if (undoWritten_ == 1) undoStart_ = p;
                            const float keep = 1.0f - decay_;
                            lay.audio[idx] = lay.audio[idx] * keep + inL;
                            lay.audio[idx + 1] = lay.audio[idx + 1] * keep + inR;
                            lay.hasAudio.store (true, std::memory_order_relaxed);
                        }
                    }

                    readPos_ += rate;
                    if (readPos_ >= len) readPos_ -= len;
                    if (readPos_ < 0) readPos_ += len;
                    pos_.store ((int) readPos_, std::memory_order_relaxed);
                    break;
                }
                case State::idle:
                case State::stopped:
                default:
                    break;
            }

            const bool loopAudible = (st == State::playing || st == State::overdubbing);
            const float thru = (playThrough_ || ! loopAudible) ? 1.0f : 0.0f;
            l[i] = inL * thru + outL * level_;
            if (r) r[i] = inR * thru + outR * level_;
        }
    }

    /// Called by RESET/panic: silence, keep the audio.
    void stopAll() noexcept { push (Cmd::stop); }

    /// MESSAGE THREAD. Renders the mixed loop to a file for after the gig.
    bool exportToFile (const juce::File& dest) const
    {
        const int len = lengthSamples();
        if (len <= 0) return false;
        juce::AudioBuffer<float> mix (2, len);
        mix.clear();
        for (auto& lay : layers_)
        {
            if (! lay.hasAudio.load()) continue;
            for (int i = 0; i < len; ++i)
            {
                mix.addSample (0, i, lay.audio[(size_t) i * 2]);
                mix.addSample (1, i, lay.audio[(size_t) i * 2 + 1]);
            }
        }
        juce::WavAudioFormat wav;
        dest.getParentDirectory().createDirectory();
        if (auto out = std::unique_ptr<juce::FileOutputStream> (dest.createOutputStream()))
        {
            out->setPosition (0);
            out->truncate();
            if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                    wav.createWriterFor (out.get(), sr_, 2, 24, {}, 0)))
            {
                out.release();
                return writer->writeFromAudioSampleBuffer (mix, 0, len);
            }
        }
        return false;
    }

private:
    //---------------------------------------------------------------- helpers
    double barSamples() const noexcept { return sr_ * 60.0 / bpm_ * beatsPerBar_; }
    double beatSamples() const noexcept { return sr_ * 60.0 / bpm_; }

    static float readInterp (const std::vector<float>& a, int len, double pos, int ch) noexcept
    {
        if (len <= 1) return 0.0f;
        int i0 = (int) pos;
        if (i0 < 0) i0 = 0; else if (i0 >= len) i0 = len - 1;
        int i1 = i0 + 1; if (i1 >= len) i1 = 0;
        const float f = (float) (pos - (double) i0);
        return a[(size_t) i0 * 2 + (size_t) ch] * (1.0f - f) + a[(size_t) i1 * 2 + (size_t) ch] * f;
    }

    float seamGain (int p, int len) const noexcept
    {
        const int f = juce::jmin (fadeSamples_, len / 4);
        if (f <= 0) return 1.0f;
        if (p < f) return (float) p / (float) f;
        if (p > len - f) return (float) (len - p) / (float) f;
        return 1.0f;
    }

    void closeRecording() noexcept
    {
        const int recorded = juce::jmax (1, recPos_);
        int target = recorded;
        if (quantize_ != 0)
        {
            const double unit = quantize_ == 1 ? barSamples() : beatSamples();
            if (unit > 0)
            {
                const double units = juce::jmax (1.0, std::round (recorded / unit));
                target = (int) std::round (units * unit);
            }
        }
        if (target > recorded && target <= capacity_)
        {
            // Keep the take running until the bar line.
            pendingLength_ = target;
            state_.store (State::waitingForBar, std::memory_order_relaxed);
            return;
        }
        length_.store (juce::jmax (1, juce::jmin (target, juce::jmin (recorded, capacity_))), std::memory_order_relaxed);
        layers_[(size_t) layer_].hasAudio.store (true, std::memory_order_relaxed);
        readPos_ = 0.0;
        pos_.store (0, std::memory_order_relaxed);
        state_.store (State::playing, std::memory_order_relaxed);
    }

    /// Restores a bounded slice of an armed undo, once per block.
    ///
    /// 64k frames is a 512 KB copy — tens of microseconds, about 1 % of a
    /// 256-sample block's budget — so this is invisible in the timing, and a
    /// loop of any realistic length is fully restored within a few blocks
    /// (a 4 s loop in three, a 60 s loop in about a fifth of a second). The
    /// restore walks forward from where the overdub began, so the material the
    /// playhead reaches next is the first to come back.
    static constexpr int kUndoChunkFrames = 65536;

    void serviceUndo() noexcept
    {
        if (! undoActive_) return;
        const int len = length_.load (std::memory_order_relaxed);
        if (len <= 0) { undoActive_ = false; return; }
        auto& lay = layers_[(size_t) undoLayer_];
        const int todo = juce::jmin (undoRemaining_, kUndoChunkFrames);
        for (int i = 0; i < todo; ++i)
        {
            int p = undoStart_ + undoCursor_ + i;
            while (p >= len) p -= len;                 // the span wraps the loop
            const size_t idx = (size_t) p * 2;
            lay.audio[idx] = undo_[idx];
            lay.audio[idx + 1] = undo_[idx + 1];
        }
        undoCursor_ += todo;
        undoRemaining_ -= todo;
        if (undoRemaining_ <= 0) undoActive_ = false;
    }

    void applyCommands() noexcept
    {
        const auto w = writeIdx_.load (std::memory_order_acquire);
        while (readIdx_ != w)
        {
            const int c = cmdRing_[(size_t) (readIdx_ % kRing)].exchange (0, std::memory_order_acq_rel);
            ++readIdx_;
            if (c == 0) continue;
            apply ((Cmd) c);
        }
    }

    void apply (Cmd c) noexcept
    {
        auto st = state_.load (std::memory_order_relaxed);
        switch (c)
        {
            case Cmd::record:
                switch (st)
                {
                    case State::idle:
                        recPos_ = 0;
                        layers_[(size_t) layer_].hasAudio.store (false, std::memory_order_relaxed);
                        state_.store (State::recording, std::memory_order_relaxed);
                        break;
                    case State::recording:
                        closeRecording();
                        break;
                    case State::stopped:
                        if (length_.load() > 0) { readPos_ = 0.0; state_.store (State::playing, std::memory_order_relaxed); }
                        else { recPos_ = 0; state_.store (State::recording, std::memory_order_relaxed); }
                        break;
                    case State::playing:
                        undoWritten_ = 0;
                        undoLayer_ = layer_;
                        undoValid_.store (true, std::memory_order_relaxed);
                        state_.store (State::overdubbing, std::memory_order_relaxed);
                        break;
                    case State::overdubbing:
                        state_.store (State::playing, std::memory_order_relaxed);
                        break;
                    default: break;
                }
                break;

            case Cmd::stop:
                if (st == State::recording || st == State::waitingForBar) closeRecording();
                state_.store (length_.load() > 0 ? State::stopped : State::idle, std::memory_order_relaxed);
                break;

            case Cmd::undo:
            {
                if (! undoValid_.load (std::memory_order_relaxed)) break;
                // Restoring the span here in one go would be O(loop length) INSIDE
                // one audio block: a 60 s loop is 5.8 million float copies, which
                // the soak test measured as a 7 ms block — a dropout. So undo only
                // ARMS here, and `serviceUndo` restores a bounded chunk per block.
                const int len = length_.load (std::memory_order_relaxed);
                undoRemaining_ = juce::jmin (undoWritten_, len);
                undoCursor_ = 0;
                undoActive_ = undoRemaining_ > 0;
                undoValid_.store (false, std::memory_order_relaxed);
                undoWritten_ = 0;
                if (st == State::overdubbing) state_.store (State::playing, std::memory_order_relaxed);
                break;
            }

            case Cmd::clear:
                // Audio-thread safe: CLEAR does not touch the sample data. Zeroing
                // 92 MB here would be a guaranteed dropout, and it is not needed —
                // nothing reads a layer whose `hasAudio` is false, and the next
                // recording overwrites from sample zero within the new length.
                resetTransport();
                break;

            case Cmd::layerUp:
                layer_ = (layer_ + 1) % kMaxLoopLayers;
                break;
            case Cmd::layerDown:
                layer_ = (layer_ + kMaxLoopLayers - 1) % kMaxLoopLayers;
                break;
        }
    }

    /// MESSAGE THREAD ONLY (prepare): zero the sample data as well.
    void clearAllImmediate()
    {
        for (auto& lay : layers_) std::fill (lay.audio.begin(), lay.audio.end(), 0.0f);
        std::fill (undo_.begin(), undo_.end(), 0.0f);
        resetTransport();
    }

    /// Audio-thread safe: flags and positions only.
    void resetTransport() noexcept
    {
        for (auto& lay : layers_) lay.hasAudio.store (false, std::memory_order_relaxed);
        undoValid_.store (false, std::memory_order_relaxed);
        undoWritten_ = 0;
        length_.store (0, std::memory_order_relaxed);
        pos_.store (0, std::memory_order_relaxed);
        recPos_ = 0; readPos_ = 0.0; pendingLength_ = 0; layer_ = 0; undoStart_ = 0;
        undoRemaining_ = 0; undoCursor_ = 0; undoActive_ = false;
        state_.store (State::idle, std::memory_order_relaxed);
    }

    struct Layer
    {
        std::vector<float> audio;               // interleaved stereo
        std::atomic<bool> hasAudio { false };
    };

    static constexpr int kRing = 32;

    double sr_ = 48000.0, bpm_ = 120.0;
    int beatsPerBar_ = 4, capacity_ = 0;

    std::array<Layer, kMaxLoopLayers> layers_;
    std::vector<float> undo_;

    std::atomic<State> state_ { State::idle };
    std::atomic<int> length_ { 0 }, pos_ { 0 };
    std::atomic<bool> undoValid_ { false };

    int recPos_ = 0, pendingLength_ = 0, layer_ = 0, undoLayer_ = 0, undoWritten_ = 0, undoStart_ = 0;
    int undoRemaining_ = 0, undoCursor_ = 0;
    bool undoActive_ = false;
    double readPos_ = 0.0;

    float level_ = 1.0f, decay_ = 0.0f;
    int quantize_ = 1, fadeSamples_ = 288;
    bool halfSpeed_ = false, reverse_ = false, playThrough_ = true;

    std::array<std::atomic<int>, kRing> cmdRing_ {};
    std::atomic<uint32_t> writeIdx_ { 0 };
    uint32_t readIdx_ = 0;
};

} // namespace lowend
