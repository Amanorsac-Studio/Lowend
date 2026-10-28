// LowEndTests.cpp — unit tests for the Low End engine and model.
//
// These run without an audio device and without the UI: they exercise the
// registry, the path space, JSON round-trips, and the DSP claims that would
// otherwise only be checked by ear (the tone stack's interaction, the pitch
// tracker's accuracy and its refusal to octave-halve, and the looper's undo).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <cstdio>
#include <vector>

#include "engine/AmpDsp.h"
#include "engine/BassSynth.h"
#include "engine/Looper.h"
#include "engine/Metronome.h"
#include "engine/PedalDsp.h"
#include "engine/PitchTrack.h"
#include "engine/RigModel.h"
#include "engine/Tuner.h"
#include "learn/Stft.h"
#include "plugin/TesterGate.h"
#include "TesterVectors.h"

using namespace lowend;

/// JUCE has no setter for environment variables, and the library resolves its
/// data folder from LOWEND_DATA_DIR so a test never writes into a real library.
static void setDataDirEnv (const juce::String& path)
{
#if JUCE_WINDOWS
    _putenv_s ("LOWEND_DATA_DIR", path.toRawUTF8());
#else
    setenv ("LOWEND_DATA_DIR", path.toRawUTF8(), 1);
#endif
}

namespace {

int failures = 0, checks = 0;

void check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok) { ++failures; std::printf ("  FAIL  %s\n", what.toRawUTF8()); }
}
void checkClose (double a, double b, double tol, const juce::String& what)
{
    check (std::abs (a - b) <= tol, what + "  (" + juce::String (a, 4) + " vs " + juce::String (b, 4) + ")");
}
void section (const char* name) { std::printf ("%s\n", name); }

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

/// Peak of a sine at `hz` after `fn` has processed it — a crude but sufficient
/// magnitude response probe for the filter-network claims below.
template <typename Fn>
float responseAt (float hz, Fn&& fn)
{
    const int n = (int) (kSr * 0.35);
    juce::AudioBuffer<float> b (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float s = std::sin (juce::MathConstants<float>::twoPi * hz * (float) i / (float) kSr);
        b.setSample (0, i, s);
        b.setSample (1, i, s);
    }
    fn (b);
    // Measure the last third only, so filter start-up transients are excluded.
    return b.getMagnitude (0, n * 2 / 3, n / 3);
}

/// A plucked-string-ish test tone: fundamental plus a stronger second harmonic,
/// which is exactly the case that makes naive autocorrelation report an octave
/// too low on a bass.
void fillPluck (juce::AudioBuffer<float>& b, float hz, float sr)
{
    const int n = b.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / sr;
        const float env = std::exp (-t * 1.2f);
        const float s = env * (0.5f * std::sin (juce::MathConstants<float>::twoPi * hz * t)
                             + 0.9f * std::sin (juce::MathConstants<float>::twoPi * hz * 2.0f * t)
                             + 0.4f * std::sin (juce::MathConstants<float>::twoPi * hz * 3.0f * t));
        for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample (ch, i, s * 0.4f);
    }
}

//==============================================================================
void testRegistry()
{
    section ("Registry");
    for (int i = 1; i < kNumBlockTypes; ++i)
    {
        const auto t = (BlockType) i;
        const auto& s = blockSpec (t);
        check (s.type == t, juce::String ("block ") + s.id + " is at its own enum index");
        check (juce::String (s.id).isNotEmpty(), "block has an id");
        check (! s.params.empty(), juce::String ("block ") + s.id + " has parameters");
        check (s.params.size() <= 24, juce::String ("block ") + s.id + " fits the atomic array");
        for (auto& p : s.params)
            check (p.def >= p.min && p.def <= p.max,
                   juce::String (s.id) + "." + p.id + " default is inside its range");
    }
    // Reused blocks must expose EXACTLY the shared FX unit's parameters, or the
    // adapter's index-for-index forwarding would move the wrong control.
    for (auto& b : blockRegistry())
    {
        if (! b.reuse.has_value()) continue;
        const auto& fx = jm::fxSpec (*b.reuse);
        check (b.params.size() == fx.params.size(), juce::String (b.id) + " mirrors its FX unit's parameter count");
        for (size_t i = 0; i < b.params.size() && i < fx.params.size(); ++i)
            check (juce::String (b.params[i].id) == fx.params[i].id,
                   juce::String (b.id) + " parameter " + juce::String ((int) i) + " matches the FX unit");
    }
    for (auto* list : { &inputParams(), &ampParams(), &cabParams(), &outputParams(),
                        &synthParams(), &looperParams() })
        check (list->size() <= ParamStore::kMaxParams, "section fits the ParamStore");
}

/// "New preset" must hand the player a bare board. The factory presets still
/// come with pedals, so the two starting points are checked against each other
/// — a change that quietly put the default chain back into a new preset would
/// be invisible until someone made one.
void testEmptyRig()
{
    section ("Empty rig");
    const Rig e = emptyRig ("Blank");
    check (e.name == "Blank", "the name is kept");
    check (e.pre.empty(), "a new preset has nothing before the amp");
    check (e.post.empty(), "a new preset has nothing after the cab");

    const Rig d = defaultRig();
    check (! d.pre.empty(), "the default rig still has its chain");

    // The amp and the cab are not blocks and must survive: a rig with no amp
    // would make no sound at all, which is not what "empty" means here.
    Rig w = e;
    check (setPath (w, "amp.gain", 60.0f), "an empty rig still has an amp");
    check (setPath (w, "cab.level", -3.0f), "an empty rig still has a cab");
    checkClose (getPath (w, "amp.gain"), 60.0, 0.001, "and the amp still takes settings");
}

void testPathSpace()
{
    section ("Path space");
    Rig r = defaultRig();

    check (setPath (r, "amp.gain", 72.0f), "amp.gain resolves");
    checkClose (getPath (r, "amp.gain"), 72.0, 0.001, "amp.gain round-trips");
    check (! setPath (r, "amp.nonsense", 1.0f), "an unknown parameter is refused");
    check (! setPath (r, "nope.gain", 1.0f), "an unknown section is refused");
    check (! setPath (r, "pre.99.drive", 1.0f), "an out-of-range slot is refused");

    setPath (r, "amp.gain", 1000.0f);
    checkClose (getPath (r, "amp.gain"), 100.0, 0.001, "values are clamped to the registry range");

    check (setPath (r, "pre.0.bypass", 1.0f), "pre.0.bypass resolves");
    check (r.pre[0].bypassed, "bypass reached the model");

    // Changing a slot's TYPE must reset that slot's parameters, or the new
    // pedal would inherit values that mean something else.
    const auto before = r.pre[1].type;
    check (setPath (r, "pre.1.type", (float) (int) BlockType::fuzz), "pre.1.type resolves");
    check (r.pre[1].type == BlockType::fuzz && before != BlockType::fuzz, "the slot changed type");
    checkClose (getPath (r, "pre.1.fuzz"), blockSpec (BlockType::fuzz).params[0].def, 0.001,
                "the new block starts at its own defaults");

    const auto paths = allPaths (r);
    check (paths.contains ("amp.gain") && paths.contains ("pre.1.fuzz") && paths.contains ("loop.level"),
           "allPaths covers every section");
    for (auto& p : paths)
    {
        bool ok = false;
        getPath (r, p, &ok);
        check (ok, "allPaths entry resolves: " + p);
    }
}

