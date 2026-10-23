// LowEndProcessor.h — Amanorsac Low End. Bass amp, cab, pedalboard, tracked
// synth and looper, as a VST3/AU effect and as a standalone stage
// app.
//
// THREADING. The Rig (RigModel.h) is owned by the message thread and is the
// truth. Every edit goes through `setPath`, which updates the model and pushes
// the new value into the DSP; the audio thread never reads the model, only the
// DSP objects' own atomics. Structural edits (a block added, removed or moved,
// a whole preset swapped) go through the chain's dry hand-over and the amp's
// A/B crossfade, both of which are described where they are implemented.
#pragma once

/// Set by the soak-test target, which builds the processor without the web UI.
#ifndef LOWEND_HEADLESS
 #define LOWEND_HEADLESS 0
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "../engine/AmpDsp.h"
#include "../engine/BassSynth.h"
#include "../engine/CoverPlayer.h"
#include "../engine/Limiter.h"
#include "../engine/Looper.h"
#include "../engine/Metronome.h"
#include "../engine/PedalDsp.h"
#include "../engine/PitchTrack.h"
#include "../engine/RigModel.h"
#include "../engine/Tuner.h"
#include "../learn/StemSeparator.h"
#include "TesterGate.h"

namespace lowend {

/// What the UI reads 30 times a second. All atomics, all written by the audio
/// thread, all read by the message thread; no locks anywhere in this path.
struct Meters
{
    std::atomic<float> inPeak { 0 }, outPeak { 0 }, ampSupply { 1 }, gainReduction { 0 }, synthPeak { 0 };
    std::atomic<bool> gateOpen { false }, clipping { false };
    /// The input's noise floor (dBFS peak) measured while nothing is played; -120 until known.
    std::atomic<float> noiseFloorDb { -120.0f };
};

class LowEndProcessor final : public juce::AudioProcessor,
                              public juce::ChangeBroadcaster,
                              private juce::AsyncUpdater
{
public:
    LowEndProcessor();
    ~LowEndProcessor() override;

    //-------------------------------------------------------- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return ! LOWEND_HEADLESS; }
    const juce::String getName() const override { return "Low End"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Rig"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //--------------------------------------------------------------- the rig
    /// MESSAGE THREAD. The single entry point for every edit.
    /// `structural` (block type changed, chain reordered) triggers the clickless
    /// rebuild; a plain value change goes straight through.
    bool setPath (const juce::String& path, float value, bool notify = true);
    float getPath (const juce::String& path, bool* ok = nullptr) const;
    const Rig& rig() const { return rig_; }
    Rig& editableRig() { return rig_; }

    void loadRig (const Rig& r, bool crossfade = true);
    void rebuildChains (bool crossfade);
    void pushAllParams();

    /// Chain editing (message thread).
    bool addBlock (bool post, BlockType t, int index = -1);
    bool removeBlock (bool post, int index);
    bool moveBlock (bool post, int from, int to);

    //----------------------------------------------------------- performance
    RigLibrary& library() { return library_; }
    /// The Learn tab's stem separation. Lives here rather than in the editor so
    /// a separation in progress survives the plugin window being closed.
    StemSeparator& stems() { return stems_; }
    /// The Cover tab's song player and recorder.
    CoverPlayer& cover() { return cover_; }
    /// Standalone: the device's input + output latency, so a take lands where
    /// it was played (see CoverPlayer.h). A plug-in leaves it at 0.
    void setDeviceLatency (int samples) { deviceLatency_.store (samples); cover_.setLatency (samples + rigLatency_.load()); }
    void loadPreset (const juce::String& presetId);
    void loadSong (const juce::String& songId, int sceneIndex = 0);
    /// Leave the song and go back to browsing the flat preset list.
    void clearSong();
    void nextScene (int delta);
    juce::String currentPresetId() const { return currentPresetId_; }
    juce::String currentSongId() const { return currentSongId_; }
    int currentScene() const { return scene_; }

    void tapTempo();
    double tempo() const { return tempo_.load(); }
    void setTempo (double bpm);
    bool usingHostTempo() const { return hostTempoValid_.load(); }

    void setTunerActive (bool on);

    //------------------------------------------------------------ unsaved edits
    /// An edit not yet saved. Kept when the player changes preset, written to
    /// disk so it survives a restart, and resolved when the app is closed.
    struct PendingEdit { Rig rig; juce::String presetId, songId; int scene = -1; };
    /// The edits not yet saved, the live rig included when it differs.
    std::vector<PendingEdit> pendingEdits() const;
    bool hasPendingEdits() const { return ! pendingEdits().empty(); }
    /// The live rig differs from what it was loaded from (preset or scene override).
    bool isDirty() const;
    /// Is the live rig a set scene's own version (an override)?
    bool usingOverride() const { return overrideActive_; }
    enum class Resolve { toPreset, toSet, discard };
    /// Save or drop one pending edit. `key` is from `editKey`.
    bool resolveEdit (const juce::String& key, Resolve how);
    static juce::String editKey (const juce::String& presetId, const juce::String& songId, int scene, bool isOverride)
    { return isOverride ? presetId + "@" + songId + "#" + juce::String (scene) : presetId; }
    juce::String liveEditKey() const { return editKey (currentPresetId_, loadedSongId_, loadedScene_, overrideActive_); }
    /// Bypass All: every pedal, the amp and the cab out of circuit together, so
    /// the raw instrument is heard. Crossfaded over 20 ms, latency-aligned.
    void setBypassAll (bool on) { bypassAll_.store (on); sendChangeMessage(); }
    bool bypassAll() const { return bypassAll_.load(); }
    /// Concert pitch for the tuner (A4, 415-466 Hz) and whether the output is
    /// muted while it is up. Saved with the app, not the preset.
    void setTunerReference (float a4Hz) { tuner_.setReferenceHz (a4Hz); sendChangeMessage(); }
    float tunerReference() const { return tuner_.referenceHz(); }
    void setTunerMutes (bool on) { tunerMutes_ = on; sendChangeMessage(); }
    /// Auto gate: the gate threshold is lifted to sit 12 dB above the measured
    /// noise floor (hum from the instrument, interface noise), so the preset's
    /// own threshold is a minimum, not the whole story. Saved with the app.
    void setGateAuto (bool on) { gateAuto_.store (on); sendChangeMessage(); }
    bool gateAuto() const { return gateAuto_.load(); }
    /// Mains hum filter on the input: 0 off, else 50 or 60 (Hz). Narrow cuts at
    /// the fundamental and seven harmonics. Saved with the app.
    void setHumHz (int hz) { humHz_.store (hz == 50 || hz == 60 ? hz : 0); sendChangeMessage(); }
    int humHz() const { return humHz_.load(); }
    bool tunerMutes() const { return tunerMutes_; }
    /// Load scene `index` of the open set (or preset `index` with no set open).
    void jumpToScene (int index);
    bool tunerActive() const { return tunerActive_.load(); }
    const Tuner& tuner() const { return tuner_; }

    /// The record/overdub stomp from the UI. Goes through the processor (not
    /// straight to the looper) because a count-in may have to run first.
    void requestLoopRecord() noexcept { uiLoopRecord_.fetch_add (1, std::memory_order_acq_rel); }
    /// Stop/Clear from the UI also abandon a count-in (applied on the audio thread).
    void requestCountInCancel() noexcept { uiCancelCountIn_.store (true, std::memory_order_release); }

    //------------------------------------------------------------ the click
    /// Global, saved with the app state rather than the preset: a scene change
    /// must never switch the click off. MESSAGE THREAD.
    const ClickSettings& clickSettings() const { return click_; }
    void setClickSettings (const ClickSettings&);
    /// True when the device gives the app a second output pair to put the
    /// click on (standalone with outputs 3+ enabled). Otherwise "separate"
    /// falls back to the main mix, and the UI says so.
    /// Standalone output map, in the engine's own channel numbering (the device's
    /// enabled channels, in order): where the main mix goes, whether it is folded
    /// to mono, and where the click's own pair starts (-1: no pair).
    void setOutputRouting (int mainIndex, bool mono, int clickIndex) noexcept
    {
        outMain_.store (mainIndex); outMono_.store (mono); outClick_.store (clickIndex);
    }
    bool clickSeparateAvailable() const { return clickSeparate_.load (std::memory_order_relaxed); }
    bool countingIn() const { return countingInUi_.load (std::memory_order_relaxed); }
    int clickBeat() const { return clickBeatUi_.load (std::memory_order_relaxed); }

    Looper& looper() { return looper_; }
    const Looper& looper() const { return looper_; }
    const Meters& meters() const { return meters_; }
    float trackedNote() const { return trackedNote_.load(); }
    bool trackedVoiced() const { return trackedVoiced_.load(); }

    //------------------------------------------------------------ test builds
    /// TEST BUILDS ONLY (LOWEND_TESTER_BUILD). Until a valid, unexpired test key
    /// has been entered the audio is silent and the editor asks for one. In a
    /// release build these are constants: the gate is always open and nothing
    /// here does any work. See TesterGate.h for what the key is, and is not.
    bool testerGated() const noexcept { return tester::kEnabled; }
    tester::Check testerStatus() const { return tester_.current(); }
    /// MESSAGE THREAD. Checks a typed-in key; only a good one is kept.
    tester::Check submitTesterKey (const juce::String& key);

    //----------------------------------------------------------------- learn
    /// The note the tracker hears on the input (MIDI number, -1 when none), its
    /// tuning offset in cents, and a counter that increments on every note-on.
    int liveNote() const { return liveNote_.load(); }
    float liveCents() const { return liveCents_.load(); }
    int liveNoteCount() const { return liveNoteCount_.load(); }
    /// Keys currently held on a connected MIDI keyboard.
    juce::Array<int> heldMidiNotes() const
    {
        juce::Array<int> out;
        for (int w = 0; w < 2; ++w)
        {
            const auto bits = midiHeld_[(size_t) w].load();
            for (int b = 0; b < 64; ++b)
                if (bits & (1ull << b)) out.add (w * 64 + b);
        }
        return out;
    }

    void panic();

    //---------------------------------------------------------- control map
    /// The audio thread walks these lists on every MIDI event, so they are only
    /// ever replaced wholesale under a spin lock — never mutated in place by the
    /// UI while a CC is arriving.
    std::vector<Binding> bindings() const;
    std::vector<SwitchBinding> switchBindings() const;
    void setBindings (std::vector<Binding>);
    void setSwitchBindings (std::vector<SwitchBinding>);
    /// Arms MIDI learn for `path` (empty to cancel). The next CC seen binds.
    void startLearn (const juce::String& path)
    {
        learnPath_ = path;
        learnCc_.store (-1);
        learnArmed_.store (path.isNotEmpty());
    }
    /// The CC the armed path saw, or -1. Polled by the editor, which completes
    /// the learn by adding the binding and disarming.
    int learnedCc() const { return learnCc_.load(); }
    void cancelLearn() { learnPath_.clear(); learnArmed_.store (false); learnCc_.store (-1); }
    juce::String learnTarget() const { return learnPath_; }

    /// Cabs: rescan the IR folder and (re)load the one the rig names.
    void refreshIrs();

    static juce::File appDataDirectory() { return appDataDir(); }
    /// Standalone only: the audio device, described and driven as plain data so
    /// the app's own UI owns the panel instead of a native dialog that looks
    /// like it came from another program. Every function is null in a plugin —
    /// there the HOST owns the device, and the UI says so rather than offering
    /// settings that would do nothing.
    struct AudioHooks
    {
        std::function<juce::var()> describe;              ///< drivers, devices, rates, buffer sizes, what is current
        std::function<bool (const juce::var&)> apply;     ///< {driver, output, input, rate, buffer}
        std::function<bool()> openDriverPanel;            ///< ASIO's own control panel
        std::function<juce::String()> chooseLowest;       ///< best driver + smallest safe buffer; returns a description
    };
    static AudioHooks& audioHooks();

private:
    void applyMidi (const juce::MidiBuffer&);
    void updateTransport();
    void writeSession();
    /// Which amp+cab instance a parameter edit should land on: during a scene
    /// crossfade that is the INCOMING one, otherwise the live one.
    int targetAmp() const { return xfadeRemaining_ > 0 ? 1 - activeAmp_ : activeAmp_; }
    /// Message-thread half of the footswitch/CC path (see handleAsyncUpdate).
    void handleAsyncUpdate() override;

    //--------------------------------------------- in/out params, audio-thread
    // The audio thread must never read the Rig's std::maps, so the two sections
    // it needs every block live in their own atomic stores, refreshed by
    // setPath / pushAllParams on the message thread.
    ParamStore inParams_, outParams_;
    enum InIdx { kTrim = 0, kGateOn, kGateThreshold, kGateRelease, kDiOn, kDiLevel, kDiLowPass };
    enum OutIdx { kMasterDb = 0, kLimiterOn };

    //------------------------------------------------------------ the model
    Rig rig_ { defaultRig() };
    RigLibrary library_;
    StemSeparator stems_;
    CoverPlayer cover_;
    std::atomic<int> deviceLatency_ { 0 }, rigLatency_ { 0 };
    juce::AudioBuffer<float> coverBuf_;
    mutable juce::SpinLock bindingLock_;
    std::vector<Binding> bindings_;
    std::vector<SwitchBinding> switches_;
    juce::String learnPath_, currentPresetId_, currentSongId_;
    int scene_ = 0;

    //-------------------------------------------------------------- the DSP
    BlockChain preChain_, postChain_;
    /// TWO amp+cab instances. A preset change sets the idle one up, then
    /// crossfades over 25 ms while both render, so a scene change on stage never
    /// steps the tone. See processBlock.
    std::array<AmpHead, 2> amp_;
    std::array<CabSim, 2> cab_;
    int activeAmp_ = 0;
    int xfadeRemaining_ = 0, xfadeLength_ = 0;

    NoiseGate inputGate_;
    FixedDelay diDelay_;
    Biquad diLp_[2];
    jm::MasterLimiter limiter_;

    PitchTracker tracker_;
    BassSynth synth_;
    Looper looper_;
    Metronome metronome_;
    Tuner tuner_;

    // the click (message-thread copy; the audio thread reads the atomics)
    ClickSettings click_;
    std::atomic<int> clickOutput_ { 0 }, countInBars_ { 0 }, uiLoopRecord_ { 0 };
    std::atomic<bool> clickSeparate_ { false }, countingInUi_ { false }, uiCancelCountIn_ { false };
    std::atomic<int> clickBeatUi_ { 0 };
    double hostPpq_ = -1.0;
    bool clickWasOn_ = false;             // audio thread: the click's state last block
    std::atomic<int> outMain_ { 0 }, outClick_ { 2 };
    std::atomic<bool> outMono_ { false };
    juce::AudioBuffer<float> mainBuf_;              // audio thread: host beat position, or -1
    juce::AudioBuffer<float> clickBuf_;
    /// AUDIO THREAD: a record stomp, from a footswitch (with its sample) or the UI.
    void handleLoopRecord (int offset) noexcept;

    juce::AudioBuffer<float> diBuf_, synthBuf_, ampAltBuf_, rawBuf_;
    FixedDelay rawDelay_;
    juce::SmoothedValue<float> bypassMix_;
    std::atomic<bool> bypassAll_ { false };

    // noise: the floor tracker, the auto gate and the hum filter (audio thread)
    std::atomic<bool> gateAuto_ { true };
    std::atomic<int> humHz_ { 0 };
    static constexpr int kHumHarmonics = 8;
    std::array<std::array<Biquad, kHumHarmonics>, 2> hum_;
    int humSeen_ = -1;
    // 100 ms windows of input peak; the floor is the quietest of the last 3 s
    // among windows that are clearly not playing (below -40 dBFS).
    float winPeak_ = 0.0f;
    int winCount_ = 0, winLen_ = 4800, floorIdx_ = 0;
    std::array<float, 30> floorRing_ {};
    float floorDb_ = -120.0f;

    // edits kept across preset changes (message thread)
    std::map<juce::String, PendingEdit> edits_;
    bool overrideActive_ = false;
    // The set and scene the live rig was loaded FROM. The current scene number
    // moves before a new scene loads; the live edit belongs to the old one.
    juce::String loadedSongId_;
    int loadedScene_ = -1;
    /// What the live rig was loaded from: the preset, or the scene's override.
    const Rig* baseRig() const;
    void stashLiveEdit();
    void writeEdits() const;
    void readEdits();
    bool bypassSeen_ = false;              ///< message thread: last value the UI was told

    //------------------------------------------------------------ live state
    Meters meters_;
    std::atomic<double> tempo_ { 120.0 };
    std::atomic<bool> hostTempoValid_ { false }, tunerActive_ { false };
    std::atomic<bool> panicRequested_ { false };
    std::atomic<float> trackedNote_ { 0 }, panicFade_ { 1.0f };
    std::atomic<bool> trackedVoiced_ { false };

    tester::Gate tester_ { machineStateDir() };
    /// Read by the audio thread, written by the message thread. Always true in a
    /// release build; false in a test build until a good key has been seen.
    std::atomic<bool> testerOpen_ { ! tester::kEnabled };
    struct TesterTimer final : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    } testerTimer_;
    void applyTesterVerdict (const tester::Check&);
    std::atomic<int> liveNote_ { -1 }, liveNoteCount_ { 0 };
    std::atomic<float> liveCents_ { 0.0f };
    std::array<std::atomic<juce::uint64>, 2> midiHeld_ {};
    std::atomic<int> learnCc_ { -1 };
    std::atomic<bool> learnArmed_ { false };
    bool tunerMutes_ = true;

    //-------------------------------------- audio thread -> message thread
    // A footswitch or an expression pedal arrives on the audio thread but has to
    // change the MODEL, which only the message thread may touch. The hit is
    // packed into a lock-free ring (binding index + CC value) and an async
    // update drains it; transport-only roles (looper, panic) are applied on the
    // audio thread directly because they touch no model state.
    static constexpr int kCcRing = 32;
    std::array<std::atomic<juce::uint32>, kCcRing> ccRing_ {};
    std::atomic<juce::uint32> ccWrite_ { 0 };
    juce::uint32 ccRead_ = 0;
    std::atomic<int> pendingSceneStep_ { 0 };
    std::atomic<int> pendingSceneJump_ { -1 };    ///< a scene footswitch (A-F), 0-based
    std::atomic<bool> pendingTap_ { false };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    juce::int64 lastTapMs_ = 0;
    juce::Array<double> tapIntervals_;

    //--------------------------------------------------- host parameters
    juce::AudioParameterFloat* pMaster_ = nullptr;
    juce::AudioParameterBool* pLimiter_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowEndProcessor)
};

} // namespace lowend
