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
#include "../engine/Limiter.h"
#include "../engine/Looper.h"
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
    bool tunerActive() const { return tunerActive_.load(); }
    const Tuner& tuner() const { return tuner_; }

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
    Tuner tuner_;

    juce::AudioBuffer<float> diBuf_, synthBuf_, ampAltBuf_;

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
