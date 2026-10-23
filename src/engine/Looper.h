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
// THE SEAM. A loop is only as good as its wrap. Fading the level down and up
// at the loop point (what this did first) is click-free but leaves a hole once
// a pass, which on a held bass note is a thump. Instead the take keeps
// recording for a few milliseconds after the stomp, and that "tail" — what the
// player actually played next — is crossfaded into the head of the loop. The
// end of the loop now flows into a head that starts the way the playing went
// on, so there is no dip and no click. The fade is only a fallback for a loop
// that could not be sealed (stopped within the fade time, or at the length
// ceiling).
//
// TIMING. A footswitch arrives with its sample position inside the block, and
// the command is applied at THAT sample, not at the top of the next block — a
// buffer of error per stomp made every loop a slightly different length.
//
// QUANTISE needs a tempo the player can hear. Snapping to a bar of a tempo
// nobody hears chops or pads a free loop, so Bar and Beat only apply while
// `setGridActive (true)` — the processor sets it when the click is running,
// a count-in was used, or a host is providing the tempo.
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
    enum class State { idle = 0, recording, waitingForBar, playing, overdubbing, stopped, armed, cued };

    static constexpr double kMaxLoopSeconds = 60.0;

    //-------------------------------------------------------------- lifecycle
    /// Indices into `looperParams()`; verified in prepare.
    enum Idx { kLevel = 0, kDecay, kQuantize, kSpeed, kReverse, kFadeMs, kPlayThrough, kAutoStart };

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
    // the audio thread, in order. `offset` is the sample inside the NEXT block
    // at which the command takes effect — a footswitch passes its MIDI event's
    // position; the UI (which has no sample clock) passes 0.
    enum class Cmd { record = 1, stop, undo, clear, layerUp, layerDown };

    void push (Cmd c, int offset = 0) noexcept
    {
        const int off = juce::jlimit (0, kMaxOffset, offset);
        const auto i = writeIdx_.fetch_add (1, std::memory_order_acq_rel);
        cmdRing_[(size_t) (i % kRing)].store ((off << kCmdBits) | (int) c, std::memory_order_release);
    }
    void recordPressed (int offset = 0) noexcept { push (Cmd::record, offset); }
    void stopPressed (int offset = 0) noexcept { push (Cmd::stop, offset); }
    void undoPressed (int offset = 0) noexcept { push (Cmd::undo, offset); }
    void clearPressed (int offset = 0) noexcept { push (Cmd::clear, offset); }
    void selectLayer (int i) noexcept { layer_ = juce::jlimit (0, kMaxLoopLayers - 1, i); }

    /// True while the player can hear the tempo (click, count-in or host), so
    /// Bar/Beat quantise means something. Any thread.
    void setGridActive (bool on) noexcept { gridActive_.store (on, std::memory_order_relaxed); }
    /// AUDIO THREAD, once per block before `process`: where the click's bar is.
    /// `phase` is the samples since its bar began at this block's first
    /// sample. A bar of 0 means the click is not running: the looper keeps
    /// its own grid from the tempo.
    void setClickGrid (double phase, double bar, double pulse) noexcept { extPhase_ = phase; extBar_ = bar; extPulse_ = pulse; }
    /// A fixed loop length in bars (0 = free). Any thread; read per block.
    void setFixedBars (int bars) noexcept { fixedBars_.store (juce::jlimit (0, 16, bars), std::memory_order_relaxed); }
    int positionSamples() const noexcept { return pos_.load (std::memory_order_relaxed); }
    bool gridActive() const noexcept { return gridActive_.load (std::memory_order_relaxed); }

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
        autoStart_ = params.flag (kAutoStart);

        drainCommands();
        serviceUndo();

        const int n = b.getNumSamples();
        const int nCh = juce::jmin (2, b.getNumChannels());
        if (n <= 0 || nCh <= 0 || capacity_ <= 0)
        {
            for (int k = 0; k < nPending_; ++k) apply (pending_[(size_t) k].cmd);
            nPending_ = 0;
            return;
        }

        auto st = state_.load (std::memory_order_relaxed);
        int len = length_.load (std::memory_order_relaxed);
        int nextCmd = 0;
        const int bars = fixedBars_.load (std::memory_order_relaxed);
        const int fixedLen = bars > 0 ? juce::jmin (capacity_, (int) std::round (bars * barSamples())) : 0;
        auto* l = b.getWritePointer (0);
        auto* r = nCh > 1 ? b.getWritePointer (1) : nullptr;

        // Playback rate. Half speed reads at 0.5 samples per sample; reverse
        // walks backwards. Both use the same fractional reader, so a reversed
        // half-speed loop is not a special case.
        const double rate = (halfSpeed_ ? 0.5 : 1.0) * (reverse_ ? -1.0 : 1.0);

        for (int i = 0; i < n; ++i)
        {
            // Commands land on their own sample. The last sample of the block
            // also takes any whose offset ran past the end.
            while (nextCmd < nPending_ && (pending_[(size_t) nextCmd].offset <= i || i == n - 1))
            {
                curI_ = i;
                apply (pending_[(size_t) nextCmd++].cmd);
                st = state_.load (std::memory_order_relaxed);
                len = length_.load (std::memory_order_relaxed);
            }

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
                        if (fixedLen > 0 && recPos_ >= fixedLen)
                        {
                            closeRecording (false);       // exactly the bars asked for
                            st = state_.load (std::memory_order_relaxed);
                            len = length_.load (std::memory_order_relaxed);
                        }
                    }
                    else   // hit the ceiling: close the loop rather than stop dead
                    {
                        closeRecording();
                        st = state_.load (std::memory_order_relaxed);
                    }
                    break;
                }
                case State::armed:
                {
                    // Recording from the count-in's downbeat, but the take has not
                    // started: it starts on the bar in which the first note is
                    // played (see startOnFirstNote). A note within a sixteenth
                    // before a bar line is a pickup into that bar: the take starts
                    // at the bar line, so `startAt_` can be ahead of now.
                    const int p = recPos_;
                    if (p < capacity_)
                    {
                        auto& lay = layers_[(size_t) layer_];
                        lay.audio[(size_t) p * 2] = inL;
                        lay.audio[(size_t) p * 2 + 1] = inR;
                        recPos_ = p + 1;
                    }
                    else { state_.store (State::idle, std::memory_order_relaxed); st = State::idle; break; }
                    if (autoStart_ && startAt_ < 0 && (std::abs (inL) > kStartLevel || std::abs (inR) > kStartLevel))
                        startOnFirstNote (p, clickPhaseAt (i));
                    if (startAt_ >= 0 && recPos_ >= startAt_)
                    {
                        // The take starts here (a pickup before the bar line is not kept).
                        recPos_ = 0;
                        startAt_ = -1;
                        state_.store (State::recording, std::memory_order_relaxed);
                        st = State::recording;
                    }
                    break;
                }
                case State::cued:
                {
                    // Waiting for the click's next "1" to play the loop from its
                    // top, so a loop restarted after STOP is on the click again.
                    const double ph = clickPhaseAt (i);
                    if (ph < 0.0 || ph < 1.0)
                    {
                        readPos_ = 0.0;
                        pos_.store (0, std::memory_order_relaxed);
                        state_.store (State::playing, std::memory_order_relaxed);
                        st = State::playing;
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
                        len = juce::jmax (1, juce::jmin (recPos_, capacity_));
                        length_.store (len, std::memory_order_relaxed);
                        layers_[(size_t) layer_].hasAudio.store (true, std::memory_order_relaxed);
                        pos_.store (0, std::memory_order_relaxed);
                        readPos_ = 0.0;
                        beginSeal (len, recPos_);
                        st = stopAfterClose_ ? State::stopped : State::playing;
                        if (st == State::stopped) sealing_ = false;
                        state_.store (st, std::memory_order_relaxed);
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
                    // Fallback seam fade, only while the loop is not (yet) sealed.
                    const float g = seamGain ((int) rp, len);
                    outL *= g; outR *= g;

                    // Seal: keep capturing what the player plays after the stomp,
                    // and crossfade it into the head once the head has been heard.
                    if (sealing_) serviceSeal (len, (int) rp, inL, inR);

                    if (st == State::overdubbing)
                    {
                        const int p = (int) rp;
                        // At half speed every position is read twice; it is
                        // written once, so an overdub adds at the level played.
                        if (p >= 0 && p < len && p != lastDubPos_)
                        {
                            lastDubPos_ = p;
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
    double barSamples() const noexcept { return extBar_ > 0.0 ? extBar_ : sr_ * 60.0 / bpm_ * beatsPerBar_; }
    double beatSamples() const noexcept { return extBar_ > 0.0 ? extPulse_ : sr_ * 60.0 / bpm_; }
    bool gridOn() const noexcept { return quantize_ != 0 && gridActive_.load (std::memory_order_relaxed); }
    /// The click's position (samples into its bar) at sample i of this block, or -1 without a click.
    double clickPhaseAt (int i) const noexcept
    {
        if (extBar_ <= 0.0) return -1.0;
        double ph = std::fmod (extPhase_ + i, extBar_);
        return ph < 0.0 ? ph + extBar_ : ph;
    }

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
        if (sealed_) return 1.0f;                        // the wrap is continuous
        const int f = juce::jmin (fadeSamples_, len / 4);
        if (f <= 0) return 1.0f;
        if (p < f) return (float) p / (float) f;         // the take's first moment
        if (! sealing_ && p > len - f) return (float) (len - p) / (float) f;
        return 1.0f;
    }

    /// The loop length is fixed and playback starts now. `recordedEnd` is how
    /// much of the take is already in the buffer — more than `len` when a late
    /// stomp was rounded down to the bar, which is tail we already have.
    void beginSeal (int len, int recordedEnd) noexcept
    {
        sealLayer_ = layer_;
        sealLen_ = juce::jmin (fadeSamples_, len / 4, capacity_ - len);
        sealDone_ = 0;
        tailWrite_ = juce::jmax (len, juce::jmin (recordedEnd, capacity_));
        sealed_ = false;
        sealing_ = sealLen_ > 0;
    }

    void serviceSeal (int len, int playPos, float inL, float inR) noexcept
    {
        auto& a = layers_[(size_t) sealLayer_].audio;
        // Time runs on whatever the playback rate: the input now IS the take's
        // continuation, so it goes on the end of the take.
        if (tailWrite_ < len + sealLen_)
        {
            a[(size_t) tailWrite_ * 2] = inL;
            a[(size_t) tailWrite_ * 2 + 1] = inR;
            ++tailWrite_;
        }
        // Blend head position k once its tail sample exists and the playhead
        // has already played it this pass (the first pass hears the take as
        // it was; every later pass hears the sealed seam).
        while (sealDone_ < sealLen_ && len + sealDone_ < tailWrite_ && sealDone_ <= playPos)
        {
            const int k = sealDone_;
            const float g = (float) k / (float) sealLen_;    // 0 = all tail, 1 = all head
            const size_t h = (size_t) k * 2, t = (size_t) (len + k) * 2;
            a[h]     = a[t] * (1.0f - g) + a[h] * g;
            a[h + 1] = a[t + 1] * (1.0f - g) + a[h + 1] * g;
            ++sealDone_;
        }
        if (sealDone_ >= sealLen_) { sealing_ = false; sealed_ = true; }
    }

    void closeRecording (bool mayWaitForBar = true) noexcept
    {
        const int recorded = juce::jmax (1, recPos_);
        int target = recorded;
        if (mayWaitForBar && quantize_ != 0 && gridActive_.load (std::memory_order_relaxed))
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
        const int len = juce::jmax (1, juce::jmin (target, juce::jmin (recorded, capacity_)));
        length_.store (len, std::memory_order_relaxed);
        layers_[(size_t) layer_].hasAudio.store (true, std::memory_order_relaxed);
        // A stomp just after the bar line was rounded DOWN: the loop's "1" is
        // the bar line that has already passed, so playback resumes where the
        // music is now (the overshoot), not at the top. Otherwise the loop
        // would start late by the lateness of the stomp and sit off the click.
        const int overshoot = (recorded > len) ? (recorded - len) % len : 0;
        readPos_ = (double) overshoot;
        pos_.store (overshoot, std::memory_order_relaxed);
        beginSeal (len, recorded);
        if (stopAfterClose_) { sealing_ = false; state_.store (State::stopped, std::memory_order_relaxed); }
        else state_.store (State::playing, std::memory_order_relaxed);
    }

    /// AUDIO THREAD. The first note of an armed take was played at `p` samples
    /// after the count-in's downbeat. The take starts on the grid unit (bar or
    /// beat) at or before it; a note just before a unit line (within a
    /// sixteenth) is a pickup into the next one and the take starts there.
    /// Free-running: the take starts on the note itself.
    /// REC with start-on-note off. On the click (and Bar/Beat quantise) the
    /// take starts on the next unit line, or on the one just passed when the
    /// press was a sixteenth late; free-running, at once.
    void beginOnGrid() noexcept
    {
        const double ph = clickPhaseAt (curI_);
        const double unit = gridOn() ? (quantize_ == 1 ? barSamples() : beatSamples()) : 0.0;
        recPos_ = 0;
        if (ph < 0.0 || unit <= 1.0) { state_.store (State::recording, std::memory_order_relaxed); return; }
        const double into = std::fmod (ph, unit);
        if (into < beatSamples() / 4.0)
        {
            const int pad = (int) std::round (into);
            auto& a = layers_[(size_t) layer_].audio;
            std::fill (a.begin(), a.begin() + (std::ptrdiff_t) pad * 2, 0.0f);
            recPos_ = pad;
            state_.store (State::recording, std::memory_order_relaxed);
        }
        else
        {
            startAt_ = (int) std::round (unit - into);
            state_.store (State::armed, std::memory_order_relaxed);
        }
    }

    /// PLAY after STOP. On the click the loop waits for the click's next "1"
    /// (or joins at once when the "1" has only just passed), so the loop and
    /// the click stay together however long the pause. Free-running, at once.
    void resumeOnGrid() noexcept
    {
        const double ph = clickPhaseAt (curI_);
        if (ph < 0.0 || ! gridOn())
        {
            readPos_ = 0.0;
            state_.store (State::playing, std::memory_order_relaxed);
            return;
        }
        if (ph < beatSamples() / 4.0) { readPos_ = ph; state_.store (State::playing, std::memory_order_relaxed); }
        else state_.store (State::cued, std::memory_order_relaxed);
    }

    void startOnFirstNote (int p, double clickPhase = -1.0) noexcept
    {
        double unit = 0.0;
        if (gridOn())
            unit = quantize_ == 1 ? barSamples() : beatSamples();
        int start = p;
        if (unit > 1.0)
        {
            // Where the unit lines are: on the click when it runs, else counted
            // from the press.
            const double into = clickPhase >= 0.0 ? std::fmod (clickPhase, unit) : std::fmod ((double) p, unit);
            start = p - (int) std::round (into);
            const double sixteenth = beatSamples() / 4.0;
            if (into > unit - sixteenth) start = p + (int) std::round (unit - into);
        }
        if (start < 0)
        {
            // The bar began before REC was pressed: the take is that bar, with
            // silence where nothing was recorded yet, so it stays on the click.
            auto& a = layers_[(size_t) layer_].audio;
            const int pad = -start, n = juce::jmin (recPos_, capacity_ - pad);
            if (n > 0) std::memmove (a.data() + (size_t) pad * 2, a.data(), (size_t) n * 2 * sizeof (float));
            std::fill (a.begin(), a.begin() + (std::ptrdiff_t) pad * 2, 0.0f);
            recPos_ = n + pad;
            startAt_ = -1;
            state_.store (State::recording, std::memory_order_relaxed);
        }
        else if (start <= p)
        {
            // Everything since that line is the take's beginning: slide it down.
            // At most one bar, done once; a few hundred microseconds.
            auto& a = layers_[(size_t) layer_].audio;
            const int n = recPos_ - start;
            if (start > 0 && n > 0) std::memmove (a.data(), a.data() + (size_t) start * 2, (size_t) n * 2 * sizeof (float));
            recPos_ = juce::jmax (0, n);
            startAt_ = -1;
            state_.store (State::recording, std::memory_order_relaxed);
        }
        else startAt_ = start;
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

    /// Moves this block's commands out of the ring, ordered by offset (stable,
    /// so two at the same sample keep the order they were pressed in).
    void drainCommands() noexcept
    {
        nPending_ = 0;
        const auto w = writeIdx_.load (std::memory_order_acquire);
        while (readIdx_ != w)
        {
            const int c = cmdRing_[(size_t) (readIdx_ % kRing)].exchange (0, std::memory_order_acq_rel);
            ++readIdx_;
            if (c == 0 || nPending_ >= kRing) continue;
            Pending p { (Cmd) (c & kCmdMask), c >> kCmdBits };
            int k = nPending_++;
            while (k > 0 && pending_[(size_t) k - 1].offset > p.offset) { pending_[(size_t) k] = pending_[(size_t) k - 1]; --k; }
            pending_[(size_t) k] = p;
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
                        startAt_ = -1;
                        stopAfterClose_ = false;
                        sealing_ = sealed_ = false;
                        layers_[(size_t) layer_].hasAudio.store (false, std::memory_order_relaxed);
                        if (autoStart_) state_.store (State::armed, std::memory_order_relaxed);
                        else beginOnGrid();
                        break;
                    case State::armed:
                        // A second stomp while waiting: the take starts now, on this bar.
                        if (startAt_ < 0) startOnFirstNote (juce::jmax (0, recPos_ - 1), clickPhaseAt (curI_));
                        if (startAt_ >= 0 && startAt_ > recPos_) startAt_ = recPos_;
                        break;
                    case State::recording:
                        closeRecording();
                        break;
                    case State::stopped:
                        if (length_.load() > 0) resumeOnGrid();
                        else { recPos_ = 0; startAt_ = -1; stopAfterClose_ = false; sealing_ = sealed_ = false; state_.store (autoStart_ ? State::armed : State::recording, std::memory_order_relaxed); }
                        break;
                    case State::playing:
                        undoWritten_ = 0;
                        undoLayer_ = layer_;
                        lastDubPos_ = -1;
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
                // Stop during a take ends it the way REC would (on the nearest
                // bar when the grid is on) and then stops, the way a floor
                // looper does. Nothing is lost: the bar is finished first.
                if (st == State::armed) { state_.store (State::idle, std::memory_order_relaxed); break; }
                if (st == State::cued) { state_.store (State::stopped, std::memory_order_relaxed); break; }
                if (st == State::recording) { stopAfterClose_ = true; closeRecording (true); break; }
                if (st == State::waitingForBar) { stopAfterClose_ = true; break; }
                sealing_ = false;                       // no tail is coming
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
        sealing_ = sealed_ = false; sealLen_ = sealDone_ = tailWrite_ = 0; lastDubPos_ = -1;
        undoRemaining_ = 0; undoCursor_ = 0; undoActive_ = false;
        state_.store (State::idle, std::memory_order_relaxed);
    }

    struct Layer
    {
        std::vector<float> audio;               // interleaved stereo
        std::atomic<bool> hasAudio { false };
    };

    static constexpr int kRing = 32;
    static constexpr int kCmdBits = 4, kCmdMask = (1 << kCmdBits) - 1;
    static constexpr int kMaxOffset = (1 << 26) - 1;
    struct Pending { Cmd cmd = Cmd::record; int offset = 0; };
    std::array<Pending, kRing> pending_ {};
    int nPending_ = 0;

    double sr_ = 48000.0, bpm_ = 120.0;
    int beatsPerBar_ = 4, capacity_ = 0;

    std::array<Layer, kMaxLoopLayers> layers_;
    std::vector<float> undo_;

    std::atomic<State> state_ { State::idle };
    std::atomic<int> length_ { 0 }, pos_ { 0 };
    std::atomic<bool> undoValid_ { false };

    int recPos_ = 0, pendingLength_ = 0, layer_ = 0, undoLayer_ = 0, undoWritten_ = 0, undoStart_ = 0;
    int startAt_ = -1;                     // armed: the sample the take starts on, once known
    int curI_ = 0;                         // the sample in the block a command lands on
    double extPhase_ = 0.0, extBar_ = 0.0, extPulse_ = 0.0;   // the click's grid, this block
    std::atomic<int> fixedBars_ { 0 };
    bool autoStart_ = true, stopAfterClose_ = false;
    static constexpr float kStartLevel = 0.01f;   // -40 dBFS: a note, not hum
    int undoRemaining_ = 0, undoCursor_ = 0;
    bool undoActive_ = false;
    double readPos_ = 0.0;

    float level_ = 1.0f, decay_ = 0.0f;
    int quantize_ = 1, fadeSamples_ = 288;
    bool halfSpeed_ = false, reverse_ = false, playThrough_ = true;

    std::array<std::atomic<int>, kRing> cmdRing_ {};
    std::atomic<uint32_t> writeIdx_ { 0 };
    uint32_t readIdx_ = 0;
    std::atomic<bool> gridActive_ { false };

    // seam crossfade (see THE SEAM)
    int sealLayer_ = 0, sealLen_ = 0, sealDone_ = 0, tailWrite_ = 0, lastDubPos_ = -1;
    bool sealing_ = false, sealed_ = false;
};

} // namespace lowend
