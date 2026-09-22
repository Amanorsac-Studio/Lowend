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
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
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
    for (auto& f : diLp_) f.reset();
    limiter_.prepare (sr_, maxBlock_);
    tracker_.prepare (sr_);
    synth_.prepare (sr_, maxBlock_);
    looper_.prepare (sr_, maxBlock_);
    tuner_.prepare (sr_, tracker_.hopSamples());

    diBuf_.setSize (2, maxBlock_);
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
    setLatencySamples (chainLatency + limiter_.latencySamples());
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
                return;
            }
        }
    }
    hostTempoValid_.store (false);
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
                case SwitchRole::loopRecord: looper_.recordPressed(); break;
                case SwitchRole::loopStop:   looper_.stopPressed(); break;
                case SwitchRole::loopUndo:   looper_.undoPressed(); break;
                case SwitchRole::loopClear:  looper_.clearPressed(); break;
                case SwitchRole::panic:      panicRequested_.store (true); break;
                // The rest change the MODEL, which only the message thread may
                // touch, so they are handed over rather than done here.
                case SwitchRole::presetNext: pendingSceneStep_.fetch_add (1); break;
                case SwitchRole::presetPrev: pendingSceneStep_.fetch_add (-1); break;
                case SwitchRole::tapTempo:   pendingTap_.store (true); break;
                case SwitchRole::tuner:      tunerActive_.store (! tunerActive_.load()); break;
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
    if (pendingSceneStep_.load() != 0 || pendingTap_.load()) needsAsync = true;
    if (needsAsync) triggerAsyncUpdate();
}

//==============================================================================
/// MESSAGE THREAD. Everything a footswitch or expression pedal wanted to do to
/// the MODEL happens here, one hop after the audio thread saw it.
void LowEndProcessor::handleAsyncUpdate()
{
    if (pendingTap_.exchange (false)) tapTempo();

    if (const int steps = pendingSceneStep_.exchange (0); steps != 0)
        nextScene (steps > 0 ? 1 : -1);

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
    looper_.setTempo (bpm);

    //------------------------------------------------------------ input stage
    const float trim = juce::Decibels::decibelsToGain (inParams_[kTrim]);
    const bool gateOn = inParams_.flag (kGateOn);
    inputGate_.setParams (inParams_[kGateThreshold], inParams_[kGateRelease], 1.0f);

    float inPeak = 0.0f;
    auto* l = buffer.getWritePointer (0);
    auto* r = nCh > 1 ? buffer.getWritePointer (1) : nullptr;
    for (int i = 0; i < n; ++i)
    {
        l[i] *= trim;
        if (r) r[i] *= trim;
        const float mono = r ? 0.5f * (l[i] + r[i]) : l[i];
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

    float outPeak = 0.0f;
    for (int ch = 0; ch < nCh; ++ch)
        outPeak = juce::jmax (outPeak, buffer.getMagnitude (ch, 0, n));
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
    if (auto* r = library_.findPreset (presetId))
    {
        currentPresetId_ = presetId;
        loadRig (*r, true);
        writeSession();
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
