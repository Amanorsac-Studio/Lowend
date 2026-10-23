// LowEndProcessor.cpp — see header.
#include "LowEndProcessor.h"
#if ! LOWEND_HEADLESS
 #include "LowEndEditor.h"
#endif

namespace lowend {

/// MESSAGE THREAD ONLY: reads a value straight out of a Rig section's map.
/// The audio thread uses `inParams_` / `outParams_` and the engine objects'
/// own stores instead — it must never touch these containers.
static float mapValue (const ParamValues& v, const char* id, float fallback)
{
    auto it = v.find (id);
    return it == v.end() ? fallback : it->second;
}

LowEndProcessor::AudioHooks& LowEndProcessor::audioHooks()
{
    static AudioHooks hooks;
    return hooks;
}

//==============================================================================
LowEndProcessor::LowEndProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // A deliberately SHORT and STABLE host parameter list. Everything else is a
    // path (RigModel.h) saved with the rig: exposing 200 parameters would make
    // the DAW's automation list change every time a block is swapped, which is
    // exactly the thing that breaks a session six months later.
    addParameter (pMaster_ = new juce::AudioParameterFloat ("master", "Master",
                       juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f));
    addParameter (pLimiter_ = new juce::AudioParameterBool ("limiter", "Limiter", true));

    preChain_.setCapacity (kMaxPreBlocks);
    postChain_.setCapacity (kMaxPostBlocks);

    library_.generateFactoryIfNeeded();
    library_.scan();

    // Land on the first factory preset so the app makes a sound the moment it
    // opens, rather than on an empty default nobody chose.
    if (! library_.presets().empty())
    {
        rig_ = library_.presets().front();
        currentPresetId_ = rig_.id;
    }
    else
        rig_ = defaultRig();
    readEdits();
    // Open where the player left off: the last preset, set and scene, with the
    // edit kept for it if there is one, or the scene's own version.
    {
        const auto sess = juce::JSON::parse (stageDir().getChildFile ("session.json"));
        const auto pid = sess.getProperty ("preset", {}).toString();
        if (const auto* r = library_.findPreset (pid))
        {
            currentPresetId_ = pid;
            rig_ = *r;
            if (const auto* sg = library_.findSong (sess.getProperty ("song", {}).toString()))
            {
                currentSongId_ = sg->id;
                loadedSongId_ = sg->id;
                scene_ = juce::jlimit (0, juce::jmax (0, sg->presetIds.size() - 1), (int) sess.getProperty ("scene", 0));
                loadedScene_ = scene_;
                if (sg->presetIds[scene_] == pid)
                    if (auto it = sg->overrides.find (scene_); it != sg->overrides.end())
                    { rig_ = it->second; rig_.id = r->id; rig_.name = r->name; overrideActive_ = true; }
            }
            if (const double t = (double) sess.getProperty ("tempo", 0.0); t > 0.0) tempo_.store (t);
        }
    }
    if (auto e = edits_.find (liveEditKey()); e != edits_.end()) rig_ = e->second.rig;
    // Test builds: decide now from the stored key (so a tester who already
    // entered one is not asked again), then re-decide every half minute so a key
    // that expires mid-session takes effect without a restart.
    if (tester::kEnabled)
    {
        testerOpen_.store (tester_.current().status == tester::Status::ok);
        testerTimer_.fn = [this] { applyTesterVerdict (tester_.refresh()); };
        testerTimer_.startTimer (30000);
    }
}

LowEndProcessor::~LowEndProcessor()
{
    stashLiveEdit();          // nothing the player changed is lost by quitting
    testerTimer_.stopTimer();
}

void LowEndProcessor::applyTesterVerdict (const tester::Check& c)
{
    const bool open = c.status == tester::Status::ok;
    if (testerOpen_.exchange (open) != open)
        sendChangeMessage();       // the editor learns the moment the gate opens or shuts
}

tester::Check LowEndProcessor::submitTesterKey (const juce::String& key)
{
    const auto c = tester_.submit (key);
    applyTesterVerdict (tester_.current());
    return c;
}

//==============================================================================
bool LowEndProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    // The standalone may open four outputs: 1-2 the rig, 3-4 the click (the
    // device decides which physical pair that is). A plug-in stays stereo; in
    // a DAW the DAW's own click and routing do this job.
    // The standalone opens whichever channels the player chose on the interface
    // (a mono or stereo main, plus the click's pair): 1 to 4 of them. A plug-in
    // stays mono or stereo; in a DAW the DAW does the routing.
    const bool standalone = wrapperType == wrapperType_Standalone && out.size() >= 1 && out.size() <= 4;
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo() && ! standalone) return false;
    // A bass goes into ONE jack. Mono in / stereo out is the normal case on
    // stage and must be supported, not just the symmetric layouts.
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo() && ! in.isDisabled())
        return false;
    return true;
}

void LowEndProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr_ = sampleRate;
    maxBlock_ = juce::jmax (1, samplesPerBlock);

    inParams_.configure (inputParams());
    outParams_.configure (outputParams());
    jassert (inParams_.verify (kTrim, "trim") && inParams_.verify (kDiLowPass, "diLowPass")
             && outParams_.verify (kLimiterOn, "limiter"));

    preChain_.prepare (sr_, maxBlock_);
    postChain_.prepare (sr_, maxBlock_);
    for (auto& a : amp_) a.prepare (sr_, maxBlock_);
    for (auto& c : cab_) c.prepare (sr_, maxBlock_);
    inputGate_.prepare (sr_);
    diDelay_.prepare (sr_, 60.0f);
    rawDelay_.prepare (sr_, 60.0f);
    bypassMix_.reset (sr_, 0.02);
    winLen_ = juce::jmax (1, (int) (sr_ * 0.1));
    winPeak_ = 0.0f; winCount_ = 0; floorIdx_ = 0; floorDb_ = -120.0f;
    floorRing_.fill (1.0f);
    meters_.noiseFloorDb.store (-120.0f);
    humSeen_ = -1;
    for (auto& ch : hum_) for (auto& f : ch) f.reset();
    bypassMix_.setCurrentAndTargetValue (bypassAll_.load() ? 1.0f : 0.0f);
    for (auto& f : diLp_) f.reset();
    limiter_.prepare (sr_, maxBlock_);
    tracker_.prepare (sr_);
    synth_.prepare (sr_, maxBlock_);
    looper_.prepare (sr_, maxBlock_);
    metronome_.prepare (sr_);
    metronome_.setSettings (click_);
    clickBuf_.setSize (2, maxBlock_);
    mainBuf_.setSize (2, maxBlock_);
    coverBuf_.setSize (2, maxBlock_);
    cover_.prepare (sr_, maxBlock_);
    tuner_.prepare (sr_, tracker_.hopSamples());

    diBuf_.setSize (2, maxBlock_);
    rawBuf_.setSize (2, maxBlock_);
    ampAltBuf_.setSize (2, maxBlock_);
    synthBuf_.setSize (1, maxBlock_);

    xfadeLength_ = (int) (sr_ * 0.025);
    xfadeRemaining_ = 0;

    rebuildChains (false);
    pushAllParams();
    refreshIrs();

    // The DI path is delay-matched to whatever latency the drives, the amp's
    // oversampler and the cab's convolution add, so blending it in cannot comb.
    const int chainLatency = preChain_.latencySamples() + amp_[activeAmp_].latencySamples()
                           + cab_[activeAmp_].latencySamples() + postChain_.latencySamples();
    diDelay_.setDelaySamples (chainLatency);
    rawDelay_.setDelaySamples (chainLatency);
    setLatencySamples (chainLatency + limiter_.latencySamples());
    // A Cover take is tapped before the limiter, so only the chain's part counts.
    rigLatency_.store (chainLatency);
    cover_.setLatency (deviceLatency_.load() + chainLatency);
}