void testJson()
{
    section ("JSON");
    Rig r = defaultRig ("Test Rig");
    r.id = "abc";
    r.notes = "line one";
    setPath (r, "amp.model", 1);
    setPath (r, "amp.bass", 71);
    setPath (r, "pre.2.drive", 66);
    r.post.push_back (defaultBlockState (BlockType::ambientVerb));

    const auto round = rigFromVar (rigToVar (r));
    check (round.name == r.name && round.id == r.id && round.notes == r.notes, "identity survives a round trip");
    checkClose (getPath (round, "amp.bass"), 71.0, 0.001, "amp values survive");
    checkClose (getPath (round, "pre.2.drive"), 66.0, 0.001, "block values survive");
    check (round.pre.size() == r.pre.size() && round.post.size() == r.post.size(), "chain lengths survive");
    check (round.post.back().type == BlockType::ambientVerb, "block types survive");

    // A preset naming a block this build does not have must be reported, not
    // silently dropped into the wrong slot.
    auto v = rigToVar (r);
    if (auto* arr = v.getProperty ("pre", {}).getArray())
        if (! arr->isEmpty())
            (*arr)[0].getDynamicObject()->setProperty ("type", "somePedalFromTheFuture");
    juce::StringArray missing;
    const auto degraded = rigFromVar (v, &missing);
    check (missing.contains ("somePedalFromTheFuture"), "an unknown block is reported");
    check (degraded.pre.size() == r.pre.size() - 1, "an unknown block is dropped, not guessed");

    Song s;
    s.name = "A Song";
    s.tempo = 96.0;
    s.presetIds.add ("one");
    s.presetIds.add ("two");
    const auto sr = songFromVar (songToVar (s));
    check (sr.name == s.name && sr.presetIds == s.presetIds, "songs round-trip");
    checkClose (sr.tempo, 96.0, 0.001, "song tempo round-trips");
}

void testToneStack()
{
    section ("Tone stack");
    // The claim in AmpDsp.h is that the three controls INTERACT: scooping the
    // mid must cut the middle AND lift the ends relative to a flat setting.
    auto measure = [] (float bass, float mid, float treble, float hz)
    {
        ToneStack st;
        st.prepare (kSr);
        st.setControls (bass, mid, treble, 600.0f, false, false);
        return responseAt (hz, [&st] (juce::AudioBuffer<float>& b)
        {
            auto* l = b.getWritePointer (0);
            for (int i = 0; i < b.getNumSamples(); ++i) l[i] = st.process (l[i]);
        });
    };

    const float flatMid = measure (0.5f, 0.5f, 0.5f, 600.0f);
    const float scoopMid = measure (0.5f, 0.0f, 0.5f, 600.0f);
    check (scoopMid < flatMid * 0.5f, "mid at zero cuts the middle by more than 6 dB");

    const float flatLow = measure (0.5f, 0.5f, 0.5f, 60.0f);
    const float scoopLow = measure (0.5f, 0.0f, 0.5f, 60.0f);
    check (scoopLow > flatLow, "scooping the mid lifts the low end (the controls interact)");

    const float bassUp = measure (1.0f, 0.5f, 0.5f, 60.0f);
    const float bassDown = measure (0.0f, 0.5f, 0.5f, 60.0f);
    check (bassUp > bassDown * 3.0f, "the bass control has real range at 60 Hz");

    const float trebUp = measure (0.5f, 0.5f, 1.0f, 4000.0f);
    const float trebDown = measure (0.5f, 0.5f, 0.0f, 4000.0f);
    check (trebUp > trebDown * 3.0f, "the treble control has real range at 4 kHz");
}

void testCab()
{
    section ("Cabinet");
    auto measure = [] (CabModel model, float hz)
    {
        CabSim cab;
        cab.prepare (kSr, kBlock);
        ParamValues v = defaultsFor (cabParams());
        v["model"] = (float) (int) model;
        cab.setParams (v);
        return responseAt (hz, [&cab] (juce::AudioBuffer<float>& b) { cab.process (b); });
    };
    // A speaker in a box is a band-pass. Whatever else the model does, it must
    // not pass 12 kHz or 15 Hz, or it is not a cab.
    for (auto m : { CabModel::c115, CabModel::c410, CabModel::c810 })
    {
        const float mid = measure (m, 200.0f);
        check (measure (m, 12000.0f) < mid * 0.1f, "cab rolls off the top");
        check (measure (m, 15.0f) < mid * 0.5f, "cab rolls off below the box tuning");
    }
    check (measure (CabModel::c810, 60.0f) > measure (CabModel::c210, 60.0f),
           "the 8x10 goes lower than the 2x10");
    check (measure (CabModel::c210, 2500.0f) > measure (CabModel::c810, 2500.0f),
           "the 2x10 has more top than the 8x10");
}

void testPitchTracker()
{
    section ("Pitch tracker");
    struct Case { float hz; const char* name; };
    // Low B through the top of the G string.
    const Case cases[] = { { 30.87f, "low B" }, { 41.20f, "low E" }, { 55.00f, "A" },
                           { 98.00f, "G" }, { 196.00f, "high G" }, { 329.63f, "E4" } };

    for (auto& c : cases)
    {
        PitchTracker t;
        t.prepare (kSr);
        juce::AudioBuffer<float> b (1, (int) (kSr * 1.2));
        fillPluck (b, c.hz, (float) kSr);
        const auto* s = b.getReadPointer (0);

        float best = 0.0f;
        bool sawVoiced = false;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            t.pushSample (s[i]);
            if (t.hopDue())
            {
                const auto& r = t.analyse();
                if (r.voiced) { sawVoiced = true; best = r.hz; }
            }
        }
        check (sawVoiced, juce::String ("tracks ") + c.name);
        if (sawVoiced)
        {
            const double cents = 1200.0 * std::log2 (best / c.hz);
            check (std::abs (cents) < 20.0,
                   juce::String ("within 20 cents on ") + c.name + " (" + juce::String (cents, 1) + " cents)");
            // The octave error is the failure mode that matters: a detector that
            // halves the frequency is worse than one that says nothing.
            check (std::abs (best - c.hz * 0.5f) > c.hz * 0.1f,
                   juce::String ("no octave-down error on ") + c.name);
        }
    }

    // Silence must not produce notes.
    {
        PitchTracker t;
        t.prepare (kSr);
        int events = 0;
        for (int i = 0; i < (int) kSr; ++i)
        {
            t.pushSample (0.0f);
            if (t.hopDue())
            {
                t.analyse();
                NoteEvent ev[2];
                events += t.collectEvents (ev, 2);
            }
        }
        check (events == 0, "silence produces no note events");
    }
}

