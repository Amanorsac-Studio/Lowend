// CoverPlayer.h — the Cover tab: a song played by the engine, the player
// recorded over it, takes played back, and the result exported.
//
// WHY IN THE ENGINE. The Learn tab plays songs in the web view, which reaches
// the computer's default output, not the audio interface, and cannot be lined
// up with what the interface records. Here the song is mixed into the same
// buffer as the rig, so it plays on the device the player hears, and every
// recorded sample has a known place on the song's timeline.
//
// THE TIMELINE. Everything is indexed in samples from the start of the song at
// the engine's sample rate. A take is a buffer as long as the song: what was
// played at song position p is written at index p. Its files are written the
// same way (from the start of the song), so a take dropped into a DAW at bar 1
// lines up with the song without any offset.
//
// LATENCY. What the player hears at song position p left the interface a
// moment after the engine wrote it (output latency); what they played reached
// the engine a moment after they played it (input latency), and the rig adds
// its own (oversampling). A sample captured while the playhead reads p was
// therefore played over song position p - L, where L is the sum, and is written
// there. In a plug-in the host reports and compensates its own; only the rig's
// part applies.
//
// PUNCH. With a range set, only that range is written. The new take starts as
// a copy of the take it punches into, so the result is that take with the range
// replaced. During the pass the old take plays outside the range and is muted
// inside it, so the player hears themselves only once.
//
// THREADING. The song and the takes are built on the message thread or a
// background job and published to the audio thread as raw pointers under a
// spin lock; the message thread owns them (shared_ptr) and frees them only
// after unpublishing. The audio thread only ever TRIES the lock: a block that
// misses it renders no song and captures nothing, which in practice only
// happens during a load or a take swap, never mid-take.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>
#include "RigModel.h"

namespace lowend {

class CoverPlayer
{
public:
    static constexpr int kMaxStems = 4;
    static constexpr double kMaxMinutes = 20.0;   // longest song (and take) accepted

    enum class State { empty, loading, ready, error };

    struct Song
    {
        juce::String id;
        std::vector<juce::AudioBuffer<float>> stems;  // stereo, at the engine rate
        juce::StringArray names;                      // "Drums", "Bass", ... or "Mix"
        int length = 0;                               // samples
        double sampleRate = 0;
        juce::Array<juce::var> peaks;                 // the whole song, for the page's waveform
    };

    struct Take
    {
        int number = 0;
        juce::AudioBuffer<float> wet { 2, 0 };        // the rig's output
        juce::AudioBuffer<float> dry { 1, 0 };        // the instrument, after the input trim
        int from = -1, to = -1;                       // written range; to is exclusive
        bool punched = false;
        bool saved = false;
    };

    CoverPlayer() = default;
    ~CoverPlayer() { *alive_ = false; pool_.removeAllJobs (true, 10000); }

    static juce::File coversDir() { return appDataDir().getChildFile ("Covers"); }
    static juce::File takesDir (const juce::String& songId) { return coversDir().getChildFile (songId); }
    static juce::File exportDir() { return coversDir().getChildFile ("Exports"); }
    static juce::File learnDir()  { return appDataDir().getChildFile ("Learn"); }
    static bool validId (const juce::String& id)
    {
        return id.isNotEmpty() && id.length() <= 64
            && id.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
    }
    /// The song's audio on disk: its four stems if it was separated, else the
    /// mix the page handed over. Empty when the song has neither.
    static juce::Array<juce::File> songFiles (const juce::String& id)
    {
        juce::Array<juce::File> out;
        if (! validId (id)) return out;
        const auto dir = learnDir().getChildFile (id);
        for (auto* n : { "drums", "bass", "other", "vocals" })
            if (dir.getChildFile (juce::String (n) + ".wav").existsAsFile())
                out.add (dir.getChildFile (juce::String (n) + ".wav"));
        if (out.size() != kMaxStems)
        {
            out.clear();
            if (dir.getChildFile ("mix.wav").existsAsFile()) out.add (dir.getChildFile ("mix.wav"));
        }
        return out;
    }

    //================================================================ AUDIO THREAD
    void prepare (double sampleRate, int maxBlock)
    {
        if (sampleRate != sr_ && sr_ > 0 && songId_.isNotEmpty()) reloadDue_.store (true);
        sr_ = sampleRate;
        juce::ignoreUnused (maxBlock);
        clickPhase_ = 1.0e9;
    }