//==============================================================================
void LowEndProcessor::updateTransport()
{
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
            {
                hostTempoValid_.store (true);
                tempo_.store (*bpm);
                const auto ppq = pos->getPpqPosition();
                hostPpq_ = (pos->getIsPlaying() && ppq.hasValue()) ? *ppq : -1.0;
                return;
            }
        }
    }
    hostTempoValid_.store (false);
    hostPpq_ = -1.0;
}

void LowEndProcessor::setClickSettings (const ClickSettings& c)
{
    click_ = c;
    click_.countInBars = juce::jlimit (0, 2, c.countInBars);
    click_.beatUnit = c.beatUnit == 8 ? 8 : 4;
    click_.loopBars = (c.loopBars == 1 || c.loopBars == 2 || c.loopBars == 4 || c.loopBars == 8 || c.loopBars == 16) ? c.loopBars : 0;
    click_.output = juce::jlimit (0, 2, c.output);
    metronome_.setSettings (click_);
    countInBars_.store (click_.countInBars);
    clickOutput_.store (click_.output);
    sendChangeMessage();
}

void LowEndProcessor::handleLoopRecord (int offset) noexcept
{
    // A second stomp during the count-in cancels it.
    if (metronome_.countingIn()) { metronome_.cancelCountIn(); return; }
    const int bars = countInBars_.load (std::memory_order_relaxed);
    if (bars > 0 && looper_.state() == Looper::State::idle)
    {
        metronome_.beginCountIn (bars, offset, tempo_.load());
        return;
    }
    looper_.recordPressed (offset);
}

void LowEndProcessor::setTempo (double bpm)
{
    tempo_.store (juce::jlimit (30.0, 300.0, bpm));
}

void LowEndProcessor::tapTempo()
{
    const auto now = juce::Time::currentTimeMillis();
    const auto delta = now - lastTapMs_;
    lastTapMs_ = now;
    if (delta > 2000 || delta < 200) { tapIntervals_.clear(); return; }   // new count-in
    tapIntervals_.add ((double) delta);
    while (tapIntervals_.size() > 4) tapIntervals_.remove (0);
    double sum = 0;
    for (auto d : tapIntervals_) sum += d;
    if (! tapIntervals_.isEmpty())
    {
        hostTempoValid_.store (false);
        setTempo (60000.0 / (sum / tapIntervals_.size()));
        sendChangeMessage();
    }
}

//==============================================================================
/// Marks a ring slot as written (index 0 with value 0 would otherwise be
/// indistinguishable from an empty slot).
static constexpr juce::uint32 kCcValidBit = 0x80000000u;

std::vector<Binding> LowEndProcessor::bindings() const
{
    const juce::SpinLock::ScopedLockType sl (bindingLock_);
    return bindings_;
}
std::vector<SwitchBinding> LowEndProcessor::switchBindings() const
{
    const juce::SpinLock::ScopedLockType sl (bindingLock_);
    return switches_;
}
void LowEndProcessor::setBindings (std::vector<Binding> b)
{
    const juce::SpinLock::ScopedLockType sl (bindingLock_);
    bindings_ = std::move (b);
}
void LowEndProcessor::setSwitchBindings (std::vector<SwitchBinding> s)
{
    const juce::SpinLock::ScopedLockType sl (bindingLock_);
    switches_ = std::move (s);
}

void LowEndProcessor::applyMidi (const juce::MidiBuffer& midi)
{
    // Keys held on a MIDI keyboard, for the Learn tab's piano. Tracked before
    // the binding lock below, so a UI edit of the mappings can never drop a
    // note-off and leave a key stuck lit.
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())
        {
            const int n = m.getNoteNumber();
            midiHeld_[(size_t) (n >> 6)].fetch_or (1ull << (n & 63));
        }
        else if (m.isNoteOff())
        {
            const int n = m.getNoteNumber();
            midiHeld_[(size_t) (n >> 6)].fetch_and (~(1ull << (n & 63)));
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            midiHeld_[0].store (0);
            midiHeld_[1].store (0);
        }
    }

    // TRY-lock, exactly as the pedal chain does: if the UI happens to be
    // replacing the map in this instant, this block's CCs are dropped rather
    // than the audio thread waiting. One buffer of a knob move is not a bug;
    // a priority inversion on stage is.
    const juce::SpinLock::ScopedTryLockType sl (bindingLock_);
    if (! sl.isLocked()) return;

    bool needsAsync = false;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (! m.isController()) continue;
        const int cc = m.getControllerNumber();
        const int value = m.getControllerValue();

        // MIDI learn: the audio thread only ever reads an atomic flag here — the
        // path string it belongs to is message-thread state.
        if (learnArmed_.load (std::memory_order_relaxed)) { learnCc_.store (cc); continue; }

        for (auto& s : switches_)
        {
            if (s.cc != cc || value < 64) continue;
            switch (s.role)
            {
                // Looper stomps land on their own sample (meta.samplePosition).
                case SwitchRole::loopRecord: handleLoopRecord (meta.samplePosition); break;
                case SwitchRole::loopStop:   metronome_.cancelCountIn(); looper_.stopPressed (meta.samplePosition); break;
                case SwitchRole::loopUndo:   looper_.undoPressed (meta.samplePosition); break;
                case SwitchRole::loopClear:  metronome_.cancelCountIn(); looper_.clearPressed (meta.samplePosition); break;
                case SwitchRole::panic:      panicRequested_.store (true); break;
                // The rest change the MODEL, which only the message thread may
                // touch, so they are handed over rather than done here.
                case SwitchRole::presetNext: pendingSceneStep_.fetch_add (1); break;
                case SwitchRole::presetPrev: pendingSceneStep_.fetch_add (-1); break;
                case SwitchRole::tapTempo:   pendingTap_.store (true); break;
                case SwitchRole::tuner:      tunerActive_.store (! tunerActive_.load()); break;
                case SwitchRole::bypassAll:  bypassAll_.store (! bypassAll_.load()); needsAsync = true; break;
                case SwitchRole::sceneA: case SwitchRole::sceneB: case SwitchRole::sceneC:
                case SwitchRole::sceneD: case SwitchRole::sceneE: case SwitchRole::sceneF:
                    pendingSceneJump_.store ((int) s.role - (int) SwitchRole::sceneA);
                    break;
                default: break;
            }
        }

        for (size_t i = 0; i < bindings_.size() && i < 255; ++i)
        {
            if (bindings_[i].cc != cc) continue;
            const juce::uint32 packed = ((juce::uint32) i << 8) | (juce::uint32) juce::jlimit (0, 127, value);
            const auto w = ccWrite_.fetch_add (1, std::memory_order_acq_rel);
            ccRing_[(size_t) (w % kCcRing)].store (packed | kCcValidBit, std::memory_order_release);
            needsAsync = true;
        }
    }
    if (pendingSceneStep_.load() != 0 || pendingTap_.load() || pendingSceneJump_.load() >= 0) needsAsync = true;
    if (needsAsync) triggerAsyncUpdate();
}