void testTuner()
{
    section ("Tuner");
    PitchTracker t;
    t.prepare (kSr);
    Tuner tuner;
    tuner.prepare (kSr, t.hopSamples());

    // 41.20 Hz is a low E; 30 cents sharp of it must read as E, +30.
    const float hz = 41.20f * std::pow (2.0f, 30.0f / 1200.0f);
    juce::AudioBuffer<float> b (1, (int) (kSr * 1.5));
    fillPluck (b, hz, (float) kSr);
    const auto* s = b.getReadPointer (0);
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        t.pushSample (s[i]);
        if (t.hopDue()) tuner.update (t.analyse());
    }
    check (tuner.hasReading(), "the tuner gets a reading");
    if (tuner.hasReading())
    {
        check (tuner.noteName() == "E1", "names the note (" + tuner.noteName() + ")");
        check (tuner.nearestString() == "E", "names the string");
        checkClose (tuner.cents(), 30.0, 12.0, "reports the cents error");
        check (! tuner.inTune(), "30 cents sharp is not in tune");
    }
}

void testLooper()
{
    section ("Looper");
    Looper lp;
    lp.prepare (kSr, kBlock);
    ParamValues v = defaultsFor (looperParams());
    v["autoStart"] = 0;     // these takes start on the stomp, not on a note
    v["quantize"] = 0;      // free, so the length is exactly what was recorded
    v["fadeMs"] = 1;
    lp.setParams (v);
    lp.setTempo (120.0);

    auto runBlocks = [&lp] (int blocks, float value)
    {
        juce::AudioBuffer<float> b (2, kBlock);
        for (int i = 0; i < blocks; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < kBlock; ++s) b.setSample (ch, s, value);
            lp.process (b);
        }
    };
    auto captureBlock = [&lp] (float input)
    {
        juce::AudioBuffer<float> b (2, kBlock);
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < kBlock; ++s) b.setSample (ch, s, input);
        lp.process (b);
        return b;
    };

    check (lp.state() == Looper::State::idle, "starts idle");
    lp.recordPressed();
    runBlocks (10, 0.25f);
    check (lp.state() == Looper::State::recording, "records after the first press");
    lp.recordPressed();
    runBlocks (1, 0.0f);
    check (lp.state() == Looper::State::playing, "the second press closes the loop and plays");
    check (lp.lengthSamples() > 0, "the loop has a length");

    // Playing back: with no input, the output must be the recorded material.
    {
        auto b = captureBlock (0.0f);
        check (std::abs (b.getSample (0, kBlock / 2) - 0.25f) < 0.02f, "the loop plays back what was recorded");
    }

    // Overdub adds; undo takes exactly that away again.
    lp.recordPressed();
    check (lp.state() == Looper::State::overdubbing || lp.state() == Looper::State::playing,
           "the third press starts an overdub");
    // Cover the WHOLE loop with the overdub (the take was 10 blocks long), so
    // the next block read back is guaranteed to be material that was overdubbed
    // and undo has a full span to restore.
    runBlocks (14, 0.25f);
    {
        auto b = captureBlock (0.0f);
        check (b.getSample (0, kBlock / 2) > 0.4f, "the overdub is audible");
    }
    lp.undoPressed();
    runBlocks (1, 0.0f);
    {
        // Walk a full loop and confirm every sample is back to the original.
        float worst = 0.0f;
        const int blocks = lp.lengthSamples() / kBlock + 2;
        for (int i = 0; i < blocks; ++i)
        {
            auto b = captureBlock (0.0f);
            for (int s = 0; s < kBlock; ++s)
                worst = juce::jmax (worst, std::abs (b.getSample (0, s)) - 0.25f);
        }
        check (worst < 0.03f, "undo restores the layer exactly");
    }

    // Bar quantisation: at 120 bpm a 4/4 bar is 2 s. A take of about 1.2 s must
    // land on exactly one bar, not on 1.2 s.
    {
        Looper q;
        q.prepare (kSr, kBlock);
        auto qv = defaultsFor (looperParams());
        qv["autoStart"] = 0;
        qv["quantize"] = 1;
        q.setParams (qv);
        q.setTempo (120.0);
        q.setGridActive (true);      // the click is running, so the bar is real
        juce::AudioBuffer<float> b (2, kBlock);
        b.clear();
        q.recordPressed();
        for (int i = 0; i < (int) (kSr * 1.2) / kBlock; ++i) q.process (b);
        q.recordPressed();
        for (int i = 0; i < (int) (kSr * 1.2) / kBlock; ++i) q.process (b);
        checkClose (q.lengthSeconds(), 2.0, 0.05, "a 1.2 s take is closed on the bar line");
    }
}

/// The ways a loop "does not loop well" on stage, each pinned by a test.
void testLooperSeams()
{
    section ("Looper seams and timing");
    auto fill = [] (juce::AudioBuffer<float>& b, float v)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int s = 0; s < b.getNumSamples(); ++s) b.setSample (ch, s, v);
    };

    // 1. Bar quantise with NO audible tempo. The default is Bar, and the tempo
    //    is whatever the app last had (120 here) — a tempo the player cannot
    //    hear. Snapping to it chopped or padded every free loop. With no click
    //    and no host tempo the loop must be exactly what was played.
    {
        Looper lp;
        lp.prepare (kSr, kBlock);
        { auto v = defaultsFor (looperParams()); v["autoStart"] = 0; lp.setParams (v); }      // quantise = Bar
        lp.setTempo (120.0);
        juce::AudioBuffer<float> b (2, kBlock);
        fill (b, 0.25f);
        lp.recordPressed();
        const int blocks = (int) (kSr * 1.3) / kBlock;    // a 1.3 s phrase
        for (int i = 0; i < blocks; ++i) lp.process (b);
        lp.recordPressed();
        for (int i = 0; i < 4; ++i) lp.process (b);
        check (lp.lengthSamples() == blocks * kBlock,
               "with no audible tempo a loop is exactly as long as it was played ("
                   + juce::String (lp.lengthSamples()) + " vs " + juce::String (blocks * kBlock) + ")");
    }

    // 2. The seam. A held note across the loop point must not dip: the old
    //    seam fade pulled the level to zero at every wrap, which on a bass is a
    //    thump once per loop. Record a steady level, keep playing it through
    //    the stomp (as a player does), then listen to the wrap with no input.
    {
        Looper lp;
        lp.prepare (kSr, kBlock);
        auto v = defaultsFor (looperParams());
        v["autoStart"] = 0;
        v["quantize"] = 0;
        v["fadeMs"] = 10;
        lp.setParams (v);
        juce::AudioBuffer<float> b (2, kBlock);
        fill (b, 0.25f);
        lp.recordPressed();
        for (int i = 0; i < 40; ++i) lp.process (b);
        lp.recordPressed();
        for (int i = 0; i < 8; ++i) lp.process (b);        // the note rings on past the stomp
        // Now silence in; walk two whole passes so the wrap is crossed twice.
        float lo = 1.0f;
        const int walk = 2 * lp.lengthSamples() / kBlock + 2;
        for (int i = 0; i < walk; ++i)
        {
            fill (b, 0.0f);
            lp.process (b);
            for (int s = 0; s < kBlock; ++s) lo = juce::jmin (lo, b.getSample (0, s));
        }
        check (lo > 0.22f, "a held note does not dip at the loop point (lowest " + juce::String (lo, 3) + ")");
    }

    // 3. Footswitch timing. A stomp arrives with its sample position inside the
    //    block; applying it at the top of the NEXT block made every loop up to
    //    a buffer too long or short, and the error changed from loop to loop.
    {
        Looper lp;
        lp.prepare (kSr, kBlock);
        auto v = defaultsFor (looperParams());
        v["autoStart"] = 0;
        v["quantize"] = 0;
        lp.setParams (v);
        juce::AudioBuffer<float> b (2, kBlock);
        fill (b, 0.25f);
        lp.recordPressed (37);                             // 37 samples into this block
        lp.process (b);
        for (int i = 0; i < 9; ++i) lp.process (b);
        lp.recordPressed (200);                            // 200 samples into the 11th block
        lp.process (b);
        check (lp.lengthSamples() == 10 * kBlock + 200 - 37,
               "a stomp lands on its own sample (" + juce::String (lp.lengthSamples()) + ")");
    }

    // 4. Half speed overdub. Reading at half speed visits every sample twice,
    //    and the overdub used to write on both visits: double level, and an undo
    //    that saved already-overdubbed audio.
    {
        Looper lp;
        lp.prepare (kSr, kBlock);
        auto v = defaultsFor (looperParams());
        v["autoStart"] = 0;
        v["quantize"] = 0;
        v["speed"] = 1;
        lp.setParams (v);
        juce::AudioBuffer<float> b (2, kBlock);
        fill (b, 0.25f);
        lp.recordPressed();
        for (int i = 0; i < 20; ++i) lp.process (b);
        lp.recordPressed();                                // close -> playing (at half speed)
        lp.process (b);
        lp.recordPressed();                                // overdub one full half-speed pass
        for (int i = 0; i < 2 * 20; ++i) { fill (b, 0.25f); lp.process (b); }
        lp.recordPressed();                                // back to playing
        float hi = 0.0f;
        for (int i = 0; i < 2 * 20 + 4; ++i)
        {
            fill (b, 0.0f);
            lp.process (b);
            for (int s = 0; s < kBlock; ++s) hi = juce::jmax (hi, b.getSample (0, s));
        }
        check (hi < 0.55f, "a half-speed overdub adds once, not twice (peak " + juce::String (hi, 3) + ")");
    }
}