    /// The record tap. `wet` is the rig's stereo output before the looper and
    /// the master; `dry` is the instrument after the input trim, aligned with it.
    void capture (const float* wetL, const float* wetR, const float* dryL, const float* dryR, int n) noexcept
    {
        if (! recording_ || ! playing_) return;
        const juce::SpinLock::ScopedTryLockType tl (lock_);
        if (! tl.isLocked() || rec_ == nullptr) return;

        auto& t = *rec_;
        const int len = t.wet.getNumSamples();
        const juce::int64 base = pos_ - latency_.load (std::memory_order_relaxed);
        const juce::int64 a = punchA_.load (std::memory_order_relaxed), b = punchB_.load (std::memory_order_relaxed);
        const bool punch = a >= 0 && b > a;
        auto* wl = t.wet.getWritePointer (0);
        auto* wr = t.wet.getWritePointer (1);
        auto* d = t.dry.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            const juce::int64 at = base + i;
            if (at < 0 || at >= len) continue;
            if (at < startPos_) continue;              // the pre-roll is heard, not recorded
            if (punch && (at < a || at >= b)) continue;
            const int k = (int) at;
            wl[k] = wetL[i];
            wr[k] = wetR[i];
            d[k] = 0.5f * (dryL[i] + dryR[i]);
            if (t.from < 0 || k < t.from) t.from = k;
            if (k + 1 > t.to) t.to = k + 1;
        }
    }