//==============================================================================
/// MESSAGE THREAD. Everything a footswitch or expression pedal wanted to do to
/// the MODEL happens here, one hop after the audio thread saw it.
void LowEndProcessor::handleAsyncUpdate()
{
    if (pendingTap_.exchange (false)) tapTempo();
    if (const bool b = bypassAll_.load(); b != bypassSeen_) { bypassSeen_ = b; sendChangeMessage(); }   // a footswitch toggled it

    if (const int steps = pendingSceneStep_.exchange (0); steps != 0)
        nextScene (steps > 0 ? 1 : -1);
    if (const int k = pendingSceneJump_.exchange (-1); k >= 0)
        jumpToScene (k);

    const auto snapshot = bindings();
    const auto w = ccWrite_.load (std::memory_order_acquire);
    while (ccRead_ != w)
    {
        const auto packed = ccRing_[(size_t) (ccRead_ % kCcRing)].exchange (0, std::memory_order_acq_rel);
        ++ccRead_;
        if ((packed & kCcValidBit) == 0) continue;
        const size_t index = (packed & ~kCcValidBit) >> 8;
        const int value = (int) (packed & 0xff);
        if (index >= snapshot.size()) continue;
        const auto& b = snapshot[index];
        const float t = value / 127.0f;
        setPath (b.path, b.toggle ? (value >= 64 ? b.hi : b.lo) : b.lo + t * (b.hi - b.lo));
    }
}

