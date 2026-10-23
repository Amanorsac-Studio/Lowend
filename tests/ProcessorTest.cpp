// ProcessorTest.cpp — the signal path as a player hears it.
//
//   LowEndProcTest
//
// Two complaints are pinned here. "The effect sounds in parallel with the
// instrument": a pedal switched on must fade in, never pass through a moment
// where the clean signal and the delayed effect cancel each other. And "let me
// hear the raw instrument": Bypass All must give back the input itself — the
// same level, no colouring.
#include "plugin/LowEndProcessor.h"
#include "engine/PedalDsp.h"
#include <cmath>
#include <cstdio>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace lowend;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    if (! ok) { ++failures; std::printf ("  FAIL  %s\n", what.toRawUTF8()); }
    else std::printf ("  ok    %s\n", what.toRawUTF8());
}
static void setEnv (const char* k, const juce::String& v)
{
   #if JUCE_WINDOWS
    _putenv_s (k, v.toRawUTF8());
   #else
    setenv (k, v.toRawUTF8(), 1);
   #endif
}
/// Share of a signal's energy that is the pure tone `f` (1 = nothing else at all).
static double purity (const std::vector<float>& x, double f, double sr)
{
    double s = 0, c = 0, e = 1.0e-20;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double ph = 2.0 * juce::MathConstants<double>::pi * f * (double) i / sr;
        s += x[i] * std::sin (ph); c += x[i] * std::cos (ph); e += (double) x[i] * x[i];
    }
    return (2.0 * (s * s + c * c) / (double) x.size()) / e;
}
static double rms (const std::vector<float>& x) { double e = 0; for (auto v : x) e += (double) v * v; return std::sqrt (e / (double) juce::jmax<size_t> (1, x.size())); }

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("LowEnd signal path\n");
    auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LowEndProcTest-" + juce::Uuid().toDashedString());
    temp.createDirectory();
    setEnv ("LOWEND_DATA_DIR", temp.getFullPathName());

    constexpr double sr = 48000.0;
    constexpr int block = 256;

    //-------------------------------------------------------------- Bypass All
    {
        LowEndProcessor p;
        p.setPlayConfigDetails (2, 2, sr, block);
        p.prepareToPlay (sr, block);
        juce::MidiBuffer midi;
        double phase = 0.0;
        const double f = 220.0;
        auto render = [&] (double seconds)
        {
            std::vector<float> out;
            juce::AudioBuffer<float> b (2, block);
            const int blocks = (int) (seconds * sr / block);
            for (int k = 0; k < blocks; ++k)
            {
                for (int i = 0; i < block; ++i)
                {
                    const float s = 0.25f * (float) std::sin (phase);
                    phase += 2.0 * juce::MathConstants<double>::pi * f / sr;
                    b.setSample (0, i, s); b.setSample (1, i, s);
                }
                p.processBlock (b, midi);
                for (int i = 0; i < block; ++i) out.push_back (b.getSample (0, i));
            }
            return out;
        };
        render (0.5);
        const auto rig = render (0.5);
        p.setBypassAll (true);
        render (0.2);                                   // past the 20 ms fade
        const auto raw = render (0.5);
        const double inRms = 0.25 / std::sqrt (2.0);
        std::printf ("    rig purity %.4f   raw purity %.6f   raw level %.2f dB\n",
                     purity (rig, f, sr), purity (raw, f, sr), 20.0 * std::log10 (rms (raw) / inRms));
        check (purity (raw, f, sr) > 0.9999, "Bypass All gives back the input untouched (a pure tone stays pure)");
        check (std::abs (20.0 * std::log10 (rms (raw) / inRms)) < 0.5, "Bypass All keeps the input level");
        check (purity (rig, f, sr) < purity (raw, f, sr), "with the rig on, the rig is heard (it colours the tone)");
        p.setBypassAll (false);
        render (0.2);
        const auto back = render (0.5);
        check (std::abs (purity (back, f, sr) - purity (rig, f, sr)) < 0.05 || purity (back, f, sr) < 0.9, "switching Bypass All off brings the rig back");
    }

    //------------------------------------------------- a pedal switched on fades in
    // The drive at Mix 0 is a delayed wire (its oversampler's latency). A tone
    // at the frequency where that delay is half a cycle is the worst case: if
    // the crossfade's clean side were not delayed too, the two would cancel
    // half way through the fade.
    {
        BlockChain chain;
        chain.setCapacity (kMaxPreBlocks);
        auto st = defaultBlockState (BlockType::bassOd, true);
        st.params["blend"] = 0.0f;
        st.params["level"] = 50.0f;
        chain.prepare (sr, block);
        chain.setState ({ st });
        const int lat = chain.latencySamples();
        std::printf ("    drive latency %d samples\n", lat);
        check (lat > 0, "the drive reports its oversampling latency");
        if (lat > 0)
        {
            const double f = sr / (2.0 * lat);
            double phase = 0.0;
            juce::AudioBuffer<float> b (2, block);
            auto run = [&] (int blocks, std::vector<float>* keep)
            {
                for (int k = 0; k < blocks; ++k)
                {
                    for (int i = 0; i < block; ++i)
                    {
                        const float s = 0.5f * (float) std::sin (phase);
                        phase += 2.0 * juce::MathConstants<double>::pi * f / sr;
                        b.setSample (0, i, s); b.setSample (1, i, s);
                    }
                    chain.process (b, 120.0);
                    if (keep) for (int i = 0; i < block; ++i) keep->push_back (b.getSample (0, i));
                }
            };
            run (20, nullptr);
            chain.setBypassed (0, false);
            std::vector<float> fade;
            run (8, &fade);                                       // ~43 ms: the whole 20 ms fade
            float worst = 1.0f;
            const int win = juce::jmax (8, (int) std::ceil (sr / f) * 2);
            for (size_t i = 0; i + (size_t) win < fade.size(); i += (size_t) win / 2)
            {
                float pk = 0.0f;
                for (int k = 0; k < win; ++k) pk = juce::jmax (pk, std::abs (fade[i + (size_t) k]));
                worst = juce::jmin (worst, pk / 0.5f);
            }
            std::printf ("    lowest level during the fade: %.2f of full\n", worst);
            check (worst > 0.8f, "switching a pedal on is a fade, not a dip where clean and effect cancel");
        }
    }



    //------------------------------------------------- preset edits are kept and resolved
    {
        auto* p = new LowEndProcessor();
        p->setPlayConfigDetails (2, 2, sr, block);
        p->prepareToPlay (sr, block);
        const auto& lib = p->library();
        const auto a = lib.presets()[0].id, b = lib.presets()[1].id;
        const float gainA = getPath (*lib.findPreset (a), "amp.gain");
        p->loadPreset (a);
        p->setPath ("amp.gain", gainA + 20.0f, false);
        check (p->isDirty(), "an edit marks the preset as changed");
        p->loadPreset (b);
        check (! p->isDirty(), "the next preset loads as saved");
        p->loadPreset (a);
        check (std::abs (getPath (p->rig(), "amp.gain") - (gainA + 20.0f)) < 0.01f, "going back, the edit is still there");
        check (p->pendingEdits().size() == 1, "one edit is pending");
        delete p;                                   // quit with the edit unsaved
        p = new LowEndProcessor();
        p->setPlayConfigDetails (2, 2, sr, block);
        p->prepareToPlay (sr, block);
        check (p->pendingEdits().size() == 1, "the unsaved edit survives a restart");
        p->loadPreset (a);
        check (std::abs (getPath (p->rig(), "amp.gain") - (gainA + 20.0f)) < 0.01f, "and is what the preset plays after the restart");
        // save to a set only: the library preset stays as it was
        const auto& sunday = *std::find_if (p->library().songs().begin(), p->library().songs().end(), [] (const Song& s) { return s.name == "Sunday Set"; });
        const auto sid = sunday.id, scenePreset = sunday.presetIds[1];
        const float gainS = getPath (*p->library().findPreset (scenePreset), "amp.gain");
        p->loadSong (sid, 1);
        p->setPath ("amp.gain", gainS + 15.0f, false);
        check (p->resolveEdit (p->liveEditKey(), LowEndProcessor::Resolve::toSet), "save to this set only");
        check (std::abs (getPath (*p->library().findPreset (scenePreset), "amp.gain") - gainS) < 0.01f, "the preset itself is unchanged");
        check (p->usingOverride() && ! p->isDirty(), "the scene now plays its own version, saved");
        p->loadSong (sid, 0); p->loadSong (sid, 1);
        check (std::abs (getPath (p->rig(), "amp.gain") - (gainS + 15.0f)) < 0.01f, "the set version comes back with the scene");
        // save to the preset: library updated, nothing pending for it
        p->loadPreset (a);
        check (p->resolveEdit (p->liveEditKey(), LowEndProcessor::Resolve::toPreset), "save to the preset");
        check (std::abs (getPath (*p->library().findPreset (a), "amp.gain") - (gainA + 20.0f)) < 0.01f, "the preset on disk has the edit");
        check (p->pendingEdits().empty(), "nothing is left pending");
        delete p;
    }

    //------------------------------------------------- every factory preset, rendered
    // A steady note through each preset, then silence. Three numbers per preset:
    // energy that is not the note or its harmonics (aliasing, buzz, ring-mod
    // artefacts — "a weird sound on top"), the noise floor with no input, and
    // DC. With an output folder argument each render is also written as a WAV.
    {
        const juce::File outDir = argc > 1 ? juce::File (juce::String (argv[1])) : juce::File();
        if (outDir != juce::File()) outDir.createDirectory();
        LowEndProcessor p;
        p.setPlayConfigDetails (2, 2, sr, block);
        p.prepareToPlay (sr, block);
        juce::MidiBuffer midi;
        const double f0 = 55.0;          // A1, a bass note
        constexpr int order = 15, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::printf ("    %-22s %10s %10s %8s\n", "preset", "inharm dB", "floor dBFS", "DC");
        int flagged = 0;
        for (auto& r : p.library().presets())
        {
            p.loadPreset (r.id);
            double phase = 0.0;
            juce::AudioBuffer<float> b (2, block);
            std::vector<float> tone, quiet, all;
            const int total = (int) (4.2 * sr / block), silentFrom = (int) (1.6 * sr / block);
            for (int k = 0; k < total; ++k)
            {
                for (int i = 0; i < block; ++i)
                {
                    float s = 0.0f;
                    if (k < silentFrom)
                        for (int h = 1; h <= 6; ++h) s += (float) (std::sin (h * phase) / h);
                    phase += 2.0 * juce::MathConstants<double>::pi * f0 / sr;
                    b.setSample (0, i, 0.18f * s); b.setSample (1, i, 0.18f * s);
                }
                p.processBlock (b, midi);
                for (int i = 0; i < block; ++i)
                {
                    const float v = b.getSample (0, i);
                    all.push_back (v);
                    const double t = (double) (k * block + i) / sr;
                    if (t > 0.6 && (int) tone.size() < N) tone.push_back (v);
                    if (t > 3.6) quiet.push_back (v);   // two seconds after the note: every release and tail is over
                }
            }
            std::vector<float> w ((size_t) N * 2, 0.0f);
            for (int i = 0; i < N && i < (int) tone.size(); ++i)
                w[(size_t) i] = tone[(size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
            fft.performFrequencyOnlyForwardTransform (w.data());
            double harm = 1e-20, inh = 1e-20, dc = 0, peakE = 0, peakHz = 0;
            for (int i = 4; i < N / 2; ++i)
            {
                const double hz = (double) i * sr / N, e = (double) w[(size_t) i] * w[(size_t) i];
                // The grid is the note two octaves down: octave pedals and the synth's sub add
                // sub-octaves on purpose, and those are part of the note, not an artefact.
                const double g = f0 / 4.0, k = hz / g, near = std::abs (k - std::round (k)) * g;
                const bool isH = near < 4.0 && std::round (k) >= 1;
                (isH ? harm : inh) += e;
                if (! isH && e > peakE) { peakE = e; peakHz = hz; }
            }
            for (auto v : all) dc += v; dc /= (double) all.size();
            const double inhDb = 10.0 * std::log10 (inh / harm);
            const double floorDb = 20.0 * std::log10 (rms (quiet) + 1e-9);
            const bool bad = inhDb > -25.0 || floorDb > -70.0 || std::abs (dc) > 0.01;
            if (bad) ++flagged;
            std::printf ("    %-22s %10.1f %10.1f %8.4f   loudest stray %6.0f Hz (%5.1f dB)%s\n", r.name.toRawUTF8(), inhDb, floorDb, dc, peakHz, 10.0 * std::log10 (peakE / harm), bad ? "   <- CHECK" : "");
            if (outDir != juce::File())
            {
                juce::AudioBuffer<float> wav (1, (int) all.size());
                for (size_t i = 0; i < all.size(); ++i) wav.setSample (0, (int) i, all[i]);
                juce::WavAudioFormat fmt;
                auto os = outDir.getChildFile (juce::File::createLegalFileName (r.name) + ".wav").createOutputStream();
                if (os) { os->setPosition (0); os->truncate();
                    if (auto wr = std::unique_ptr<juce::AudioFormatWriter> (fmt.createWriterFor (os.get(), sr, 1, 24, {}, 0))) { os.release(); wr->writeFromAudioSampleBuffer (wav, 0, wav.getNumSamples()); } }
            }
        }
        check (flagged == 0, "every factory preset is free of buzz, hiss and DC (" + juce::String (flagged) + " flagged)");
    }

    //------------------------------------------------------------------- Cover
    // The song is played by the engine and the player is recorded over it;
    // a take must land on the song's timeline where it was played.
    {
        const juce::String id = "covertest";
        const auto dir = CoverPlayer::learnDir().getChildFile (id);
        dir.createDirectory();
        {
            // 44.1 kHz, as the stems are, so the load has to resample to the engine's 48.
            constexpr double fileRate = 44100.0;
            juce::AudioBuffer<float> mix (2, (int) (2.0 * fileRate));
            for (int i = 0; i < mix.getNumSamples(); ++i)
            {
                const float v = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * i / fileRate);
                mix.setSample (0, i, v); mix.setSample (1, i, v);
            }
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (dir.getChildFile ("mix.wav"));
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (fileRate).withNumChannels (2).withBitsPerSample (24));
            w->writeFromAudioSampleBuffer (mix, 0, mix.getNumSamples());
        }

        LowEndProcessor p;
        p.setPlayConfigDetails (2, 2, sr, block);
        p.prepareToPlay (sr, block);
        p.setBypassAll (true);                       // the raw input, so the tap is easy to read
        auto& cv = p.cover();
        auto pump = [&] (std::function<bool()> done)
        {
            for (int k = 0; k < 200 && ! done(); ++k)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
                cv.poll();
            }
            return done();
        };
        check (cv.load (id, "Cover Test"), "Cover loads a song from its mix");
        const auto loadStart = juce::Time::getMillisecondCounterHiRes();
        check (pump ([&] { return cv.hasSong(); }), "the song is ready");
        const double loadMs = juce::Time::getMillisecondCounterHiRes() - loadStart;
        check (loadMs < 300.0, "a 44.1 kHz song resamples quickly (" + juce::String (loadMs, 0) + " ms for 2 s of audio)");

        juce::MidiBuffer midi;
        juce::int64 clock = 0;                       // samples since play was pressed
        auto run = [&] (double seconds, std::function<float (juce::int64)> in)
        {
            std::vector<float> out;
            juce::AudioBuffer<float> b (2, block);
            for (int k = 0; k < (int) (seconds * sr / block); ++k)
            {
                for (int i = 0; i < block; ++i) { const float v = in (clock + i); b.setSample (0, i, v); b.setSample (1, i, v); }
                p.processBlock (b, midi);
                for (int i = 0; i < block; ++i) out.push_back (b.getSample (0, i));
                clock += block;
            }
            return out;
        };
        auto silent = [] (juce::int64) { return 0.0f; };

        // Playback: the song is heard, at its own level.
        cv.setLevels (0.0f, 0.0f, true);
        cv.play (0.0, 0, 120.0, false);
        clock = 0;                                   // block 0 plays song samples 0..255
        const auto heard = run (1.0, silent);
        std::vector<float> body (heard.begin() + 4800, heard.end());
        check (purity (body, 330.0, sr) > 0.999, "Cover plays the song through the engine");
        check (std::abs (20.0 * std::log10 (rms (body) / (0.25 / std::sqrt (2.0)))) < 0.5, "at the song's own level");
        cv.stop();
        run (0.05, silent);

        // A take: an impulse played at song position 1.0 s lands at 1.0 s,
        // once the device's round trip is accounted for.
        const int deviceLatency = 300;
        p.setDeviceLatency (deviceLatency);
        const juce::int64 hit = (juce::int64) sr;
        cv.play (0.0, 0, 120.0, true);
        clock = 0;
        run (1.6, [&] (juce::int64 t) { return t == hit + deviceLatency ? 0.9f : 0.0f; });
        cv.stop();
        run (0.05, silent);
        check (pump ([&] { auto d = cv.describe(); auto* t = d["takes"].getArray(); return t && t->size() == 1 && (bool) (*t)[0]["saved"]; }), "the take is kept and saved");

        auto readDi = [&] (int number)
        {
            juce::AudioBuffer<float> b;
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (CoverPlayer::takesDir (id).getChildFile ("Take " + juce::String (number) + " DI.wav").createInputStream().release(), true));
            if (r != nullptr) { b.setSize (1, (int) r->lengthInSamples); r->read (&b, 0, (int) r->lengthInSamples, 0, true, false); }
            return b;
        };
        {
            const auto di = readDi (1);
            int at = -1; float peak = 0.0f;
            for (int i = 0; i < di.getNumSamples(); ++i) if (std::abs (di.getSample (0, i)) > peak) { peak = std::abs (di.getSample (0, i)); at = i; }
            check (at == (int) hit, "the take lands where it was played (sample " + juce::String (at) + ", expected " + juce::String (hit) + ")");
        }

        // Punch 1.2-1.4 s over take 1: the rest of take 1 is kept.
        cv.setPunch (1.2, 1.4);
        cv.play (1.0, 0, 120.0, true);
        clock = (juce::int64) sr;
        run (1.0, [] (juce::int64) { return 0.3f; });
        cv.stop();
        run (0.05, silent);
        check (pump ([&] { auto d = cv.describe(); auto* t = d["takes"].getArray(); return t && t->size() == 2 && (bool) (*t)[1]["saved"]; }), "the punched take is kept as take 2");
        {
            const auto di = readDi (2);
            auto at = [&] (double s) { const int i = (int) (s * sr); return i < di.getNumSamples() ? di.getSample (0, i) : 0.0f; };
            check (std::abs (at (1.3) - 0.3f) < 0.01f, "inside the punch, the new playing");
            check (std::abs (at (1.5)) < 1.0e-6f && std::abs (at (1.1)) < 1.0e-6f, "outside it, nothing new is written");
            check (di.getNumSamples() > (int) hit && std::abs (di.getSample (0, (int) hit) - 0.9f) < 0.01f, "and take 1's playing is kept");
        }
        cv.clearPunch();

        // Count-in: four beats at 120 BPM before 1.0 s, so the playhead starts at -1.0 s.
        cv.play (1.0, 4, 120.0, false);
        const auto count = run (0.4, silent);
        check (cv.isCountingIn() && cv.positionSeconds() < 0.0, "a count-in runs before the song");
        float clickPeak = 0.0f;
        for (auto v : count) clickPeak = juce::jmax (clickPeak, std::abs (v));
        check (clickPeak > 0.05f, "the count-in is heard");
        cv.stop();
        run (0.05, silent);

        // A count-in from the middle of the song plays the bar before as pre-roll;
        // only what is played from the start point is recorded.
        cv.selectTake (0);
        cv.play (1.5, 2, 120.0, true);              // pre-roll 0.5-1.5 s
        run (1.5, [] (juce::int64) { return 0.2f; });
        cv.stop();
        run (0.05, silent);
        check (pump ([&] { auto d = cv.describe(); auto* t = d["takes"].getArray(); return t && t->size() == 3 && (bool) (*t)[2]["saved"]; }), "a take recorded after a count-in is kept");
        {
            const auto di = readDi (3);
            auto at = [&] (double s) { const int i = (int) (s * sr); return i < di.getNumSamples() ? di.getSample (0, i) : 0.0f; };
            check (std::abs (at (1.0)) < 1.0e-6f, "the pre-roll is not recorded");
            check (std::abs (at (1.6) - 0.2f) < 0.01f, "the take starts at the start point");
        }

        // Export: the mix, the rig's track and the DI.
        check (cv.exportMix (1), "export starts");
        check (pump ([&] { return ! (bool) cv.describe()["exporting"]; }), "export finishes");
        const auto res = cv.describe()["export"];
        check ((bool) res["ok"] && res["files"].size() == 3, "export writes the mix, the guitar and the DI");
        for (auto& f : *res["files"].getArray())
            check (juce::File (f.toString()).getSize() > 1000, "exported " + juce::File (f.toString()).getFileName());
    }

    temp.deleteRecursively();
    std::printf (failures == 0 ? "OK\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