void testMetronome()
{
    section ("Metronome");
    // Onsets: the first sample of each click (the voice is exactly silent
    // between clicks, so "was zero, now is not" is an onset).
    auto onsets = [] (Metronome& m, int blocks, double bpm)
    {
        std::vector<long long> at;
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        float prev = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            m.render (l.data(), r.data(), kBlock, bpm);
            for (int s = 0; s < kBlock; ++s)
            {
                if (prev == 0.0f && l[(size_t) s] != 0.0f) at.push_back ((long long) b * kBlock + s);
                prev = l[(size_t) s];
            }
        }
        return at;
    };

    {
        Metronome m;
        m.prepare (kSr);
        ClickSettings cs; cs.on = true;
        m.setSettings (cs);
        const auto at = onsets (m, (int) (kSr * 2.1) / kBlock, 120.0);
        check (at.size() == 5, "120 bpm gives a click every half second (" + juce::String ((int) at.size()) + " in 2.1 s)");
        bool exact = ! at.empty() && at[0] == 0;
        for (size_t i = 1; i < at.size(); ++i) exact = exact && at[i] - at[i - 1] == 24000;
        check (exact, "the clicks are sample-exact at any buffer size");
    }
    {
        // The one is accented.
        Metronome m;
        m.prepare (kSr);
        ClickSettings cs; cs.on = true;
        m.setSettings (cs);
        std::vector<float> l ((size_t) 24000), r ((size_t) 24000);
        m.render (l.data(), r.data(), 24000, 120.0);
        const float one = juce::FloatVectorOperations::findMaximum (l.data(), 24000);
        m.render (l.data(), r.data(), 24000, 120.0);
        const float two = juce::FloatVectorOperations::findMaximum (l.data(), 24000);
        check (one > two * 1.2f, "beat one is louder than beat two");
    }
    {
        // Count-in with the click OFF: four clicks, then silence, and the
        // looper is told to start on the downbeat after them, to the sample.
        Metronome m;
        m.prepare (kSr);
        m.setSettings (ClickSettings {});
        std::vector<float> l ((size_t) kBlock), r ((size_t) kBlock);
        m.beginCountIn (1, 37, 120.0);
        long long due = -1;
        int clicks = 0;
        float prev = 0.0f;
        for (int b = 0; b < (int) (kSr * 3.0) / kBlock; ++b)
        {
            const int d = m.countInDue (kBlock);
            if (d >= 0 && due < 0) due = (long long) b * kBlock + d;
            m.render (l.data(), r.data(), kBlock, 120.0);
            for (int s = 0; s < kBlock; ++s)
            {
                if (prev == 0.0f && l[(size_t) s] != 0.0f) ++clicks;
                prev = l[(size_t) s];
            }
        }
        check (clicks == 4, "a one-bar count-in plays four clicks and stops (" + juce::String (clicks) + ")");
        check (due == 37 + 4 * 24000, "recording starts exactly on the next downbeat (" + juce::String (due) + ")");
        check (! m.countingIn(), "the count-in ends");
    }
}

void testChainAndBlocks()
{
    section ("Blocks and chain");
    // Every block must be constructible, preparable, and must not produce NaN,
    // silence-from-nowhere or unbounded output on a normal signal.
    for (int i = 1; i < kNumBlockTypes; ++i)
    {
        const auto t = (BlockType) i;
        auto block = createBlock (t);
        check (block != nullptr, juce::String ("createBlock(") + blockSpec (t).id + ")");
        if (! block) continue;
        block->prepare (kSr, kBlock);

        // The buffer is REFILLED every pass. Feeding a block its own output would
        // compound any gain it has (a 6 dB boost run 20 times is +120 dB), which
        // measures the test, not the block.
        juce::AudioBuffer<float> b (2, kBlock);
        for (int pass = 0; pass < 20; ++pass)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < kBlock; ++s)
                {
                    const int i = pass * kBlock + s;
                    b.setSample (ch, s, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 110.0f * i / (float) kSr));
                }
            block->process (b, 120.0);
        }

        bool finite = true;
        float peak = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < kBlock; ++s)
            {
                const float v = b.getSample (ch, s);
                if (! std::isfinite (v)) finite = false;
                peak = juce::jmax (peak, std::abs (v));
            }
        check (finite, juce::String (blockSpec (t).id) + " stays finite");
        check (peak < 20.0f, juce::String (blockSpec (t).id) + " stays bounded");
    }

    // A bypassed chain must be transparent, and the mix smoothing must have
    // settled by the time the caller looks.
    {
        BlockChain chain;
        chain.setCapacity (kMaxPreBlocks);
        ChainState st { defaultBlockState (BlockType::bassOd, true),
                        defaultBlockState (BlockType::fuzz, true) };
        chain.prepare (kSr, kBlock);
        chain.setState (st);

        juce::AudioBuffer<float> b (2, kBlock), ref (2, kBlock);
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < kBlock; ++s)
            {
                const float v = 0.2f * std::sin (juce::MathConstants<float>::twoPi * 82.0f * s / (float) kSr);
                b.setSample (ch, s, v);
                ref.setSample (ch, s, v);
            }
        chain.process (b, 120.0);
        float worst = 0.0f;
        for (int s = 0; s < kBlock; ++s) worst = juce::jmax (worst, std::abs (b.getSample (0, s) - ref.getSample (0, s)));
        check (worst < 1.0e-5f, "a fully bypassed chain is a wire");
    }
}