//==============================================================================
void LowEndProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nOut = buffer.getNumChannels();
    if (n <= 0 || nOut <= 0) return;

    // A test build with no valid key is silent. One relaxed atomic read; the
    // verdict itself is decided on the message thread.
    if (! testerOpen_.load (std::memory_order_relaxed))
    {
        buffer.clear();
        return;
    }

    updateTransport();
    applyMidi (midi);
    midi.clear();

    // Mono input into a stereo working buffer: the amp and cab are mono devices
    // and everything stereo happens after them.
    if (nOut > 1 && getTotalNumInputChannels() == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, n);
    for (int ch = 2; ch < nOut; ++ch) buffer.clear (ch, 0, n);
    const int nCh = juce::jmin (2, nOut);

    if (panicRequested_.exchange (false))
    {
        synth_.panic();
        preChain_.reset();
        postChain_.reset();
        for (auto& a : amp_) a.reset();
        for (auto& c : cab_) c.reset();
        limiter_.reset();
        tracker_.reset();
        buffer.clear();
        return;
    }

    const double bpm = tempo_.load();
    looper_.setTempo (bpm, metronome_.beatsPerBar());
    looper_.setFixedBars (click_.loopBars);
    // The click switched on under a playing loop joins the loop where it is.
    {
        const bool clickOn = metronome_.isOn();
        const auto ls = looper_.state();
        if (clickOn && ! clickWasOn_ && looper_.lengthSamples() > 0
            && (ls == Looper::State::playing || ls == Looper::State::overdubbing))
        {
            const double bar = metronome_.barSamples (bpm);
            if (bar > 0.0) metronome_.setBarPhase (std::fmod ((double) looper_.positionSamples(), bar), bpm);
        }
        clickWasOn_ = clickOn;
    }

    // UI record stomps, then a count-in that finishes inside this block.
    if (uiCancelCountIn_.exchange (false, std::memory_order_acq_rel)) metronome_.cancelCountIn();
    for (int k = uiLoopRecord_.exchange (0, std::memory_order_acq_rel); k > 0; --k)
        handleLoopRecord (0);
    if (const int due = metronome_.countInDue (n); due >= 0)
        looper_.recordPressed (due);
    // Bar/Beat quantise only when the player can hear the bar.
    // The looper's bar is the click's bar whenever the click (or a count-in) runs.
    if (metronome_.isOn() || metronome_.countingIn())
        looper_.setClickGrid (metronome_.barPhase (bpm), metronome_.barSamples (bpm), metronome_.pulseSamples (bpm));
    else
        looper_.setClickGrid (0.0, 0.0, 0.0);
    looper_.setGridActive (metronome_.isOn() || metronome_.countingIn()
                           || countInBars_.load (std::memory_order_relaxed) > 0 || hostTempoValid_.load());

    //------------------------------------------------------------ input stage
    const float trim = juce::Decibels::decibelsToGain (inParams_[kTrim]);
    const bool gateOn = inParams_.flag (kGateOn);
    // The gate's threshold: the preset's, lifted above the measured noise floor
    // when Auto gate is on (a floor is only ever lifted, and never past -30 dBFS,
    // so quiet playing is not gated by a noisy interface).
    float gateThreshold = inParams_[kGateThreshold];
    if (gateAuto_.load (std::memory_order_relaxed) && floorDb_ > -119.0f)
        gateThreshold = juce::jmin (-30.0f, juce::jmax (gateThreshold, floorDb_ + 12.0f));
    inputGate_.setParams (gateThreshold, inParams_[kGateRelease], 1.0f);
    // The hum filter: narrow cuts at the mains frequency and its harmonics.
    const int humHz = humHz_.load (std::memory_order_relaxed);
    if (humHz != humSeen_)
    {
        humSeen_ = humHz;
        for (auto& ch : hum_)
            for (int k = 0; k < kHumHarmonics; ++k)
            {
                if (humHz > 0) ch[(size_t) k].setNotch (sr_, (float) (humHz * (k + 1)), 35.0f);
                else ch[(size_t) k].setBypass();
                ch[(size_t) k].reset();
            }
    }

    float inPeak = 0.0f;
    auto* l = buffer.getWritePointer (0);
    auto* r = nCh > 1 ? buffer.getWritePointer (1) : nullptr;
    if (rawBuf_.getNumSamples() < n) rawBuf_.setSize (2, n, false, false, true);
    for (int i = 0; i < n; ++i)
    {
        l[i] *= trim;
        if (r) r[i] *= trim;
        if (humHz > 0)
        {
            for (auto& f : hum_[0]) l[i] = f.process (l[i]);
            if (r) for (auto& f : hum_[1]) r[i] = f.process (r[i]);
        }
        // The raw instrument for Bypass All: after the input trim, before the
        // gate and everything else, delayed to arrive with the processed path.
        rawDelay_.push (l[i], r ? r[i] : l[i]);
        rawBuf_.setSample (0, i, rawDelay_.read (0));
        rawBuf_.setSample (1, i, rawDelay_.read (1));
        const float mono = r ? 0.5f * (l[i] + r[i]) : l[i];
        // Noise floor: the quietest 100 ms window of the last 3 s, counting only
        // windows below -40 dBFS (nothing is being played in them).
        winPeak_ = juce::jmax (winPeak_, std::abs (mono));
        if (++winCount_ >= winLen_)
        {
            floorRing_[(size_t) floorIdx_] = winPeak_ < 0.01f ? juce::jmax (winPeak_, 1.0e-6f) : 1.0f;
            floorIdx_ = (floorIdx_ + 1) % (int) floorRing_.size();
            float quietest = 1.0f;
            for (auto v : floorRing_) quietest = juce::jmin (quietest, v);
            if (quietest < 0.01f)
            {
                floorDb_ = juce::Decibels::gainToDecibels (quietest, -120.0f);
                meters_.noiseFloorDb.store (floorDb_, std::memory_order_relaxed);
            }
            winPeak_ = 0.0f; winCount_ = 0;
        }
        if (gateOn)
        {
            const float g = inputGate_.nextGain (mono);
            l[i] *= g;
            if (r) r[i] *= g;
        }
        inPeak = juce::jmax (inPeak, std::abs (l[i]));
    }
    meters_.inPeak.store (inPeak);
    meters_.gateOpen.store (! gateOn || inputGate_.isOpen());

    //---------------------------------------------------- DI tap (pre-drive)
    const bool diOn = inParams_.flag (kDiOn);
    if (diOn)
    {
        if (diBuf_.getNumSamples() < n) diBuf_.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            diDelay_.push (l[i], r ? r[i] : l[i]);
            diBuf_.setSample (0, i, diDelay_.read (0));
            diBuf_.setSample (1, i, diDelay_.read (1));
        }
    }

    //------------------------------------------------- pitch tracking, synth voice
    // The tracker runs every block (it also feeds the tuner). When the synth is
    // on, each note boundary it finds is applied at its own sample offset and
    // the voice is rendered in the segments between them, so a note starts
    // where it was detected rather than at the top of the next buffer.
    if (synthBuf_.getNumSamples() < n) synthBuf_.setSize (1, n, false, false, true);
    synthBuf_.clear (0, 0, n);
    float* synthMono = synthBuf_.getWritePointer (0);
    const bool synthOn = synth_.enabled();

    int rendered = 0;
    for (int i = 0; i < n; ++i)
    {
        tracker_.pushSample (r ? 0.5f * (l[i] + r[i]) : l[i]);
        if (! tracker_.hopDue()) continue;

        const auto& res = tracker_.analyse();
        tuner_.update (res);
        trackedNote_.store (res.note);
        trackedVoiced_.store (res.voiced);

        NoteEvent evs[2];
        const int k = tracker_.collectEvents (evs, 2);

        // Learn tab: publish the played note. A counter ticks on every note-on
        // so the UI lists a repeated note (A, A, A) as three events, not one.
        for (int j = 0; j < k; ++j)
        {
            if (evs[j].kind == NoteEvent::Kind::on)
            {
                liveNote_.store ((int) std::lround (evs[j].note));
                liveNoteCount_.fetch_add (1);
            }
            else if (evs[j].kind == NoteEvent::Kind::off)
                liveNote_.store (-1);
        }
        if (res.voiced && liveNote_.load() >= 0)
            liveCents_.store ((res.note - std::round (res.note)) * 100.0f);

        if (synthOn && k > 0)
        {
            if (i > rendered) { synth_.render (synthMono + rendered, i - rendered); rendered = i; }
            for (int j = 0; j < k; ++j) synth_.handle (evs[j]);
        }
    }
    if (n > rendered) synth_.render (synthMono + rendered, n - rendered);

    float synthPeak = 0.0f;
    for (int i = 0; i < n; ++i) synthPeak = juce::jmax (synthPeak, std::abs (synthMono[i]));
    meters_.synthPeak.store (synthPeak);

    const bool synthPreAmp = synthOn && synth_.route() == SynthRoute::preAmp;
    if (synthPreAmp)
        for (int ch = 0; ch < nCh; ++ch)
            buffer.addFrom (ch, 0, synthMono, n);

    //------------------------------------------------------------- pedalboard
    preChain_.process (buffer, bpm);

    //------------------------------------------------------------- amp + cab
    if (xfadeRemaining_ > 0)
    {
        // Both rigs render; equal-power crossfade between them. 25 ms, so the
        // cost is two amps for one buffer at a scene change and one the rest of
        // the time.
        const int incoming = 1 - activeAmp_;
        if (ampAltBuf_.getNumSamples() < n) ampAltBuf_.setSize (2, n, false, false, true);
        for (int ch = 0; ch < nCh; ++ch) ampAltBuf_.copyFrom (ch, 0, buffer, ch, 0, n);

        amp_[(size_t) activeAmp_].process (buffer);
        cab_[(size_t) activeAmp_].process (buffer);
        amp_[(size_t) incoming].process (ampAltBuf_);
        cab_[(size_t) incoming].process (ampAltBuf_);

        for (int i = 0; i < n; ++i)
        {
            const int done = xfadeLength_ - xfadeRemaining_ + i;
            float gOld = 0.0f, gNew = 1.0f;
            equalPower ((float) done / (float) juce::jmax (1, xfadeLength_), gOld, gNew);
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* w = buffer.getWritePointer (ch);
                w[i] = w[i] * gOld + ampAltBuf_.getSample (ch, i) * gNew;
            }
        }
        xfadeRemaining_ -= n;
        if (xfadeRemaining_ <= 0) { xfadeRemaining_ = 0; activeAmp_ = incoming; }
    }
    else
    {
        amp_[(size_t) activeAmp_].process (buffer);
        cab_[(size_t) activeAmp_].process (buffer);
    }
    meters_.ampSupply.store (amp_[(size_t) activeAmp_].supply());

    //-------------------------------------------------------------- post rack
    postChain_.process (buffer, bpm);

    if (synthOn && ! synthPreAmp)
        for (int ch = 0; ch < nCh; ++ch)
            buffer.addFrom (ch, 0, synthMono, n);

    //--------------------------------------------------------------- DI blend
    if (diOn)
    {
        const float diGain = juce::Decibels::decibelsToGain (inParams_[kDiLevel]);
        const float diCut = inParams_[kDiLowPass];
        for (int ch = 0; ch < nCh; ++ch)
        {
            diLp_[ch].setLowPass (sr_, diCut, 0.707f);
            auto* w = buffer.getWritePointer (ch);
            for (int i = 0; i < n; ++i) w[i] += diLp_[ch].process (diBuf_.getSample (ch, i)) * diGain;
        }
    }

    //------------------------------------------------------------ bypass all
    bypassMix_.setTargetValue (bypassAll_.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    if (bypassMix_.isSmoothing() || bypassMix_.getCurrentValue() > 0.0f)
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = bypassMix_.getNextValue();
            for (int ch = 0; ch < nCh; ++ch)
            {
                auto* w = buffer.getWritePointer (ch);
                w[i] = w[i] * (1.0f - g) + rawBuf_.getSample (ch, i) * g;
            }
        }
    }

    //------------------------------------------------------------ cover take
    // The rig's output and the raw instrument, before the looper and the
    // master, so a take does not depend on the monitoring level.
    cover_.capture (buffer.getReadPointer (0), buffer.getReadPointer (nCh > 1 ? 1 : 0),
                    rawBuf_.getReadPointer (0), rawBuf_.getReadPointer (1), n);

    //---------------------------------------------------------------- looper
    looper_.process (buffer);

    //---------------------------------------------------------------- output
    if (tunerActive_.load() && tunerMutes_)
        buffer.applyGain (0.0f);

    const float master = juce::Decibels::decibelsToGain (
        juce::jlimit (-60.0f, 12.0f, outParams_[kMasterDb] + pMaster_->get()));
    buffer.applyGain (0, n, master);

    limiter_.setEnabled (outParams_.flag (kLimiterOn) && pLimiter_->get());
    limiter_.process (buffer);
    meters_.gainReduction.store (limiter_.gainReductionDb());

    //------------------------------------------------------------ cover song
    // After the limiter, like the click: the song must not pump the guitar.
    if (coverBuf_.getNumSamples() < n) coverBuf_.setSize (2, n, false, false, true);
    coverBuf_.clear (0, n);
    cover_.render (coverBuf_.getWritePointer (0), coverBuf_.getWritePointer (1), n);
    if (cover_.isPlaying() || coverBuf_.getMagnitude (0, n) > 0.0f)
    {
        if (nCh > 1) { buffer.addFrom (0, 0, coverBuf_, 0, 0, n); buffer.addFrom (1, 0, coverBuf_, 1, 0, n); }
        else { buffer.addFrom (0, 0, coverBuf_, 0, 0, n, 0.5f); buffer.addFrom (0, 0, coverBuf_, 1, 0, n, 0.5f); }
    }

    //----------------------------------------------------------------- click
    // After the looper (never recorded into a loop) and after the limiter (it
    // must not pump the bass).
    {
        if (clickBuf_.getNumSamples() < n) clickBuf_.setSize (2, n, false, false, true);
        auto* cl = clickBuf_.getWritePointer (0);
        auto* cr = clickBuf_.getWritePointer (1);
        metronome_.render (cl, cr, n, bpm, hostPpq_);
        const int clickAt = outClick_.load (std::memory_order_relaxed);
        const bool separate = clickAt >= 0 && clickAt + 1 < nOut;
        clickSeparate_.store (separate, std::memory_order_relaxed);
        const int route = clickOutput_.load (std::memory_order_relaxed);
        const bool toMain = route != ClickSettings::separate || ! separate;
        if (toMain)
            for (int ch = 0; ch < nCh; ++ch) buffer.addFrom (ch, 0, clickBuf_, ch, 0, n);
        if (! (route != ClickSettings::mainMix && separate)) clickBuf_.clear (0, n);   // the click goes only to the main mix
        countingInUi_.store (metronome_.countingIn(), std::memory_order_relaxed);
        clickBeatUi_.store (metronome_.beatForUi(), std::memory_order_relaxed);
    }

    //------------------------------------------------------------ output map
    // The path is processed on channels 0-1; here it is put where the player
    // chose: a stereo pair, or one channel in mono (the two sides summed), and
    // the click on its own pair. Channels not chosen are silent.
    {
        const int mainAt = juce::jlimit (0, juce::jmax (0, nOut - 1), outMain_.load (std::memory_order_relaxed));
        const bool mono = outMono_.load (std::memory_order_relaxed) || nOut == 1;
        const int clickAt = outClick_.load (std::memory_order_relaxed);
        const bool clickOwn = clickAt >= 0 && clickAt + 1 < nOut && clickSeparate_.load (std::memory_order_relaxed);
        if (mainBuf_.getNumSamples() < n) mainBuf_.setSize (2, n, false, false, true);
        mainBuf_.copyFrom (0, 0, buffer, 0, 0, n);
        mainBuf_.copyFrom (1, 0, buffer, nCh > 1 ? 1 : 0, 0, n);
        if (! (mainAt == 0 && ! mono && ! clickOwn && nOut <= 2))
        {
            buffer.clear();
            if (mono)
            {
                auto* d = buffer.getWritePointer (mainAt);
                const auto* a = mainBuf_.getReadPointer (0);
                const auto* b = mainBuf_.getReadPointer (1);
                for (int i = 0; i < n; ++i) d[i] = 0.5f * (a[i] + b[i]);
            }
            else
            {
                buffer.copyFrom (mainAt, 0, mainBuf_, 0, 0, n);
                if (mainAt + 1 < nOut) buffer.copyFrom (mainAt + 1, 0, mainBuf_, 1, 0, n);
            }
            if (clickOwn)
            {
                buffer.addFrom (clickAt, 0, clickBuf_, 0, 0, n);
                buffer.addFrom (clickAt + 1, 0, clickBuf_, 1, 0, n);
            }
        }
    }

    float outPeak = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        outPeak = juce::jmax (outPeak, mainBuf_.getMagnitude (ch, 0, n));
    meters_.outPeak.store (outPeak);
    meters_.clipping.store (outPeak > 0.999f);
}