    /// Adds the song, the selected take and the click to the main mix, and
    /// moves the playhead.
    void render (float* outL, float* outR, int n) noexcept
    {
        if (const int cmd = command_.exchange (0, std::memory_order_acq_rel); cmd != 0) applyCommand (cmd);
        if (! playing_) { playingUi_.store (false, std::memory_order_relaxed); return; }

        const juce::SpinLock::ScopedTryLockType tl (lock_);
        const Song* song = tl.isLocked() ? song_ : nullptr;
        const Take* take = tl.isLocked() ? play_ : nullptr;
        const int len = song != nullptr ? song->length : 0;

        const float songGain = gainOf (songDb_);
        const float takeGain = takeOn_.load (std::memory_order_relaxed) ? gainOf (takeDb_) : 0.0f;
        const juce::int64 a = punchA_.load (std::memory_order_relaxed), b = punchB_.load (std::memory_order_relaxed);
        const bool punching = recording_ && a >= 0 && b > a;

        float g[kMaxStems] {};
        if (song != nullptr)
        {
            bool solo = false;
            for (size_t s = 0; s < song->stems.size(); ++s) solo |= stemSolo_[s].load (std::memory_order_relaxed);
            for (size_t s = 0; s < song->stems.size(); ++s)
            {
                const bool on = solo ? stemSolo_[s].load (std::memory_order_relaxed) : ! stemMute_[s].load (std::memory_order_relaxed);
                g[s] = on ? songGain * gainOf (stemDb_[s]) : 0.0f;
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const juce::int64 p = pos_ + i;
            float l = 0.0f, r = 0.0f;
            if (p >= 0 && p < len)
            {
                for (size_t s = 0; s < song->stems.size(); ++s)
                {
                    if (g[s] == 0.0f) continue;
                    l += song->stems[s].getSample (0, (int) p) * g[s];
                    r += song->stems[s].getSample (1, (int) p) * g[s];
                }
            }
            if (take != nullptr && takeGain > 0.0f && p >= take->from && p < take->to
                && ! (punching && p >= a && p < b))
            {
                l += take->wet.getSample (0, (int) p) * takeGain;
                r += take->wet.getSample (1, (int) p) * takeGain;
            }
            const float c = clickSample (p, tl.isLocked() ? &beats_ : nullptr);
            outL[i] += l + c;
            outR[i] += r + c;
        }

        pos_ += n;
        posUi_.store (pos_, std::memory_order_relaxed);
        countingInUi_.store (pos_ < startPos_, std::memory_order_relaxed);

        // The end: two seconds after the song (a ringing chord is kept), or the
        // end of the punch range plus one second.
        const juce::int64 end = punching ? b + (juce::int64) sr_ : (juce::int64) len + (juce::int64) (2.0 * sr_);
        if (len > 0 && pos_ >= end) stopNow();
    }

    //=============================================================== MESSAGE THREAD
    /// Starts loading `id` in the background. Takes recorded for it are loaded too.
    bool load (const juce::String& id, const juce::String& name)
    {
        if (! validId (id)) return false;
        const auto files = songFiles (id);
        if (files.isEmpty()) { setState (State::error, "This song has no audio for Cover. Open it in Learn first."); return false; }

        stop();
        unpublish();
        recording_owned_.reset();
        selected_.reset();
        takes_.clear();
        song_owned_.reset();
        songId_ = id;
        songName_ = name.isNotEmpty() ? name : id;
        setState (State::loading, {});
        const double sr = sr_ > 0 ? sr_ : 48000.0;
        const int gen = ++generation_;

        pool_.addJob ([this, id, files, sr, gen]
        {
            auto song = std::make_shared<Song>();
            song->id = id;
            song->sampleRate = sr;
            juce::String err;
            for (auto& f : files)
            {
                juce::AudioBuffer<float> buf;
                if (! readResampled (f, sr, buf, err)) break;
                song->length = juce::jmax (song->length, buf.getNumSamples());
                song->stems.push_back (std::move (buf));
                const auto base = f.getFileNameWithoutExtension();
                song->names.add (base == "other" ? juce::String ("Guitar & keys") : base == "mix" ? juce::String ("Song") : base.substring (0, 1).toUpperCase() + base.substring (1));
            }
            for (auto& s : song->stems)
                if (s.getNumSamples() < song->length) s.setSize (2, song->length, true, true, false);
            // The waveform is drawn from the audio itself: an older analysis may
            // not have stored one.
            {
                constexpr int kBins = 800;
                const int span = juce::jmax (1, song->length / kBins);
                for (int k = 0; k < kBins; ++k)
                {
                    float pk = 0.0f;
                    for (int i = k * span; i < juce::jmin (song->length, (k + 1) * span); i += 4)
                    {
                        float v = 0.0f;
                        for (auto& st : song->stems) v += st.getSample (0, i) + st.getSample (1, i);
                        pk = juce::jmax (pk, std::abs (v) * 0.5f);
                    }
                    song->peaks.add (std::round (juce::jmin (1.0f, pk) * 1000.0f) / 1000.0f);
                }
            }

            std::vector<std::shared_ptr<Take>> takes;
            if (err.isEmpty()) takes = readTakes (id, sr, song->length);

            juce::MessageManager::callAsync ([this, alive = alive_, song, takes, err, gen]
            {
                if (! *alive) return;
                if (gen != generation_.load()) return;          // another song was chosen meanwhile
                if (err.isNotEmpty() || song->stems.empty()) { setState (State::error, err.isNotEmpty() ? err : "The song could not be read."); return; }
                song_owned_ = song;
                takes_ = takes;
                publish();
                setState (State::ready, {});
            });
        });
        return true;
    }

    void unload()
    {
        stop();
        ++generation_;
        unpublish();
        recording_owned_.reset();
        selected_.reset();
        takes_.clear();
        song_owned_.reset();
        songId_.clear();
        setState (State::empty, {});
    }

    /// The editor's timer. Reloads after a sample-rate change, collects a take
    /// that has just finished, and reports whether anything changed.
    bool poll()
    {
        bool changed = false;
        if (reloadDue_.exchange (false) && songId_.isNotEmpty()) { load (songId_, songName_); changed = true; }
        if (finished_.exchange (false)) { collectTake(); changed = true; }
        if (stateChanged_.exchange (false)) changed = true;
        return changed;
    }

    /// Play from `seconds`. With `countInBeats` the click counts in at `bpm`
    /// first. With `record` a new take is armed for the pass.
    bool play (double seconds, int countInBeats, double bpm, bool record)
    {
        if (song_owned_ == nullptr || playingUi_.load()) return false;
        const auto start = (juce::int64) juce::jmax (0.0, seconds * sr_);
        if (record && ! armTake()) return false;
        const double beat = 60.0 / juce::jlimit (30.0, 300.0, bpm) * sr_;
        pendingStart_.store (start);
        pendingFrom_.store (start - (juce::int64) std::llround (juce::jlimit (0, 8, countInBeats) * beat));
        pendingBeat_.store (beat);
        command_.store (record ? 2 : 1, std::memory_order_release);
        return true;
    }

    void stop() { if (playingUi_.load() || command_.load() != 0) command_.store (3, std::memory_order_release); }

    /// The punch range in seconds; anything else clears it.
    void setPunch (double aSeconds, double bSeconds)
    {
        if (aSeconds < 0 || bSeconds <= aSeconds) { punchA_.store (-1); punchB_.store (-1); return; }
        punchA_.store ((juce::int64) (aSeconds * sr_));
        punchB_.store ((juce::int64) (bSeconds * sr_));
    }
    void clearPunch() { punchA_.store (-1); punchB_.store (-1); }

    void setLevels (float songDb, float takeDb, bool hearTake)
    {
        songDb_.store (songDb); takeDb_.store (takeDb); takeOn_.store (hearTake);
    }
    void setStem (int i, float db, bool mute, bool solo)
    {
        if (i < 0 || i >= kMaxStems) return;
        stemDb_[(size_t) i].store (db); stemMute_[(size_t) i].store (mute); stemSolo_[(size_t) i].store (solo);
    }
    /// The click during playback: on the song's own beat grid.
    void setClick (bool on, double bpm, double firstBeatSeconds, int beatsPerBar, float levelDb)
    {
        clickOn_.store (on);
        gridBeat_.store (60.0 / juce::jlimit (30.0, 300.0, bpm) * sr_);
        gridT0_.store (firstBeatSeconds * sr_);
        gridBar_.store (juce::jlimit (1, 12, beatsPerBar));
        clickDb_.store (levelDb);
    }
    /// The click's beats, in seconds, with the "1" of each bar marked. When
    /// given, they replace the single tempo of `setClick`: a song with a free
    /// intro or a tempo change is clicked where its beats really are.
    void setClickBeats (const std::vector<double>& seconds, std::vector<bool> accents)
    {
        std::vector<juce::int64> at;
        at.reserve (seconds.size());
        for (auto t : seconds) at.push_back ((juce::int64) std::llround (t * sr_));
        {
            const juce::SpinLock::ScopedLockType sl (lock_);
            beats_.swap (at);
            accents_.swap (accents);
            beatsReset_ = true;
        }
    }

    /// Device input + output latency plus the rig's own, in samples.
    void setLatency (int samples) { latency_.store (juce::jmax (0, samples)); }
    int latency() const { return latency_.load(); }

    bool selectTake (int number)
    {
        std::shared_ptr<Take> t;
        for (auto& k : takes_) if (k->number == number) t = k;
        if (number != 0 && t == nullptr) return false;
        selected_ = t;
        publish();
        return true;
    }
    bool deleteTake (int number)
    {
        for (size_t i = 0; i < takes_.size(); ++i)
        {
            if (takes_[i]->number != number) continue;
            if (selected_ == takes_[i]) selected_.reset();
            publish();
            takeFile (songId_, number, false).deleteFile();
            takeFile (songId_, number, true).deleteFile();
            takes_.erase (takes_.begin() + (std::ptrdiff_t) i);
            return true;
        }
        return false;
    }

    /// Writes the mix, the rig's track and the DI track of take `number` (or
    /// the song alone with 0) to the export folder. Runs in the background;
    /// the result arrives through `exportResult`.
    bool exportMix (int number)
    {
        if (song_owned_ == nullptr || exporting_.load()) return false;
        std::shared_ptr<Take> take;
        for (auto& k : takes_) if (k->number == number) take = k;
        if (number != 0 && take == nullptr) return false;

        struct Mix { float song; float stem[kMaxStems]; bool on[kMaxStems]; float take; } m {};
        bool solo = false;
        for (int s = 0; s < kMaxStems; ++s) solo |= stemSolo_[(size_t) s].load();
        m.song = gainOf (songDb_);
        m.take = gainOf (takeDb_);
        for (int s = 0; s < kMaxStems; ++s)
        {
            m.stem[s] = gainOf (stemDb_[(size_t) s]);
            m.on[s] = solo ? stemSolo_[(size_t) s].load() : ! stemMute_[(size_t) s].load();
        }

        exporting_.store (true);
        exportResult_ = juce::var();
        auto song = song_owned_;
        const auto dir = exportDir().getChildFile (juce::File::createLegalFileName (songName_));
        const auto stem = juce::File::createLegalFileName (songName_) + (number > 0 ? " - Take " + juce::String (number) : juce::String());
        pool_.addJob ([this, song, take, m, dir, stem]
        {
            juce::String err;
            dir.createDirectory();
            const int len = juce::jmax (song->length, take != nullptr ? take->to : 0);
            juce::AudioBuffer<float> mix (2, len);
            mix.clear();
            for (size_t s = 0; s < song->stems.size(); ++s)
                if (m.on[s])
                    for (int ch = 0; ch < 2; ++ch)
                        mix.addFrom (ch, 0, song->stems[s], ch, 0, song->length, m.song * m.stem[s]);
            if (take != nullptr)
                for (int ch = 0; ch < 2; ++ch)
                    mix.addFrom (ch, 0, take->wet, ch, 0, take->to, m.take);

            // Never clipped: a mix that peaks over -0.3 dBFS is turned down as
            // a whole, and the change is reported.
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch) peak = juce::jmax (peak, mix.getMagnitude (ch, 0, len));
            const float ceiling = juce::Decibels::decibelsToGain (-0.3f);
            float trimDb = 0.0f;
            if (peak > ceiling) { mix.applyGain (ceiling / peak); trimDb = juce::Decibels::gainToDecibels (ceiling / peak); }

            juce::StringArray written;
            auto put = [&] (const juce::String& name, const juce::AudioBuffer<float>& b, int ch, int n)
            {
                if (err.isNotEmpty()) return;
                const auto f = dir.getChildFile (name + ".wav");
                if (! writeWav (f, b, ch, n, song->sampleRate)) err = "Could not write " + f.getFileName() + ".";
                else written.add (f.getFullPathName());
            };
            put (stem + (take != nullptr ? " - Mix" : " - Backing"), mix, 2, len);
            if (take != nullptr)
            {
                put (stem + " - Bass", take->wet, 2, take->to);
                put (stem + " - Bass DI", take->dry, 1, take->to);
            }

            juce::MessageManager::callAsync ([this, alive = alive_, err, written, trimDb, dir]
            {
                if (! *alive) return;
                auto o = new juce::DynamicObject();
                o->setProperty ("ok", err.isEmpty());
                o->setProperty ("error", err);
                o->setProperty ("folder", dir.getFullPathName());
                juce::Array<juce::var> files;
                for (auto& w : written) files.add (w);
                o->setProperty ("files", files);
                o->setProperty ("trimDb", trimDb);
                exportResult_ = juce::var (o);
                exporting_.store (false);
                stateChanged_.store (true);
            });
        });
        return true;
    }

