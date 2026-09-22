// SoakTest.cpp — the stage-reliability test.
//
// Runs continuous audio through a real LowEndProcessor while, underneath it,
// presets change, blocks are added, removed and reordered, the looper is driven
// through its whole state machine and the synth is switched on and off.
// It asserts what §9 of the build spec promises: no NaN, no denormal storm, no
// unbounded output, no dropout-sized processing spike, and no growth in the
// audio thread's work over time.
//
// The structural edits are deliberately made from the message thread WHILE the
// audio "thread" is between blocks, which is exactly the race the chain's dry
// hand-over and the amp's A/B crossfade exist to survive.
//
// RUN IT ON A QUIET MACHINE. The budget checks are wall-clock, on an ordinary
// (non-realtime-priority) thread, so they measure the machine as much as the
// code: on a box already at 97 % CPU the same build that averages 0.48 ms per
// block averages 1.2 ms and spikes past 50 ms, and the timing assertions fail
// for reasons that have nothing to do with this plugin. The correctness checks
// (NaN, denormals, output ceiling) are load-independent and are the ones to
// trust when the machine is busy; a timing failure on a loaded box means
// "re-run it idle", not "there is a bug".
#include "plugin/LowEndProcessor.h"
#include <juce_events/juce_events.h>
#include <chrono>
#include <cmath>
#include <cstdio>

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

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

/// A plucked bass note that decays and repeats — enough to keep the gate open,
/// the tracker tracking and the compressors working.
struct BassSource
{
    void render (juce::AudioBuffer<float>& b)
    {
        const int n = b.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            if (--samplesToNextNote <= 0)
            {
                samplesToNextNote = (int) (kSr * (0.35 + 0.4 * rng.nextDouble()));
                hz = 41.2f * std::pow (2.0f, (float) rng.nextInt (13) / 12.0f);
                phase = 0.0;
                age = 0.0f;
            }
            age += 1.0f / (float) kSr;
            const float env = std::exp (-age * 2.2f);
            phase += hz / kSr;
            if (phase >= 1.0) phase -= 1.0;
            const float s = env * 0.35f * (float) (std::sin (juce::MathConstants<double>::twoPi * phase)
                          + 0.6 * std::sin (2.0 * juce::MathConstants<double>::twoPi * phase)
                          + 0.25 * std::sin (3.0 * juce::MathConstants<double>::twoPi * phase));
            for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample (ch, i, s);
        }
    }
    juce::Random rng { 20260905 };
    double phase = 0.0;
    float hz = 41.2f, age = 0.0f;
    int samplesToNextNote = 1;
};

} // namespace