void testBassOdBlend()
{
    section ("Bass OD blend");
    // The claim in PedalDsp.h: at blend = 0 the pedal is a wire with a level
    // knob, and the low end survives at high drive because the drive path is
    // high-passed.
    auto lowEnergy = [] (float blend, float drive)
    {
        BassOdBlock od;
        od.prepare (kSr, kBlock);
        od.setParam ("blend", blend);
        od.setParam ("drive", drive);
        od.setParam ("level", 50);
        return responseAt (45.0f, [&od] (juce::AudioBuffer<float>& b) { od.process (b, 120.0); });
    };
    const float dry = lowEnergy (0.0f, 80.0f);
    const float wet = lowEnergy (100.0f, 80.0f);
    const float blended = lowEnergy (60.0f, 80.0f);
    check (wet < dry, "the fully driven path loses low end (which is why blend exists)");
    check (blended > wet, "blending the clean path back restores it");
}

void testSynth()
{
    section ("Tracked synth");
    BassSynth s;
    s.prepare (kSr, kBlock);
    ParamValues v = defaultsFor (synthParams());
    v["on"] = 1;
    v["level"] = 0;
    v["attack"] = 1;
    v["glide"] = 0;
    s.setParams (v);

    std::vector<float> out ((size_t) kBlock, 0.0f);
    s.render (out.data(), kBlock);
    float silentPeak = 0.0f;
    for (auto f : out) silentPeak = juce::jmax (silentPeak, std::abs (f));
    check (silentPeak == 0.0f, "the synth is silent until a note arrives");

    s.handle ({ NoteEvent::Kind::on, 45.0f, 1.0f });
    float peak = 0.0f;
    bool finite = true;
    for (int block = 0; block < 20; ++block)
    {
        std::fill (out.begin(), out.end(), 0.0f);
        s.render (out.data(), kBlock);
        for (auto f : out) { peak = juce::jmax (peak, std::abs (f)); if (! std::isfinite (f)) finite = false; }
    }
    check (finite, "the synth stays finite");
    check (peak > 0.01f, "the synth sounds on a note");
    check (peak < 4.0f, "the synth stays bounded");

    s.handle ({ NoteEvent::Kind::off, 45.0f, 0.0f });
    for (int block = 0; block < 200; ++block)
    {
        std::fill (out.begin(), out.end(), 0.0f);
        s.render (out.data(), kBlock);
    }
    check (! s.sounding(), "the envelope reaches idle after note off");
}

void testFactoryContent()
{
    section ("Factory content");
    // Every factory edit must resolve — makeFactoryRig() logs a warning otherwise,
    // and a silently-ignored path is exactly the bug this catches.
    auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("LowEndTest-" + juce::Uuid().toDashedString());
    temp.createDirectory();
    setDataDirEnv (temp.getFullPathName());

    RigLibrary lib;
    lib.generateFactoryIfNeeded();
    lib.scan();
    check (lib.presets().size() == factoryPresets().size() && lib.presets().size() >= 100, "the whole factory library was written ("
           + juce::String ((int) lib.presets().size()) + ")");
    check (lib.songs().size() == 3, "three demo songs");
    check (lib.setlists().size() == 1, "one demo setlist");

    for (auto& r : lib.presets())
    {
        check (r.id.isNotEmpty(), "preset has an id");
        check (! r.pre.empty(), juce::String ("preset ") + r.name + " has a board");
        check (r.pre[0].type == BlockType::noiseGate, juce::String ("preset ") + r.name + " starts with the gate");
    }
    // Synth Bass proves the synth paths save; Dub Echo proves a reused post
    // block's own parameter ids resolve through the path space.
    bool sawSynth = false, sawDub = false;
    for (auto& r : lib.presets())
    {
        if (r.name == "Synth Bass")
        {
            sawSynth = true;
            checkClose (getPath (r, "synth.on"), 1.0, 0.001, "Synth Bass has the synth on");
        }
        if (r.name == "Dub Echo")
        {
            sawDub = true;
            check (r.post.size() == 1 && r.post[0].type == BlockType::analogDelay, "Dub Echo has the delay after the cab");
            checkClose (getPath (r, "post.0.feedback"), 48.0, 0.001, "Dub Echo's delay feedback saved");
        }
    }
    check (sawSynth && sawDub, "Synth Bass and Dub Echo are in the factory set");
    for (auto& r : lib.presets())
        check (r.name != "Arp Pulse", "no factory preset still references the removed arpeggiator");

    temp.deleteRecursively();
}

//==============================================================================
/// The spectrogram branch of HT-Demucs only works if our STFT is the one the
/// weights were trained against, so two things are checked here: the geometry
/// (a 7.8 s segment must become exactly 4 x 2048 x 336 for the network), and
/// that the transform is genuinely invertible. The second matters because the
/// separated stems are rebuilt through the INVERSE transform — any scaling
/// mistake there would quietly attenuate or brighten every stem, and nothing
/// else in the pipeline would notice.
void testStft()
{
    std::printf ("\nSTFT (stem separation front end)\n");
    using namespace lowend::stems;

    // Demucs' segment geometry, mirrored from Separator.cpp.
    const int segment = (int) (7.8 * kSampleRate);              // 343980
    const int le      = (int) std::ceil ((double) segment / kHop);
    const int pad     = (kHop / 2) * 3;
    const int padEnd  = pad + le * kHop - segment;
    const int padded  = segment + pad + padEnd;

    check (segment == 343980, "segment is 343980 samples");
    check (le == 336, "336 frames reach the network");
    check (frameCount (padded) - 2 * kEdgeFrames == le, "trimming the edge frames leaves exactly le");
    check (kModelBins == 2048, "2048 bins reach the network");

    Stft stft (padded);
    std::vector<std::complex<float>> spec ((size_t) stft.frames() * kBins);
    std::vector<float> in ((size_t) padded), out ((size_t) padded);

    // Something with content everywhere: a bass note, a high partial, and noise.
    juce::Random rng (7);
    for (int i = 0; i < padded; ++i)
    {
        const double t = i / (double) kSampleRate;
        in[(size_t) i] = (float) (0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 41.2 * t)
                               + 0.1 * std::sin (2.0 * juce::MathConstants<double>::pi * 3150.0 * t)
                               + 0.05 * (rng.nextFloat() * 2.0f - 1.0f));
    }

    stft.forward (in.data(), padded, spec.data());
    stft.inverse (spec.data(), padded, out.data());

    // The first and last window overlap incompletely, so the edges are not
    // reconstructed and Demucs discards them (that is what pad/padEnd are for).
    // Everything the model actually returns to us is interior.
    double sig = 0.0, err = 0.0;
    for (int i = kFftSize; i < padded - kFftSize; ++i)
    {
        const double a = in[(size_t) i], d = out[(size_t) i] - a;
        sig += a * a; err += d * d;
    }
    const double dB = 10.0 * std::log10 ((sig + 1e-20) / (err + 1e-20));
    std::printf ("  round trip: %.1f dB\n", dB);
    check (dB > 90.0, "STFT round-trips to better than 90 dB");

    // A sine at a bin centre must land in that bin with the right magnitude:
    // this is the check that would catch a wrong forward scaling, which would
    // feed the network spectrograms in the wrong units.
    const int bin = 100;
    const double freq = (double) bin * kSampleRate / kFftSize;
    for (int i = 0; i < padded; ++i)
        in[(size_t) i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / kSampleRate);
    stft.forward (in.data(), padded, spec.data());
    double peak = 0.0; int peakBin = 0;
    for (int b = 0; b < kBins; ++b)
    {
        const double m = std::abs (spec[(size_t) 8 * kBins + (size_t) b]);
        if (m > peak) { peak = m; peakBin = b; }
    }
    double winSum = 0.0;
    for (auto w : hann()) winSum += w;
    const double expect = winSum / 2.0 / std::sqrt ((double) kFftSize);
    std::printf ("  peak bin %d, magnitude %.4f (expected %.4f)\n", peakBin, peak, expect);
    check (peakBin == bin, "a sine lands in its own bin");
    checkClose (peak, expect, 0.02 * expect, "forward scaling matches torch.stft(normalized=True)");
}

