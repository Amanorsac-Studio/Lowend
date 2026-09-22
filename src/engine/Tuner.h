// Tuner.h — chromatic tuner, reading the same detector as the synth.
//
// The tuner does not need a fast answer, it needs a STEADY one, so it smooths
// the detector's fractional note with a one-pole whose time constant is long
// (120 ms) and holds the last confident reading for a moment after the note
// decays — a needle that vanishes the instant a string stops ringing is useless
// on a dark stage.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include "PitchTrack.h"

namespace lowend {

class Tuner
{
public:
    void prepare (double sr, int hopSamples)
    {
        // The smoother runs once per detector hop, not per sample.
        const double hopRate = sr > 0 && hopSamples > 0 ? sr / hopSamples : 90.0;
        coef_ = (float) (1.0 - std::exp (-1.0 / (0.12 * hopRate)));
        holdHops_ = (int) (hopRate * 0.6);
        reset();
    }
    void reset()
    {
        smoothed_ = 0.0f; hold_ = 0; hasReading_.store (false);
        note_.store (0); cents_.store (0.0f); hz_.store (0.0f);
    }

    void setReferenceHz (float a4) { refA4_ = juce::jlimit (415.0f, 466.0f, a4); }
    float referenceHz() const { return refA4_; }

    /// Call once per detector hop with the tracker's result.
    void update (const PitchResult& r)
    {
        if (r.voiced && r.confidence > 0.75f)
        {
            // The detector names A4 = 440; a different reference is a constant
            // offset in semitones, applied here so the detector stays untouched.
            const float refOffset = 12.0f * std::log2 (440.0f / refA4_);
            const float n = r.note + refOffset;
            smoothed_ = hasReading_.load() ? smoothed_ + coef_ * (n - smoothed_) : n;
            hasReading_.store (true);
            hold_ = holdHops_;

            const int nearest = (int) std::round (smoothed_);
            note_.store (nearest);
            cents_.store ((smoothed_ - nearest) * 100.0f);
            hz_.store (r.hz);
        }
        else if (hold_ > 0)
        {
            --hold_;
        }
        else
        {
            hasReading_.store (false);
        }
    }

    bool hasReading() const { return hasReading_.load(); }
    int midiNote() const { return note_.load(); }
    float cents() const { return cents_.load(); }
    float hz() const { return hz_.load(); }
    bool inTune() const { return hasReading() && std::abs (cents()) <= 3.0f; }

    /// "E1", "A#2" — sharps, because bass players read the fretboard in sharps.
    juce::String noteName() const
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int n = midiNote();
        if (n <= 0) return {};
        return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
    }
    /// The open string this reading is nearest to, or empty. Standard 4- and
    /// 5-string tuning, plus the high C.
    juce::String nearestString() const
    {
        if (! hasReading()) return {};
        struct S { int note; const char* name; };
        static const S strings[] = { { 23, "B" }, { 28, "E" }, { 33, "A" }, { 38, "D" }, { 43, "G" }, { 48, "C" } };
        const int n = midiNote();
        for (auto& s : strings)
            if (std::abs (s.note - n) <= 1) return s.name;
        return {};
    }

private:
    float coef_ = 0.1f, smoothed_ = 0.0f, refA4_ = 440.0f;
    int hold_ = 0, holdHops_ = 50;
    std::atomic<bool> hasReading_ { false };
    std::atomic<int> note_ { 0 };
    std::atomic<float> cents_ { 0.0f }, hz_ { 0.0f };
};

} // namespace lowend