    //--------------------------------------------------- the page hands over a mix
    /// A song that was never separated exists only in the web view. The page
    /// sends it once (stereo, 44.1 kHz, 16-bit, base64, in chunks) and it is
    /// kept as Learn/<id>/mix.wav.
    bool beginUpload (const juce::String& id, int frames)
    {
        if (! validId (id) || frames <= 0 || frames > (int) (kMaxMinutes * 60.0 * 44100.0)) return false;
        upId_ = id;
        upFrames_ = frames;
        upload_.setSize (2, frames);
        upload_.clear();
        return true;
    }
    bool appendChunk (const juce::String& id, int offset, const juce::String& b64)
    {
        if (id != upId_) return false;
        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64 (decoded, b64)) return false;
        const auto* s = static_cast<const juce::int16*> (decoded.getData());
        const int frames = (int) (decoded.getDataSize() / 4);
        if (offset < 0 || offset + frames > upFrames_) return false;
        auto* L = upload_.getWritePointer (0);
        auto* R = upload_.getWritePointer (1);
        for (int i = 0; i < frames; ++i)
        {
            L[offset + i] = (float) (juce::int16) juce::ByteOrder::swapIfBigEndian ((juce::uint16) s[2 * i]) / 32768.0f;
            R[offset + i] = (float) (juce::int16) juce::ByteOrder::swapIfBigEndian ((juce::uint16) s[2 * i + 1]) / 32768.0f;
        }
        return true;
    }
    bool finishUpload (const juce::String& id, const juce::String& name)
    {
        if (id != upId_ || upFrames_ <= 0) return false;
        const auto dir = learnDir().getChildFile (id);
        dir.createDirectory();
        const bool ok = writeWav (dir.getChildFile ("mix.wav"), upload_, 2, upFrames_, 44100.0, 16);
        upload_.setSize (0, 0);
        upId_.clear();
        upFrames_ = 0;
        return ok && load (id, name);
    }

    //--------------------------------------------------------------------- status
    juce::var describe() const
    {
        auto o = new juce::DynamicObject();
        static const char* names[] = { "empty", "loading", "ready", "error" };
        o->setProperty ("state", names[(int) state_]);
        o->setProperty ("error", error_);
        o->setProperty ("song", songId_);
        o->setProperty ("songName", songName_);
        o->setProperty ("length", song_owned_ != nullptr ? song_owned_->length / sr_ : 0.0);
        juce::Array<juce::var> stems;
        if (song_owned_ != nullptr) for (auto& n : song_owned_->names) stems.add (n);
        o->setProperty ("stems", stems);
        o->setProperty ("peaks", song_owned_ != nullptr ? juce::var (song_owned_->peaks) : juce::var());
        juce::Array<juce::var> takes;
        for (auto& t : takes_)
        {
            auto k = new juce::DynamicObject();
            k->setProperty ("number", t->number);
            k->setProperty ("from", t->from / sr_);
            k->setProperty ("to", t->to / sr_);
            k->setProperty ("punched", t->punched);
            k->setProperty ("saved", t->saved);
            k->setProperty ("peaks", peaks (t->wet, t->to, song_owned_ != nullptr ? song_owned_->length : t->to));
            takes.add (juce::var (k));
        }
        o->setProperty ("takes", takes);
        o->setProperty ("selected", selected_ != nullptr ? selected_->number : 0);
        o->setProperty ("latencyMs", latency_.load() * 1000.0 / sr_);
        o->setProperty ("exporting", exporting_.load());
        o->setProperty ("export", exportResult_);
        return juce::var (o);
    }
    /// The playhead (seconds; negative during a count-in), for the 30 Hz tick.
    double positionSeconds() const { return (double) posUi_.load (std::memory_order_relaxed) / sr_; }
    bool isPlaying() const { return playingUi_.load (std::memory_order_relaxed); }
    bool isRecording() const { return recordingUi_.load (std::memory_order_relaxed); }
    bool isCountingIn() const { return countingInUi_.load (std::memory_order_relaxed); }
    bool hasSong() const { return song_owned_ != nullptr; }
    juce::String songId() const { return songId_; }