//==============================================================================
// Model edits — MESSAGE THREAD
//==============================================================================
bool LowEndProcessor::setPath (const juce::String& path, float value, bool notify)
{
    const auto info = pathInfo (rig_, path);
    const bool structural = path.endsWith (".type");
    if (! lowend::setPath (rig_, path, value)) return false;

    if (structural)
    {
        rebuildChains (true);
    }
    else if (path.startsWith ("pre.") || path.startsWith ("post."))
    {
        juce::String section, param; int index = -1;
        detail::splitPath (path, section, index, param);
        auto& chain = section == "pre" ? preChain_ : postChain_;
        if (param == "bypass") chain.setBypassed (index, value >= 0.5f);
        else chain.setParam (index, param.toStdString(), value);
    }
    else if (path.startsWith ("amp.")) amp_[(size_t) targetAmp()].setParams (rig_.amp);
    else if (path.startsWith ("cab."))
    {
        cab_[(size_t) targetAmp()].setParams (rig_.cab);
        if (path == "cab.model" || path == "cab.ir") refreshIrs();
    }
    else if (path.startsWith ("synth.")) synth_.setParams (rig_.synth);
    else if (path.startsWith ("loop.")) looper_.setParams (rig_.loop);
    else if (path.startsWith ("in.")) inParams_.setAll (rig_.in);
    else if (path.startsWith ("out.")) outParams_.setAll (rig_.out);

    juce::ignoreUnused (info);
    if (notify) sendChangeMessage();
    return true;
}