int main (int argc, char** argv)
{
    double seconds = 60.0;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--seconds" && i + 1 < argc)
            seconds = juce::String (argv[i + 1]).getDoubleValue();

    juce::ScopedJuceInitialiser_GUI juceInit;

    // Never touch a real library.
    auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("LowEndSoak-" + juce::Uuid().toDashedString());
    temp.createDirectory();
    setDataDirEnv (temp.getFullPathName());

    LowEndProcessor proc;
    proc.setPlayConfigDetails (2, 2, kSr, kBlock);
    proc.prepareToPlay (kSr, kBlock);

    BassSource source;
    juce::AudioBuffer<float> buffer (2, kBlock);
    juce::MidiBuffer midi;
    juce::Random rng { 424242 };

    const auto presets = proc.library().presets();
    const int totalBlocks = (int) (seconds * kSr / kBlock);
    int failures = 0;
    double worstBlockMs = 0.0, worstSteadyMs = 0.0, totalMs = 0.0;
    int worstSteadyBlock = -1, lastAction = -1, worstSteadyAction = -1;
    double firstHalfMs = 0.0, secondHalfMs = 0.0;
    float worstPeak = 0.0f;
    int nanBlocks = 0, denormalBlocks = 0;

    std::printf ("Low End soak: %.0f s of audio, %d blocks at %d samples\n",
                 seconds, totalBlocks, kBlock);

    for (int block = 0; block < totalBlocks; ++block)
    {
        source.render (buffer);
        midi.clear();

        const auto t0 = std::chrono::steady_clock::now();
        proc.processBlock (buffer, midi);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli> (t1 - t0).count();
        worstBlockMs = juce::jmax (worstBlockMs, ms);
        // The first few blocks after prepareToPlay pay for first-touch of pages
        // and the oversamplers' first run. They are reported separately from the
        // steady-state worst case, which is the number that decides whether this
        // survives a gig.
        if (block >= 50 && ms > worstSteadyMs) { worstSteadyMs = ms; worstSteadyBlock = block; worstSteadyAction = lastAction; }
        totalMs += ms;
        (block < totalBlocks / 2 ? firstHalfMs : secondHalfMs) += ms;

        bool sawNan = false;
        int denormals = 0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int s = 0; s < kBlock; ++s)
            {
                const float v = buffer.getSample (ch, s);
                if (! std::isfinite (v)) sawNan = true;
                else
                {
                    worstPeak = juce::jmax (worstPeak, std::abs (v));
                    if (v != 0.0f && std::abs (v) < 1.0e-30f) ++denormals;
                }
            }
        if (sawNan) ++nanBlocks;
        if (denormals > kBlock / 4) ++denormalBlocks;

        // ---- the abuse, roughly four times a second
        if (block % 45 == 0)
        {
            lastAction = rng.nextInt (8);
            switch (lastAction)
            {
                case 0:
                    if (! presets.empty()) proc.loadPreset (presets[(size_t) rng.nextInt ((int) presets.size())].id);
                    break;
                case 1:
                {
                    const auto t = (BlockType) (1 + rng.nextInt (kNumBlockTypes - 1));
                    proc.addBlock (rng.nextBool(), t);
                    break;
                }
                case 2:
                    proc.removeBlock (false, rng.nextInt (kMaxPreBlocks));
                    break;
                case 3:
                    proc.moveBlock (false, rng.nextInt (kMaxPreBlocks), rng.nextInt (kMaxPreBlocks));
                    break;
                case 4:
                    proc.looper().recordPressed();
                    break;
                case 5:
                    if (rng.nextBool()) proc.looper().undoPressed(); else proc.looper().stopPressed();
                    break;
                case 6:
                    proc.setPath ("synth.on", rng.nextBool() ? 1.0f : 0.0f);
                    break;
                default:
                    proc.setPath ("amp.model", (float) rng.nextInt (kNumAmpModels));
                    proc.setPath ("cab.model", (float) rng.nextInt (4));
                    break;
            }
            // Let the deferred half of the chain's structural change actually
            // run — it is a message-thread timer, exactly as it is in the app.
            juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        }
        if (block % 900 == 0 && block > 0)
            std::printf ("  %5.1f s   peak %.3f   worst block %.3f ms\n",
                         block * kBlock / kSr, worstPeak, worstBlockMs);
    }

    proc.panic();
    proc.processBlock (buffer, midi);

    const double avgMs = totalMs / juce::jmax (1, totalBlocks);
    const double budgetMs = kBlock / kSr * 1000.0;
    const double growth = firstHalfMs > 0.0 ? secondHalfMs / firstHalfMs : 1.0;

    std::printf ("\n  average %.3f ms/block (%.1f%% of the %.2f ms budget)\n"
                 "  worst   %.3f ms   steady worst %.3f ms at block %d (action %d)\n"
                 "  peak    %.3f\n  drift   %.2fx (second half vs first)\n",
                 avgMs, 100.0 * avgMs / budgetMs, budgetMs, worstBlockMs, worstSteadyMs, worstSteadyBlock, worstSteadyAction, worstPeak, growth);

    auto fail = [&failures] (const char* what) { ++failures; std::printf ("  FAIL  %s\n", what); };
    if (nanBlocks > 0)        fail ("output contained NaN or infinity");
    if (denormalBlocks > 0)   fail ("output contained a denormal storm");
    if (worstPeak > 1.05f)    fail ("output exceeded the limiter ceiling");
    if (avgMs > budgetMs)     fail ("average block cost exceeds the real-time budget");
    if (growth > 1.5)         fail ("the audio thread got slower over time");
    if (worstSteadyMs > budgetMs) fail ("a steady-state block exceeded the real-time budget");

    temp.deleteRecursively();
    std::printf ("%s\n", failures == 0 ? "\nOK" : "\nFAILED");
    return failures == 0 ? 0 : 1;
}