/// Build Standard B50: the product must survive its own data being missing,
/// truncated or corrupt - fall back, never crash. A preset file cut off halfway
/// through a save (power loss, a full disk) is the realistic case.
void testCorruptData()
{
    section ("Corrupt data");
    auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("LowEndTest-" + juce::Uuid().toDashedString());
    temp.createDirectory();
    setDataDirEnv (temp.getFullPathName());

    RigLibrary lib;
    lib.generateFactoryIfNeeded();
    lib.scan();
    const auto good = (int) lib.presets().size();

    presetsDir().getChildFile ("truncated.rig.json").replaceWithText ("{ \"name\": \"Half a pres");
    { const char junk[] = "\x00\xff\x13\x37 not json at all"; presetsDir().getChildFile ("garbage.rig.json").replaceWithData (junk, sizeof (junk) - 1); }
    presetsDir().getChildFile ("empty.rig.json").replaceWithText ("");
    presetsDir().getChildFile ("wrongshape.rig.json").replaceWithText ("[1, 2, 3]");
    songsDir().getChildFile ("broken.song.json").replaceWithText ("{{{{");

    lib.scan();      // must not crash
    check ((int) lib.presets().size() >= good, "the good presets are all still there");
    for (auto& r : lib.presets())
    {
        Rig w = r;   // every entry, including the damaged ones, is a usable rig
        check (setPath (w, "amp.gain", 50.0f), "a damaged preset still yields a working rig: " + r.name);
    }
    temp.deleteRecursively();
}

//==============================================================================
/// The test-build key (TesterGate.h). Tokens here were signed by NODE's crypto
/// with a throwaway key (scripts/make-tester-test-vectors.js), so this checks the
/// C++ verifier against an independent implementation of the same standard.
/// What matters is less that a good key opens the gate than that nothing else
/// does: a wrong key, an altered one, one for another product, an expired one,
/// or a clock wound back.
void testTesterGate()
{
    section ("Test-build key");
    using namespace lowend::tester;
    using namespace testervectors;
    const juce::String mod (kModulus), otherMod (kOtherModulus);
    const auto now = kIssuedMs + 3 * kDayMs;      // three days after issue

    // ---- a good key
    auto c = evaluate (kGood, now, 0, mod);
    check (c.status == Status::ok, "a good key is accepted");
    check (c.daysLeft == 27, "and reports 27 days left three days into 30 (" + juce::String (c.daysLeft) + ")");
    check (c.expiresMs == kGoodExpiresMs, "and reports its expiry");

    // ---- how it may be written down
    juce::String wrapped;
    for (int i = 0; i < juce::String (kGood).length(); ++i)
    {
        wrapped += juce::String (kGood).substring (i, i + 1);
        if (i % 60 == 59) wrapped += "\r\n  ";      // an email client wrapping the line
    }
    check (evaluate (wrapped, now, 0, mod).status == Status::ok, "line breaks and spaces from an email are ignored");
    check (evaluate ("  " + juce::String (kGood) + "\n", now, 0, mod).status == Status::ok, "so is padding");

    // ---- days left, at the edges
    check (evaluate (kGood, kGoodExpiresMs - 1, 0, mod).daysLeft == 1, "one millisecond before expiry still counts as a day");
    check (evaluate (kGood, kGoodExpiresMs, 0, mod).status == Status::expired, "at the expiry instant it is expired");
    check (evaluate (kGood, kIssuedMs, 0, mod).daysLeft == 30, "on the day of issue, 30 days");

    // ---- what must NOT open it
    check (evaluate ("", now, 0, mod).status == Status::none, "an empty box is 'nothing entered'");
    for (auto* junk : { "abc", "a.b", ".", "..", "abc.", ".abc", "a.b.c", "!!!.???", "hello world" })
        check (evaluate (junk, now, 0, mod).status == Status::malformed, juce::String ("not a key: ") + junk);
    check (evaluate (juce::String::repeatedString ("A", 900), now, 0, mod).status == Status::malformed, "a long run of nonsense is not a key");

    const juce::String good (kGood);
    const int dot = good.indexOfChar ('.');
    auto flip = [] (juce::String s, int at)
    {
        const auto c = s[at];
        return s.replaceSection (at, 1, juce::String::charToString (c == 'A' ? 'B' : 'A'));
    };
    check (evaluate (flip (good, 5), now, 0, mod).status == Status::badSignature, "changing one character of the body breaks it");
    check (evaluate (flip (good, dot + 40), now, 0, mod).status == Status::badSignature, "changing one character of the signature breaks it");
    check (evaluate (good.substring (0, good.length() - 4), now, 0, mod).status == Status::malformed, "a key cut short is refused");
    check (evaluate (good.substring (dot + 1) + "." + good.substring (0, dot), now, 0, mod).status != Status::ok, "body and signature swapped is refused");
    check (evaluate (kGood, now, 0, otherMod).status == Status::badSignature, "a key checked against the wrong public key is refused");
    check (evaluate (kSignedByOther, now, 0, mod).status == Status::badSignature, "a well-formed key signed by anyone else is refused");

    // ---- signed by the studio, but not a usable key
    check (evaluate (kWrongProduct, now, 0, mod).status == Status::wrongProduct, "a genuine signature for another product is refused");
    check (evaluate (kNoDates, now, 0, mod).status == Status::malformed, "a key with no dates is refused");
    check (evaluate (kBackwardsDates, now, 0, mod).status == Status::malformed, "a key that expires before it begins is refused");

    // ---- time
    c = evaluate (kExpired, now, 0, mod);
    check (c.status == Status::expired && c.expiresMs == kExpiredExpiresMs, "an expired key says so, and when");
    check (evaluate (kFromFuture, now, 0, mod).status == Status::clockBehind, "a key issued in the far future means the clock is wrong");
    check (evaluate (kLong, now, 0, mod).status == Status::ok, "a far-future expiry is fine");
    // The clock is wound back after the key has expired: the remembered time wins.
    check (evaluate (kGood, kGoodExpiresMs - kDayMs, kGoodExpiresMs + kDayMs, mod).status == Status::expired,
           "winding the clock back does not revive an expired key");

    // ---- the stored key and the remembered time
    auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("LowEndTester-" + juce::Uuid().toDashedString());
    juce::int64 fake = now;
    auto clock = [&fake] { return fake; };
    {
        Gate g (temp, clock, mod);
        check (g.current().status == Status::none, "a new machine has no key");
        check (g.submit ("nonsense").status == Status::malformed, "a bad key is refused");
        check (! temp.getChildFile ("tester-key.txt").existsAsFile(), "and is not stored");
        check (g.submit (kExpired).status == Status::expired, "an expired key is refused");
        check (g.current().status == Status::none, "and does not unlock anything");
        check (g.submit (kGood).status == Status::ok && g.current().status == Status::ok, "a good key unlocks");
    }
    {
        Gate g (temp, clock, mod);
        check (g.current().status == Status::ok, "and is still unlocked after a restart");
        check (g.submit ("nonsense").status == Status::malformed && g.current().status == Status::ok,
               "a bad key typed later does not lock a working one out");
        fake = kGoodExpiresMs + kDayMs;
        check (g.refresh().status == Status::expired, "it locks when the key expires while the app is open");
    }
    {
        fake = kGoodExpiresMs - 5 * kDayMs;                        // someone winds the clock back to before expiry
        Gate g (temp, clock, mod);
        check (g.current().status == Status::expired, "restarting with the clock wound back is still locked");
        check (g.refresh().status == Status::expired, "and stays locked");
    }
    temp.getChildFile ("tester-seen.txt").replaceWithText ("garbage that is not a number");
    {
        fake = now;
        Gate g (temp, clock, mod);                                 // must not crash on a damaged file
        check (g.current().status == Status::ok, "a damaged remembered-time file is treated as no history");
    }
    temp.getChildFile ("tester-key.txt").replaceWithText ("");
    {
        Gate g (temp, clock, mod);
        check (g.current().status == Status::none, "an empty stored key is treated as no key");
    }
    temp.deleteRecursively();
}

} // namespace