float LowEndProcessor::getPath (const juce::String& path, bool* ok) const
{
    return lowend::getPath (rig_, path, ok);
}

void LowEndProcessor::pushAllParams()
{
    inParams_.setAll (rig_.in);
    outParams_.setAll (rig_.out);
    amp_[(size_t) targetAmp()].setParams (rig_.amp);
    cab_[(size_t) targetAmp()].setParams (rig_.cab);
    synth_.setParams (rig_.synth);
    looper_.setParams (rig_.loop);
}

void LowEndProcessor::rebuildChains (bool crossfade)
{
    if (crossfade)
    {
        preChain_.applyStructuralChange (rig_.pre);
        postChain_.applyStructuralChange (rig_.post);
    }
    else
    {
        preChain_.setState (rig_.pre);
        postChain_.setState (rig_.post);
    }
}

void LowEndProcessor::loadRig (const Rig& r, bool crossfade)
{
    rig_ = r;
    inParams_.setAll (rig_.in);
    outParams_.setAll (rig_.out);
    if (crossfade && sr_ > 0)
    {
        // Set the IDLE amp+cab up with the new rig and start the crossfade; the
        // pedalboard does its own dry hand-over over the same 25 ms.
        const int idle = 1 - activeAmp_;
        amp_[(size_t) idle].setParams (rig_.amp);
        cab_[(size_t) idle].setParams (rig_.cab);
        xfadeRemaining_ = juce::jmax (1, xfadeLength_);
    }
    else
    {
        amp_[(size_t) activeAmp_].setParams (rig_.amp);
        cab_[(size_t) activeAmp_].setParams (rig_.cab);
    }
    rebuildChains (crossfade);
    synth_.setParams (rig_.synth);
    looper_.setParams (rig_.loop);
    refreshIrs();
    sendChangeMessage();
}

//------------------------------------------------------------- chain editing
bool LowEndProcessor::addBlock (bool post, BlockType t, int index)
{
    auto& chain = post ? rig_.post : rig_.pre;
    const int cap = post ? kMaxPostBlocks : kMaxPreBlocks;
    if ((int) chain.size() >= cap) return false;
    const int at = index < 0 || index > (int) chain.size() ? (int) chain.size() : index;
    chain.insert (chain.begin() + at, defaultBlockState (t));
    rebuildChains (true);
    sendChangeMessage();
    return true;
}

bool LowEndProcessor::removeBlock (bool post, int index)
{
    auto& chain = post ? rig_.post : rig_.pre;
    if (index < 0 || index >= (int) chain.size()) return false;
    chain.erase (chain.begin() + index);
    rebuildChains (true);
    sendChangeMessage();
    return true;
}

bool LowEndProcessor::moveBlock (bool post, int from, int to)
{
    auto& chain = post ? rig_.post : rig_.pre;
    if (from < 0 || from >= (int) chain.size() || to < 0 || to >= (int) chain.size() || from == to) return false;
    auto moved = chain[(size_t) from];
    chain.erase (chain.begin() + from);
    chain.insert (chain.begin() + to, std::move (moved));
    rebuildChains (true);
    sendChangeMessage();
    return true;
}

//---------------------------------------------------------------- performance
void LowEndProcessor::loadPreset (const juce::String& presetId)
{
    auto* r = library_.findPreset (presetId);
    if (r == nullptr) return;
    stashLiveEdit();                      // the edits on the rig being left are kept
    // Which scene of the open set is this? A preset chosen from the list may
    // also be a scene of the set; the scene decides whether an override plays.
    overrideActive_ = false;
    const Rig* base = r;
    if (auto* sg = library_.findSong (currentSongId_))
    {
        if (scene_ < 0 || scene_ >= sg->presetIds.size() || sg->presetIds[scene_] != presetId)
            scene_ = juce::jmax (0, sg->presetIds.indexOf (presetId));
        if (sg->presetIds[scene_] == presetId)
            if (auto it = sg->overrides.find (scene_); it != sg->overrides.end()) { base = &it->second; overrideActive_ = true; }
    }
    currentPresetId_ = presetId;
    loadedSongId_ = library_.findSong (currentSongId_) != nullptr ? currentSongId_ : juce::String();
    loadedScene_ = loadedSongId_.isNotEmpty() ? scene_ : -1;
    const auto key = liveEditKey();
    if (auto e = edits_.find (key); e != edits_.end())
        loadRig (e->second.rig, true);     // come back to it as it was left
    else
    {
        auto rig = *base;
        rig.id = r->id; rig.name = r->name;
        loadRig (rig, true);
    }
    writeSession();
}