private:
    //------------------------------------------------------------ audio thread
    void applyCommand (int cmd) noexcept
    {
        beatsReset_ = true;
        if (cmd == 3) { stopNow(); return; }
        pos_ = pendingFrom_.load();
        startPos_ = pendingStart_.load();
        countBeat_ = pendingBeat_.load();
        recording_ = cmd == 2;
        playing_ = true;
        clickPhase_ = 1.0e9;
        playingUi_.store (true, std::memory_order_relaxed);
        recordingUi_.store (recording_, std::memory_order_relaxed);
        posUi_.store (pos_, std::memory_order_relaxed);
    }
    void stopNow() noexcept
    {
        if (! playing_) return;
        const bool wasRecording = recording_;
        playing_ = recording_ = false;
        playingUi_.store (false, std::memory_order_relaxed);
        recordingUi_.store (false, std::memory_order_relaxed);
        countingInUi_.store (false, std::memory_order_relaxed);
        if (wasRecording) finished_.store (true, std::memory_order_release);
    }

    /// A short pitched blip: high on the downbeat, lower on the others. The
    /// count-in counts in on its own clock; after it, the song's grid is used.
    float clickSample (juce::int64 p, const std::vector<juce::int64>* beats) noexcept
    {
        bool tick = false, accent = false;
        if (beats != nullptr && beatsReset_) { nextBeat_ = 0; beatsReset_ = false; }
        if (p < startPos_ && countBeat_ > 0)
        {
            const double back = (double) (startPos_ - p);
            const double k = back / countBeat_;
            tick = std::abs (k - std::round (k)) * countBeat_ < 0.5 && std::round (k) >= 1.0;
            accent = tick && (juce::int64) std::round (k) % gridBar_.load (std::memory_order_relaxed) == 0;
        }
        else if (clickOn_.load (std::memory_order_relaxed) && beats != nullptr && ! beats->empty())
        {
            // The song's own beats, tempo changes and free-time intros included.
            while (nextBeat_ < beats->size() && (*beats)[nextBeat_] < p) ++nextBeat_;
            if (nextBeat_ < beats->size() && (*beats)[nextBeat_] == p)
            {
                tick = true;
                accent = nextBeat_ < accents_.size() && accents_[nextBeat_];
                ++nextBeat_;
            }
        }
        else if (clickOn_.load (std::memory_order_relaxed))
        {
            const double beat = gridBeat_.load (std::memory_order_relaxed);
            const double rel = (double) p - gridT0_.load (std::memory_order_relaxed);
            if (beat > 0 && rel >= 0)
            {
                const double k = rel / beat;
                const double frac = rel - std::floor (k) * beat;
                tick = frac < 1.0;
                accent = tick && (juce::int64) std::floor (k) % gridBar_.load (std::memory_order_relaxed) == 0;
            }
        }
        if (tick) { clickPhase_ = 0.0; clickHz_ = accent ? 1760.0 : 1320.0; }

        const double secs = clickPhase_ / sr_;
        if (secs > 0.04) return 0.0f;
        clickPhase_ += 1.0;
        const float level = gainOf (clickDb_);
        return level * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * clickHz_ * secs) * std::exp (-secs * 120.0));
    }

    static float gainOf (const std::atomic<float>& db) noexcept
    {
        const float d = db.load (std::memory_order_relaxed);
        return d <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (d);
    }

    //---------------------------------------------------------- message thread
    void setState (State s, const juce::String& err) { state_ = s; error_ = err; stateChanged_.store (true); }

    void publish()
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        song_ = song_owned_.get();
        play_ = selected_.get();
        rec_ = recording_owned_.get();
    }
    void unpublish()
    {
        const juce::SpinLock::ScopedLockType sl (lock_);
        song_ = nullptr; play_ = nullptr; rec_ = nullptr;
    }

    bool armTake()
    {
        const int len = song_owned_->length + (int) (2.0 * sr_);
        if (len <= 0 || len > (int) (kMaxMinutes * 60.0 * sr_)) return false;
        auto t = std::make_shared<Take>();
        int next = 1;
        for (auto& k : takes_) next = juce::jmax (next, k->number + 1);
        t->number = next;
        t->punched = punchA_.load() >= 0 && punchB_.load() > punchA_.load();
        t->wet.setSize (2, len);
        t->dry.setSize (1, len);
        t->wet.clear();
        t->dry.clear();
        // A punch starts from the take it replaces part of.
        if (t->punched && selected_ != nullptr)
        {
            const int n = juce::jmin (len, selected_->wet.getNumSamples());
            for (int ch = 0; ch < 2; ++ch) t->wet.copyFrom (ch, 0, selected_->wet, ch, 0, n);
            t->dry.copyFrom (0, 0, selected_->dry, 0, 0, n);
            t->from = selected_->from;
            t->to = selected_->to;
        }
        recording_owned_ = t;
        publish();
        return true;
    }

    void collectTake()
    {
        auto t = recording_owned_;
        recording_owned_.reset();
        publish();                            // the audio thread no longer sees it
        if (t == nullptr || t->from < 0 || t->to <= t->from) return;   // nothing was played into it
        takes_.push_back (t);
        selected_ = t;
        publish();
        const auto id = songId_;
        const double sr = sr_;
        pool_.addJob ([this, t, id, sr]
        {
            const auto dir = takesDir (id);
            dir.createDirectory();
            const bool ok = writeWav (takeFile (id, t->number, false), t->wet, 2, t->to, sr)
                         && writeWav (takeFile (id, t->number, true), t->dry, 1, t->to, sr);
            juce::MessageManager::callAsync ([this, alive = alive_, t, ok] { if (! *alive) return; t->saved = ok; stateChanged_.store (true); });
        });
    }

    static juce::File takeFile (const juce::String& id, int number, bool di)
    {
        return takesDir (id).getChildFile ("Take " + juce::String (number) + (di ? " DI" : "") + ".wav");
    }

    static std::vector<std::shared_ptr<Take>> readTakes (const juce::String& id, double sr, int songLength)
    {
        std::vector<std::shared_ptr<Take>> out;
        for (const auto& f : takesDir (id).findChildFiles (juce::File::findFiles, false, "Take *.wav"))
        {
            const auto base = f.getFileNameWithoutExtension();
            if (base.endsWith (" DI")) continue;
            const int number = base.fromFirstOccurrenceOf ("Take ", false, false).getIntValue();
            if (number <= 0) continue;
            juce::String err;
            auto t = std::make_shared<Take>();
            t->number = number;
            t->saved = true;
            juce::AudioBuffer<float> wet, dry;
            if (! readResampled (f, sr, wet, err)) continue;
            if (! readResampled (takeFile (id, number, true), sr, dry, err)) dry.setSize (1, wet.getNumSamples(), false, true);
            const int len = juce::jmax (songLength + (int) (2.0 * sr), wet.getNumSamples());
            t->wet.setSize (2, len); t->wet.clear();
            t->dry.setSize (1, len); t->dry.clear();
            for (int ch = 0; ch < 2; ++ch) t->wet.copyFrom (ch, 0, wet, juce::jmin (ch, wet.getNumChannels() - 1), 0, wet.getNumSamples());
            t->dry.copyFrom (0, 0, dry, 0, 0, juce::jmin (dry.getNumSamples(), len));
            // The written range: the first and last samples that are not silence.
            const auto* l = t->wet.getReadPointer (0);
            const auto* r = t->wet.getReadPointer (1);
            for (int i = 0; i < wet.getNumSamples(); ++i)
                if (l[i] != 0.0f || r[i] != 0.0f) { if (t->from < 0) t->from = i; t->to = i + 1; }
            if (t->from >= 0) out.push_back (t);
        }
        std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a->number < b->number; });
        return out;
    }

    /// Reads a WAV as stereo at `sr`.
    static bool readResampled (const juce::File& f, double sr, juce::AudioBuffer<float>& out, juce::String& err)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (f.createInputStream().release(), true));
        if (reader == nullptr) { err = "Could not read " + f.getFileName() + "."; return false; }
        const auto frames = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (kMaxMinutes * 60.0 * reader->sampleRate));
        juce::AudioBuffer<float> src (2, frames);
        reader->read (&src, 0, frames, 0, true, reader->numChannels > 1);
        if (reader->numChannels == 1) src.copyFrom (1, 0, src, 0, 0, frames);
        if (std::abs (reader->sampleRate - sr) < 0.5) { out = std::move (src); return true; }

        const double ratio = reader->sampleRate / sr;
        const int outFrames = (int) std::floor (frames / ratio);
        out.setSize (2, outFrames);
        for (int ch = 0; ch < 2; ++ch)
        {
            // Lagrange, not windowed sinc: a whole song has to resample in well
            // under a second, and for a backing track the difference is inaudible.
            juce::LagrangeInterpolator interp;
            interp.process (ratio, src.getReadPointer (ch), out.getWritePointer (ch), outFrames, frames, 0);
        }
        return true;
    }

    static bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, int channels, int frames, double sr, int bits = 24)
    {
        const auto tmp = f.getSiblingFile (f.getFileName() + ".part");
        tmp.deleteFile();
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (tmp);
            if (static_cast<juce::FileOutputStream*> (os.get())->failedToOpen()) return false;
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate (sr)
                                                  .withNumChannels (channels)
                                                  .withBitsPerSample (bits));
            if (w == nullptr) return false;
            if (! w->writeFromFloatArrays (b.getArrayOfReadPointers(), channels, juce::jmin (frames, b.getNumSamples()))) return false;
        }
        return tmp.moveFileTo (f);
    }

    /// 400 peak values over the song's length, for the take lane.
    static juce::var peaks (const juce::AudioBuffer<float>& b, int to, int length)
    {
        constexpr int kBins = 400;
        juce::Array<juce::var> out;
        const int span = juce::jmax (1, juce::jmax (length, to) / kBins);
        for (int k = 0; k < kBins; ++k)
        {
            const int s = k * span;
            const int n = juce::jmin (span, to - s);
            float p = 0.0f;
            if (n > 0) for (int ch = 0; ch < 2; ++ch) p = juce::jmax (p, b.getMagnitude (ch, s, n));
            out.add (std::round (p * 1000.0f) / 1000.0f);
        }
        return out;
    }

    //------------------------------------------------------------------ data
    std::shared_ptr<bool> alive_ = std::make_shared<bool> (true);   // message thread
    juce::ThreadPool pool_ { 1 };
    juce::SpinLock lock_;
    const Song* song_ = nullptr;          // audio thread's view, under lock_
    const Take* play_ = nullptr;
    Take* rec_ = nullptr;
    std::shared_ptr<Song> song_owned_;
    std::shared_ptr<Take> selected_, recording_owned_;
    std::vector<std::shared_ptr<Take>> takes_;
    juce::String songId_, songName_, error_;
    State state_ = State::empty;
    juce::var exportResult_;
    std::atomic<int> generation_ { 0 };
    std::atomic<bool> stateChanged_ { false }, reloadDue_ { false }, finished_ { false }, exporting_ { false };

    juce::AudioBuffer<float> upload_;
    juce::String upId_;
    int upFrames_ = 0;

    // commands and settings (message thread -> audio thread)
    std::atomic<int> command_ { 0 };      // 1 play, 2 record, 3 stop
    std::atomic<juce::int64> pendingStart_ { 0 }, pendingFrom_ { 0 };
    std::atomic<double> pendingBeat_ { 0 };
    std::atomic<juce::int64> punchA_ { -1 }, punchB_ { -1 };
    std::atomic<float> songDb_ { 0 }, takeDb_ { 0 }, clickDb_ { -6 };
    std::atomic<bool> takeOn_ { true }, clickOn_ { false };
    std::array<std::atomic<float>, kMaxStems> stemDb_ {};
    std::array<std::atomic<bool>, kMaxStems> stemMute_ {}, stemSolo_ {};
    std::atomic<double> gridBeat_ { 0 }, gridT0_ { 0 };
    std::atomic<int> gridBar_ { 4 }, latency_ { 0 };

    // the click's beats (swapped under lock_, walked by the audio thread)
    std::vector<juce::int64> beats_;
    std::vector<bool> accents_;
    size_t nextBeat_ = 0;
    std::atomic<bool> beatsReset_ { true };

    // audio thread
    double sr_ = 48000.0;
    juce::int64 pos_ = 0, startPos_ = 0;
    double countBeat_ = 0, clickPhase_ = 1.0e9, clickHz_ = 1320.0;
    bool playing_ = false, recording_ = false;

    // audio thread -> UI
    std::atomic<juce::int64> posUi_ { 0 };
    std::atomic<bool> playingUi_ { false }, recordingUi_ { false }, countingInUi_ { false };
};

} // namespace lowend
