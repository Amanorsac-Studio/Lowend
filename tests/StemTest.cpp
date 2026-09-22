// StemTest.cpp — runs stem separation on a real file, the same way the app does.
//
//   LowEndStems <mix.wav> <out-dir> [--best] [--ref-bass bass.wav]
//
// The mix goes through StemSeparator exactly as the Learn tab sends it: 16-bit
// stereo chunks, base64-encoded, then start(). It reports wall time, the
// real-time factor, and two quality numbers:
//   * reconstruction — the four stems summed against the mix (Demucs stems
//     should add back up to the song; a low figure means something broke);
//   * bass SDR — if a reference bass track is given, the signal-to-distortion
//     ratio of the separated bass against it (the standard separation metric;
//     higher is better, published HT-Demucs bass SDR is around 8–12 dB on
//     real music).
//
// The real-time factor is the number to watch: anything much above 1.0x makes
// the feature unpleasant to use. On a 12th-gen i5 laptop a 30 s clip separates
// in about 25 s, a good part of that being the one-off model load.
#include "learn/StemSeparator.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <cmath>
#include <cstdio>

using namespace lowend;

static bool readStereo (const juce::File& f, juce::AudioBuffer<float>& out, double& sr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) return false;
    sr = r->sampleRate;
    out.setSize (2, (int) r->lengthInSamples);
    r->read (&out, 0, (int) r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1) out.copyFrom (1, 0, out, 0, 0, out.getNumSamples());
    return true;
}

static double sdr (const juce::AudioBuffer<float>& ref, const juce::AudioBuffer<float>& est)
{
    double num = 0, den = 0;
    const int n = juce::jmin (ref.getNumSamples(), est.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const double s = ref.getSample (ch, i), e = est.getSample (ch, i) - s;
            num += s * s; den += e * e;
        }
    return 10.0 * std::log10 ((num + 1e-12) / (den + 1e-12));
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3) { std::printf ("usage: LowEndStems <mix.wav> <out-dir> [--best] [--ref-bass bass.wav]\n"); return 2; }
    const juce::File mixFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    bool best = false;
    juce::File refBass;
    for (int i = 3; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--best") best = true;
        else if (a == "--ref-bass" && i + 1 < argc) refBass = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
    }

    juce::AudioBuffer<float> mix;
    double sr = 0;
    if (! readStereo (mixFile, mix, sr)) { std::printf ("could not read %s\n", argv[1]); return 1; }
    if ((int) sr != StemSeparator::kSampleRate) { std::printf ("needs 44.1 kHz input (got %.0f)\n", sr); return 1; }
    const int frames = mix.getNumSamples();
    std::printf ("mix: %.1f s, %d frames\n", frames / sr, frames);

    StemSeparator sep;
    const auto q = best ? StemSeparator::Quality::best : StemSeparator::Quality::standard;
    std::printf ("cores: %d   model ready: %s\n", juce::SystemStats::getNumCpus(), sep.modelReady() ? "yes" : "no");
    if (! sep.modelReady()) return 1;

    const juce::String id = "stemtest";
    if (! sep.beginUpload (id, frames)) { std::printf ("beginUpload failed\n"); return 1; }
    // Exactly the page's hand-off: 16-bit interleaved stereo, base64, in chunks.
    const int chunk = 1 << 18;
    for (int off = 0; off < frames; off += chunk)
    {
        const int n = juce::jmin (chunk, frames - off);
        juce::MemoryBlock mb ((size_t) n * 4);
        auto* d = static_cast<juce::int16*> (mb.getData());
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
                d[2 * i + ch] = (juce::int16) juce::jlimit (-32768, 32767, (int) std::lround (mix.getSample (ch, off + i) * 32767.0f));
        if (! sep.appendChunk (id, off, juce::Base64::toBase64 (mb.getData(), mb.getSize()))) { std::printf ("appendChunk failed at %d\n", off); return 1; }
    }
    if (! sep.start (id, q)) { std::printf ("start failed: %s\n", sep.status().error.toRawUTF8()); return 1; }

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    int lastPct = -1;
    for (;;)
    {
        juce::Thread::sleep (500);
        const auto st = sep.status();
        const int pct = (int) (st.progress * 100);
        if (pct / 5 != lastPct / 5) { std::printf ("  %3d%%  %s\n", pct, st.stage.toRawUTF8()); lastPct = pct; }
        if (st.state == StemSeparator::State::done) break;
        if (st.state == StemSeparator::State::error || st.state == StemSeparator::State::cancelled)
        {
            std::printf ("FAILED: %s\n", st.error.toRawUTF8());
            return 1;
        }
    }
    const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    std::printf ("separated in %.1f s (%.2fx real time)\n", secs, secs / (frames / sr));

    // Reconstruction: stems summed vs the mix.
    juce::AudioBuffer<float> sum (2, frames), bass;
    sum.clear();
    for (int s = 0; s < StemSeparator::kNumStems; ++s)
    {
        juce::AudioBuffer<float> st;
        double ssr = 0;
        const auto f = StemSeparator::stemDir (id).getChildFile (juce::String (StemSeparator::stemName (s)) + ".wav");
        if (! readStereo (f, st, ssr)) { std::printf ("missing stem %s\n", StemSeparator::stemName (s)); return 1; }
        for (int ch = 0; ch < 2; ++ch) sum.addFrom (ch, 0, st, ch, 0, juce::jmin (frames, st.getNumSamples()));
        if (s == 1) bass = st;
        double e = 0; for (int ch = 0; ch < 2; ++ch) e += st.getRMSLevel (ch, 0, st.getNumSamples());
        std::printf ("  %-6s rms %.4f\n", StemSeparator::stemName (s), e / 2);
    }
    std::printf ("reconstruction (stems summed vs mix): %.1f dB\n", sdr (mix, sum));
    if (refBass.existsAsFile())
    {
        juce::AudioBuffer<float> ref;
        double rsr = 0;
        if (readStereo (refBass, ref, rsr)) std::printf ("bass SDR vs reference: %.1f dB\n", sdr (ref, bass));
    }

    // Copy the stems out for listening, then clean up the test lesson.
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    out.createDirectory();
    for (int s = 0; s < StemSeparator::kNumStems; ++s)
    {
        const auto name = juce::String (StemSeparator::stemName (s)) + ".wav";
        StemSeparator::stemDir (id).getChildFile (name).copyFileTo (out.getChildFile (name));
    }
    StemSeparator::deleteStems (id);
    std::printf ("stems written to %s\n", out.getFullPathName().toRawUTF8());
    return 0;
}