const Rig* LowEndProcessor::baseRig() const
{
    if (overrideActive_)
        if (auto* sg = library_.findSong (loadedSongId_))
            if (auto it = sg->overrides.find (loadedScene_); it != sg->overrides.end()) return &it->second;
    return library_.findPreset (currentPresetId_);
}

bool LowEndProcessor::isDirty() const
{
    if (currentPresetId_.isEmpty()) return true;
    const auto* b = baseRig();
    return b == nullptr || ! sameSound (rig_, *b);
}

void LowEndProcessor::stashLiveEdit()
{
    if (currentPresetId_.isEmpty()) return;
    const auto key = liveEditKey();
    if (isDirty()) edits_[key] = { rig_, currentPresetId_, loadedSongId_, loadedScene_ };
    else edits_.erase (key);
    writeEdits();
}

std::vector<LowEndProcessor::PendingEdit> LowEndProcessor::pendingEdits() const
{
    std::vector<PendingEdit> out;
    const auto liveKey = liveEditKey();
    for (auto& [k, e] : edits_) if (k != liveKey) out.push_back (e);
    if (! currentPresetId_.isEmpty() && isDirty())
        out.push_back ({ rig_, currentPresetId_, loadedSongId_, loadedScene_ });
    return out;
}

bool LowEndProcessor::resolveEdit (const juce::String& key, Resolve how)
{
    // The live rig's edit is resolved from the live rig itself.
    const bool live = key == liveEditKey();
    PendingEdit e;
    if (live) e = { rig_, currentPresetId_, loadedSongId_, loadedScene_ };
    else if (auto it = edits_.find (key); it != edits_.end()) e = it->second;
    else return false;
    const bool isOverride = key.containsChar ('@');

    if (how == Resolve::toPreset)
    {
        auto rig = e.rig;
        if (const auto* before = library_.findPreset (e.presetId))
        {
            rig.id = before->id; rig.name = before->name; rig.order = before->order;
            rig.history = before->history;
            noteChange (rig, describeRigChange (*before, rig));
        }
        library_.savePreset (rig);
        // A preset saved for everyone replaces that scene's set-only version.
        if (isOverride)
            if (auto* sg = library_.findSongMutable (e.songId)) { sg->overrides.erase (e.scene); library_.saveSong (*sg); }
    }
    else if (how == Resolve::toSet)
    {
        auto* sg = library_.findSongMutable (e.songId);
        if (sg == nullptr || e.scene < 0) return false;
        auto rig = e.rig;
        noteChange (rig, "Saved to " + sg->name + " only");
        sg->overrides[e.scene] = rig;
        library_.saveSong (*sg);
    }
    edits_.erase (key);
    if (live)
    {
        if (how == Resolve::toSet) overrideActive_ = true;
        if (how == Resolve::discard) { const auto id = currentPresetId_; currentPresetId_ = {}; loadPreset (id); }
    }
    writeEdits();
    sendChangeMessage();
    return true;
}

void LowEndProcessor::writeEdits() const
{
    juce::Array<juce::var> arr;
    for (auto& [k, e] : edits_)
    {
        auto o = juce::var (new juce::DynamicObject());
        o.getDynamicObject()->setProperty ("key", k);
        o.getDynamicObject()->setProperty ("preset", e.presetId);
        o.getDynamicObject()->setProperty ("song", e.songId);
        o.getDynamicObject()->setProperty ("scene", e.scene);
        o.getDynamicObject()->setProperty ("rig", rigToVar (e.rig));
        arr.add (o);
    }
    stageDir().createDirectory();
    stageDir().getChildFile ("edits.json").replaceWithText (juce::JSON::toString (juce::var (arr), true));
}

void LowEndProcessor::readEdits()
{
    edits_.clear();
    const auto journal = juce::JSON::parse (stageDir().getChildFile ("edits.json"));   // keep it alive while it is read
    if (auto* arr = journal.getArray())
        for (auto& o : *arr)
        {
            PendingEdit e { rigFromVar (o.getProperty ("rig", {})), o.getProperty ("preset", {}).toString(),
                            o.getProperty ("song", {}).toString(), (int) o.getProperty ("scene", -1) };
            if (library_.findPreset (e.presetId) != nullptr) edits_[o.getProperty ("key", {}).toString()] = e;
        }
}

void LowEndProcessor::loadSong (const juce::String& songId, int sceneIndex)
{
    if (auto* s = library_.findSong (songId))
    {
        currentSongId_ = songId;
        scene_ = juce::jlimit (0, juce::jmax (0, s->presetIds.size() - 1), sceneIndex);
        setTempo (s->tempo);
        if (scene_ < s->presetIds.size()) loadPreset (s->presetIds[scene_]);
        writeSession();
    }
}

void LowEndProcessor::clearSong()
{
    currentSongId_ = {};
    scene_ = 0;
    writeSession();
    sendChangeMessage();
}

void LowEndProcessor::jumpToScene (int index)
{
    if (auto* s = library_.findSong (currentSongId_))
    {
        if (index < 0 || index >= s->presetIds.size()) return;
        scene_ = index;
        loadPreset (s->presetIds[scene_]);
        return;
    }
    const auto& all = library_.presets();
    if (index >= 0 && index < (int) all.size()) loadPreset (all[(size_t) index].id);
}

void LowEndProcessor::nextScene (int delta)
{
    if (auto* s = library_.findSong (currentSongId_))
    {
        if (s->presetIds.isEmpty()) return;
        scene_ = juce::jlimit (0, s->presetIds.size() - 1, scene_ + delta);
        loadPreset (s->presetIds[scene_]);
        return;
    }
    // No song loaded: step through the preset list itself.
    const auto& all = library_.presets();
    if (all.empty()) return;
    int idx = 0;
    for (size_t i = 0; i < all.size(); ++i) if (all[i].id == currentPresetId_) { idx = (int) i; break; }
    idx = juce::jlimit (0, (int) all.size() - 1, idx + delta);
    loadPreset (all[(size_t) idx].id);
}

void LowEndProcessor::setTunerActive (bool on)
{
    tunerActive_.store (on);
    sendChangeMessage();
}

void LowEndProcessor::panic()
{
    panicRequested_.store (true);
    looper_.stopPressed();
    sendChangeMessage();
}