//==============================================================================
/// The looper against the click: a late stomp, an early stomp, the take that
/// starts on the first note, a pickup, and STOP during a take.
void testLooperTiming()
{
    section ("Looper timing");
    const double bar = kSr * 2.0;            // 120 BPM, 4/4
    const int blk = 48;
    auto make = [] (Looper& lp, bool autoStart)
    {
        lp.prepare (kSr, 48);
        ParamValues v = defaultsFor (looperParams());
        v["quantize"] = 1; v["fadeMs"] = 1; v["autoStart"] = autoStart ? 1.0f : 0.0f;
        lp.setParams (v);
        lp.setTempo (120.0);
        lp.setGridActive (true);
    };
    auto feed = [blk] (Looper& lp, double samples, float value)
    {
        juce::AudioBuffer<float> b (2, blk);
        for (int done = 0; done < (int) samples; done += blk)
        {
            for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < blk; ++s) b.setSample (ch, s, value);
            lp.process (b);
        }
    };
    auto hear = [blk] (Looper& lp)
    {
        juce::AudioBuffer<float> b (2, blk);
        b.clear();
        lp.process (b);
        return std::abs (b.getSample (0, blk - 1));
    };
    const double tol = 2.0 * blk;

    {   // stomped a beat after bar 4: four bars, and the loop stays in phase
        Looper lp; make (lp, false);
        lp.recordPressed(); feed (lp, 4.25 * bar, 0.3f);
        lp.recordPressed(); feed (lp, blk, 0.3f);
        check (lp.state() == Looper::State::playing, "a late stomp closes the loop at once");
        check (std::abs (lp.lengthSamples() - 4.0 * bar) <= tol, "the loop is four bars (rounded down)");
        check (std::abs (lp.position() * lp.lengthSamples() - (0.25 * bar + blk)) <= tol,
               "playback resumes a beat in, so the loop's 1 stays on the click's 1");
    }
    {   // stomped just before bar 4: waits for the bar line, then plays from the top
        Looper lp; make (lp, false);
        lp.recordPressed(); feed (lp, 3.9 * bar, 0.3f);
        lp.recordPressed(); feed (lp, blk, 0.3f);
        check (lp.state() == Looper::State::waitingForBar, "an early stomp waits for the bar line");
        feed (lp, 0.1 * bar + blk, 0.3f);
        check (lp.state() == Looper::State::playing, "and plays once the bar is reached");
        check (std::abs (lp.lengthSamples() - 4.0 * bar) <= tol, "four bars");
        check (lp.position() * lp.lengthSamples() <= tol + blk, "from the top of the loop");
    }
    {   // auto-start: the player comes in a little after the downbeat
        Looper lp; make (lp, true);
        lp.recordPressed(); feed (lp, blk, 0.0f);
        check (lp.state() == Looper::State::armed, "REC arms the looper until the first note");
        feed (lp, 0.3 * bar, 0.0f);
        feed (lp, 3.95 * bar, 0.3f);
        check (lp.state() == Looper::State::recording, "the first note starts the take");
        lp.recordPressed(); feed (lp, blk, 0.3f);
        check (std::abs (lp.lengthSamples() - 4.0 * bar) <= tol, "the take began on the downbeat before the note: four bars");
    }
    {   // auto-start: a whole bar of nothing, then a note in bar 2
        Looper lp; make (lp, true);
        lp.recordPressed();
        feed (lp, 1.2 * bar, 0.0f);
        feed (lp, 3.05 * bar, 0.3f);
        lp.recordPressed(); feed (lp, blk, 0.3f);
        check (std::abs (lp.lengthSamples() - 3.0 * bar) <= tol, "the take began on bar 2's downbeat: three bars, the silent bar dropped");
        // the loop's own first fifth of a bar is the silence before the note
        feed (lp, 2.75 * bar - 2 * blk, 0.0f);
        float early = 0.0f, later = 0.0f;
        for (int i = 0; i < (int) (0.1 * bar / blk); ++i) early = hear (lp);
        for (int i = 0; i < (int) (0.3 * bar / blk); ++i) later = hear (lp);
        check (early < 0.05f && later > 0.2f, "the loop starts with that bar's silence, then the note");
    }
    {   // a pickup just before bar 2 belongs to bar 2
        Looper lp; make (lp, true);
        lp.recordPressed();
        feed (lp, 0.97 * bar, 0.0f);
        feed (lp, 3.28 * bar, 0.3f);
        lp.recordPressed(); feed (lp, blk, 0.3f);
        check (std::abs (lp.lengthSamples() - 3.0 * bar) <= tol, "a pickup before the bar line starts the take at the bar line");
    }
    {   // STOP during the take closes it on the bar, then stops
        Looper lp; make (lp, false);
        lp.recordPressed(); feed (lp, 4.3 * bar, 0.3f);
        lp.stopPressed(); feed (lp, blk, 0.3f);
        check (lp.state() == Looper::State::stopped && std::abs (lp.lengthSamples() - 4.0 * bar) <= tol,
               "STOP after the bar line keeps four bars and stops");
        Looper lq; make (lq, false);
        lq.recordPressed(); feed (lq, 3.9 * bar, 0.3f);
        lq.stopPressed(); feed (lq, 0.1 * bar + blk, 0.3f);
        check (lq.state() == Looper::State::stopped && std::abs (lq.lengthSamples() - 4.0 * bar) <= tol,
               "STOP before the bar line finishes the bar, then stops");
        lq.recordPressed(); feed (lq, blk, 0.0f);
        check (lq.state() == Looper::State::playing, "REC on a stopped loop plays it");
    }
}

/// The looper on the click's grid: the click's bar decides where a take
/// starts and where PLAY after STOP comes back in. Fixed loop lengths and the
/// time signature (6/8 counts eighths) are checked against the metronome.
void testLooperOnClick()
{
    section ("Looper on the click");
    const int blk = 48;
    const double bpm = 120.0;

    // A metronome and a looper driven the way the processor drives them.
    struct Rig2
    {
        Metronome m; Looper lp;
        juce::AudioBuffer<float> b { 2, 48 };
        std::vector<float> l = std::vector<float> (48), r = std::vector<float> (48);
        void prep (bool autoStart, int beats = 4, int unit = 4)
        {
            m.prepare (kSr); lp.prepare (kSr, 48);
            ClickSettings cs; cs.on = true; cs.beatsPerBar = beats; cs.beatUnit = unit; m.setSettings (cs);
            ParamValues v = defaultsFor (looperParams());
            v["quantize"] = 1; v["fadeMs"] = 1; v["autoStart"] = autoStart ? 1.0f : 0.0f;
            lp.setParams (v); lp.setTempo (120.0); lp.setGridActive (true);
        }
        void run (double samples, float value)
        {
            for (int done = 0; done < (int) samples; done += 48)
            {
                lp.setClickGrid (m.barPhase (120.0), m.barSamples (120.0), m.pulseSamples (120.0));
                for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < 48; ++s) b.setSample (ch, s, value);
                lp.process (b);
                m.render (l.data(), r.data(), 48, 120.0);
            }
        }
    };
    const double bar = kSr * 2.0;
    const double tol = 2.0 * blk;

    {   // REC pressed in the middle of bar 1: the take starts on bar 2
        Rig2 x; x.prep (false);
        x.run (0.5 * bar, 0.3f);
        x.lp.recordPressed();
        x.run (blk, 0.3f);
        check (x.lp.state() == Looper::State::armed, "REC mid-bar waits for the click's next bar");
        x.run (0.5 * bar, 0.3f);
        check (x.lp.state() == Looper::State::recording, "and records from that bar");
    }
    {   // a fixed length of 4 bars closes the loop by itself, exactly
        Rig2 x; x.prep (false);
        x.lp.setFixedBars (4);
        x.lp.recordPressed();
        x.run (4.5 * bar, 0.3f);
        check (x.lp.state() == Looper::State::playing, "a 4-bar loop closes itself and plays");
        check (std::abs (x.lp.lengthSamples() - 4.0 * bar) <= 1.0, "and is exactly four bars (" + juce::String (x.lp.lengthSamples()) + ")");
    }
    {   // PLAY after STOP comes back in on the click's "1", from the top
        Rig2 x; x.prep (false);
        x.lp.setFixedBars (2);
        x.lp.recordPressed();
        x.run (2.2 * bar, 0.3f);
        x.lp.stopPressed(); x.run (blk, 0.0f);
        x.run (0.4 * bar, 0.0f);                      // a pause that ends mid-bar
        x.lp.recordPressed(); x.run (blk, 0.0f);
        check (x.lp.state() == Looper::State::cued, "PLAY mid-bar waits for the click's next 1");
        double waited = 0.0;
        while (x.lp.state() == Looper::State::cued && waited < 2.0 * bar) { x.run (blk, 0.0f); waited += blk; }
        const double phase = x.m.barPhase (120.0);
        check (x.lp.state() == Looper::State::playing && (phase < tol || phase > bar - tol),
               "and starts exactly on it (click " + juce::String (phase, 0) + " samples into its bar)");
        check (x.lp.positionSamples() <= tol, "from the top of the loop");
    }
    {   // 6/8: a bar is six eighths, three quarter notes long; 4/4 is four
        Metronome m; m.prepare (kSr);
        ClickSettings cs; cs.on = true; cs.beatsPerBar = 6; cs.beatUnit = 8; m.setSettings (cs);
        checkClose (m.barSamples (bpm) / kSr, 1.5, 1.0e-6, "6/8 at 120 is 1.5 s a bar");
        cs.beatsPerBar = 4; cs.beatUnit = 4; m.setSettings (cs);
        checkClose (m.barSamples (bpm) / kSr, 2.0, 1.0e-6, "4/4 at 120 is 2 s a bar");
    }
    {   // the "1" is pitched an octave above the other beats
        Metronome m; m.prepare (kSr);
        ClickSettings cs; cs.on = true; cs.beatsPerBar = 4; m.setSettings (cs);
        auto pitchOf = [&m] (int beatsToSkip)
        {
            std::vector<float> l (48), r (48), all;
            for (int k = 0; k < (int) (kSr * 2.0 / 48) + 4; ++k) { m.render (l.data(), r.data(), 48, 120.0); all.insert (all.end(), l.begin(), l.end()); }
            const size_t at = (size_t) (beatsToSkip * kSr * 0.5) + 200;
            int crossings = 0;
            for (size_t i = at; i < at + 480; ++i) if ((all[i - 1] < 0.0f) != (all[i] < 0.0f)) ++crossings;
            return crossings * kSr / 480.0 / 2.0;
        };
        const double one = pitchOf (0);
        m.prepare (kSr); m.setSettings (cs);
        const double two = pitchOf (1);
        check (one > 1.8 * two && one < 2.2 * two, "the 1 sounds an octave above beat 2 (" + juce::String (one, 0) + " vs " + juce::String (two, 0) + " Hz)");
    }
}

int main (int argc, char** argv)
{
    juce::ignoreUnused (argc, argv);
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("Low End tests\n=============\n");
    testRegistry();
    testEmptyRig();
    testPathSpace();
    testJson();
    testToneStack();
    testCab();
    testPitchTracker();
    testTuner();
    testLooper();
    testLooperTiming();
    testLooperOnClick();
    testLooperSeams();
    testMetronome();
    testChainAndBlocks();
    testBassOdBlend();
    testSynth();
    testFactoryContent();
    testCorruptData();
    testTesterGate();
    testStft();

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