void LowEndProcessor::refreshIrs()
{
    const int irIndex = (int) mapValue (rig_.cab, "ir", -1.0f);
    const bool wantsIr = (int) mapValue (rig_.cab, "model", 2.0f) == (int) CabModel::userIr;
    for (auto& c : cab_)
    {
        if (! wantsIr || irIndex < 0 || irIndex >= library_.irs().size()) { c.clearIr(); continue; }
        c.loadIr (juce::File (library_.irs()[irIndex]));
    }
}

void LowEndProcessor::writeSession()
{
    auto o = juce::var (new juce::DynamicObject());
    o.getDynamicObject()->setProperty ("preset", currentPresetId_);
    o.getDynamicObject()->setProperty ("song", currentSongId_);
    o.getDynamicObject()->setProperty ("scene", scene_);
    o.getDynamicObject()->setProperty ("tempo", tempo_.load());
    stageDir().createDirectory();
    stageDir().getChildFile ("session.json").replaceWithText (juce::JSON::toString (o, false));
}

//==============================================================================
// Plugin state
//==============================================================================
void LowEndProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto o = juce::var (new juce::DynamicObject());
    auto* d = o.getDynamicObject();
    d->setProperty ("schema", 1);
    d->setProperty ("rig", rigToVar (rig_));
    d->setProperty ("presetId", currentPresetId_);
    d->setProperty ("songId", currentSongId_);
    d->setProperty ("scene", scene_);
    d->setProperty ("tempo", tempo_.load());
    d->setProperty ("tunerRef", tuner_.referenceHz());
    d->setProperty ("tunerMutes", tunerMutes_);
    d->setProperty ("gateAuto", gateAuto_.load());
    d->setProperty ("humHz", humHz_.load());
    {
        auto c = juce::var (new juce::DynamicObject());
        auto* cd = c.getDynamicObject();
        cd->setProperty ("on", click_.on);
        cd->setProperty ("level", click_.levelDb);
        cd->setProperty ("sound", click_.sound);
        cd->setProperty ("accent", click_.accent);
        cd->setProperty ("beats", click_.beatsPerBar);
        cd->setProperty ("unit", click_.beatUnit);
        cd->setProperty ("loopBars", click_.loopBars);
        cd->setProperty ("eighths", click_.eighths);
        cd->setProperty ("countIn", click_.countInBars);
        cd->setProperty ("output", click_.output);
        d->setProperty ("click", c);
    }

    juce::Array<juce::var> binds;
    for (auto& b : bindings_)
    {
        auto e = juce::var (new juce::DynamicObject());
        e.getDynamicObject()->setProperty ("cc", b.cc);
        e.getDynamicObject()->setProperty ("path", b.path);
        e.getDynamicObject()->setProperty ("lo", b.lo);
        e.getDynamicObject()->setProperty ("hi", b.hi);
        e.getDynamicObject()->setProperty ("toggle", b.toggle);
        binds.add (e);
    }
    d->setProperty ("bindings", binds);

    juce::Array<juce::var> sw;
    for (auto& s : switches_)
    {
        auto e = juce::var (new juce::DynamicObject());
        e.getDynamicObject()->setProperty ("cc", s.cc);
        e.getDynamicObject()->setProperty ("role", switchRoleId (s.role));
        sw.add (e);
    }
    d->setProperty ("switches", sw);

    const auto json = juce::JSON::toString (o, false);
    dest.replaceAll (json.toRawUTF8(), json.getNumBytesAsUTF8());
}

void LowEndProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto v = juce::JSON::parse (juce::String::createStringFromData (data, size));
    if (! v.isObject()) return;

    juce::StringArray missing;
    auto r = rigFromVar (v.getProperty ("rig", {}), &missing);
    if (! missing.isEmpty())
        juce::Logger::writeToLog ("Low End: session references unknown blocks: " + missing.joinIntoString (", "));
    currentPresetId_ = v.getProperty ("presetId", {}).toString();
    currentSongId_ = v.getProperty ("songId", {}).toString();
    scene_ = (int) v.getProperty ("scene", 0);
    setTempo ((double) v.getProperty ("tempo", 120.0));
    tuner_.setReferenceHz ((float) (double) v.getProperty ("tunerRef", 440.0));
    tunerMutes_ = (bool) v.getProperty ("tunerMutes", true);
    gateAuto_.store ((bool) v.getProperty ("gateAuto", true));
    setHumHz ((int) v.getProperty ("humHz", 0));
    if (const auto c = v.getProperty ("click", {}); c.isObject())
    {
        ClickSettings cs;
        cs.on = (bool) c.getProperty ("on", false);
        cs.levelDb = (float) (double) c.getProperty ("level", -6.0);
        cs.sound = (int) c.getProperty ("sound", 0);
        cs.accent = (bool) c.getProperty ("accent", true);
        cs.beatsPerBar = (int) c.getProperty ("beats", 4);
        cs.beatUnit = (int) c.getProperty ("unit", 4);
        cs.loopBars = (int) c.getProperty ("loopBars", 0);
        cs.eighths = (bool) c.getProperty ("eighths", false);
        cs.countInBars = (int) c.getProperty ("countIn", 0);
        cs.output = (int) c.getProperty ("output", 0);
        setClickSettings (cs);
    }

    std::vector<Binding> newBindings;
    if (auto* arr = v.getProperty ("bindings", {}).getArray())
        for (auto& e : *arr)
        {
            Binding b;
            b.cc = (int) e.getProperty ("cc", -1);
            b.path = e.getProperty ("path", {}).toString();
            b.lo = (float) (double) e.getProperty ("lo", 0.0);
            b.hi = (float) (double) e.getProperty ("hi", 1.0);
            b.toggle = (bool) e.getProperty ("toggle", false);
            if (b.path.isNotEmpty()) newBindings.push_back (b);
        }

    setBindings (std::move (newBindings));
    std::vector<SwitchBinding> newSwitches;
    if (auto* arr = v.getProperty ("switches", {}).getArray())
        for (auto& e : *arr)
        {
            SwitchBinding s;
            s.cc = (int) e.getProperty ("cc", -1);
            s.role = switchRoleFromId (e.getProperty ("role", "none").toString());
            if (s.cc >= 0) newSwitches.push_back (s);
        }

    setSwitchBindings (std::move (newSwitches));

    loadRig (r, false);
}

juce::AudioProcessorEditor* LowEndProcessor::createEditor()
{
#if LOWEND_HEADLESS
    return nullptr;   // the soak test builds the processor without the web UI
#else
    return new LowEndEditor (*this);
#endif
}

} // namespace lowend

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new lowend::LowEndProcessor();
}
