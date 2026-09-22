// FxParams.h — canonical C++ FX + world-engine parameter registry.
// MIRRORS packages/jmi-format/src/fx-params.ts EXACTLY (same ids, ranges,
// defaults, enum choices) so preset JSON is portable between JM Capture and
// JM Signature. fx-params.ts is the source of truth; change it first.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include <optional>

namespace jm {

enum class FxType : int {
    jmChorus = 0, tremoloAutoPan, phaser, flanger, epPreamp, vintageEq,
    compressor, tapeDelay, digitalDelay, plateReverb, hallShimmer, widener,
    Count
};
constexpr int kNumFxTypes = (int) FxType::Count;

enum class ParamKind { Float, Enum, Bool };

struct ParamSpec {
    const char* id;
    const char* label;
    ParamKind kind = ParamKind::Float;
    float min = 0.0f, max = 1.0f;
    float def = 0.0f;                       // enum: index into choices; bool: 0/1
    std::vector<const char*> choices {};
    const char* unit = "";
    bool syncable = false;
    bool logTaper = false;
};

struct FxSpec {
    FxType type;
    const char* typeId;    // JSON id, e.g. "jmChorus"
    const char* name;
    std::vector<ParamSpec> params;
};

/// The FX rack's OWN division list — the 8 entries whose integer indices are
/// baked into every FX preset ever saved. Never reorder, never insert.
inline const std::vector<const char*>& syncDivisions()
{
    static const std::vector<const char*> d { "1/32", "1/16", "3/32", "1/8", "3/16", "1/4", "1/4d", "1/2" };
    return d;
}

//------------------------------------------------------------------ MODULATION
// PHASE 1: the LFOs need the full 8/1 ... 1/64 table with dotted and triplet
// forms (spec section C), which is a SUPERSET of the FX rack's list above. Because FX
// presets store the division as an INTEGER INDEX, the table is extended
// APPEND-ONLY (docs/MODULATION-RESEARCH.md section 2.4 / trap 16): indices 0-7 keep
// exactly the meaning they had, and everything new lives at 8 and above.
// Index 30 is Vital's "Freeze" (ratio 0) — the LFO holds still.
constexpr int kDivQuarter = 5;      // "1/4", the LFO's spec default
constexpr int kDivFreeze  = 30;

inline const std::vector<const char*>& allDivisionNames()
{
    static const std::vector<const char*> d {
        // 0-7: the original FX list, frozen in place
        "1/32", "1/16", "1/16d", "1/8", "1/8d", "1/4", "1/4d", "1/2",
        // 8+: appended
        "1/64", "1/64d", "1/64t", "1/32d", "1/32t", "1/16t", "1/8t", "1/4t",
        "1/2d", "1/2t", "1/1", "1/1d", "1/1t", "2/1", "2/1d", "2/1t",
        "4/1", "4/1d", "4/1t", "8/1", "8/1d", "8/1t", "FREEZE" };
    return d;
}

/// Division length in quarter-notes. Index 30 (FREEZE) returns 0.
inline double divisionBeats (int idx)
{
    static const double b[] = {
        0.125, 0.25, 0.375, 0.5, 0.75, 1.0, 1.5, 2.0,
        0.0625, 0.09375, 1.0 / 24.0, 0.1875, 1.0 / 12.0, 1.0 / 6.0, 1.0 / 3.0, 2.0 / 3.0,
        3.0, 4.0 / 3.0, 4.0, 6.0, 8.0 / 3.0, 8.0, 12.0, 16.0 / 3.0,
        16.0, 24.0, 32.0 / 3.0, 32.0, 48.0, 64.0 / 3.0, 0.0 };
    constexpr int n = (int) (sizeof (b) / sizeof (b[0]));
    return (idx >= 0 && idx < n) ? b[idx] : 1.0;
}
inline int numDivisions() { return (int) allDivisionNames().size(); }
inline const char* divisionName (int idx)
{
    auto& d = allDivisionNames();
    return (idx >= 0 && idx < (int) d.size()) ? d[(size_t) idx] : d[(size_t) kDivQuarter];
}

/// Musical display order for the LFO's division menu (slowest first). The stored
/// value is always the canonical index above, so this ordering is presentation
/// only and can change freely.
inline const std::vector<int>& lfoDivisionOrder()
{
    static const std::vector<int> o {
        27, 28, 29,   // 8/1  8/1d  8/1t
        24, 25, 26,   // 4/1
        21, 22, 23,   // 2/1
        18, 19, 20,   // 1/1
        7, 16, 17,    // 1/2
        5, 6, 15,     // 1/4
        3, 4, 14,     // 1/8
        1, 2, 13,     // 1/16
        0, 11, 12,    // 1/32
        8, 9, 10,     // 1/64
        30 };         // FREEZE
    return o;
}

inline const std::array<FxSpec, kNumFxTypes>& fxRegistry()
{
    static const std::array<FxSpec, kNumFxTypes> reg { {
        { FxType::jmChorus, "jmChorus", "JM Chorus", {
            { "mode", "Mode", ParamKind::Enum, 0, 2, 0, { "I", "II", "I+II" } },
            { "rate", "Rate", ParamKind::Float, 0.1f, 5.0f, 0.5f, {}, "Hz", false, true },
            { "depth", "Depth", ParamKind::Float, 0, 100, 60 },
            { "width", "Width", ParamKind::Float, 0, 100, 80 },
            { "mix", "Mix", ParamKind::Float, 0, 100, 50 } } },
        { FxType::tremoloAutoPan, "tremoloAutoPan", "Tremolo/Auto-Pan", {
            { "rate", "Rate", ParamKind::Float, 0.1f, 12.0f, 4.5f, {}, "Hz", true, true },
            { "depth", "Depth", ParamKind::Float, 0, 100, 50 },
            { "shape", "Shape", ParamKind::Enum, 0, 2, 0, { "sine", "tri", "square" } },
            { "spread", "Spread", ParamKind::Float, 0, 100, 70 } } },
        { FxType::phaser, "phaser", "Phaser", {
            { "rate", "Rate", ParamKind::Float, 0.05f, 4.0f, 0.3f, {}, "Hz", false, true },
            { "depth", "Depth", ParamKind::Float, 0, 100, 60 },
            { "feedback", "Feedback", ParamKind::Float, 0, 90, 40 },
            { "stages", "Stages", ParamKind::Enum, 0, 2, 1, { "2", "4", "6" } } } },
        { FxType::flanger, "flanger", "Flanger", {
            { "rate", "Rate", ParamKind::Float, 0.05f, 2.0f, 0.2f, {}, "Hz", false, true },
            { "depth", "Depth", ParamKind::Float, 0, 100, 50 },
            { "feedback", "Feedback", ParamKind::Float, -90, 90, 30 },
            { "manual", "Manual", ParamKind::Float, 0.1f, 10.0f, 2.0f, {}, "ms", false, true } } },
        { FxType::epPreamp, "epPreamp", "EP Preamp/Drive", {
            { "drive", "Drive", ParamKind::Float, 0, 100, 20 },
            { "tone", "Tone", ParamKind::Float, -100, 100, 0 },
            { "output", "Output", ParamKind::Float, -24, 12, 0, {}, "dB" } } },
        { FxType::vintageEq, "vintageEq", "Vintage EQ", {
            { "bass", "Bass", ParamKind::Float, -15, 15, 0, {}, "dB" },
            { "midFreq", "Mid Freq", ParamKind::Float, 200, 5000, 1000, {}, "Hz", false, true },
            { "midGain", "Mid Gain", ParamKind::Float, -15, 15, 0, {}, "dB" },
            { "treble", "Treble", ParamKind::Float, -15, 15, 0, {}, "dB" },
            { "lowCut", "Low Cut", ParamKind::Enum, 0, 3, 0, { "off", "40", "80", "120" } } } },
        { FxType::compressor, "compressor", "Compressor", {
            { "amount", "Amount", ParamKind::Float, 0, 100, 30 },
            { "attack", "Attack", ParamKind::Float, 1, 100, 15, {}, "ms", false, true },
            { "release", "Release", ParamKind::Float, 30, 800, 200, {}, "ms", false, true } } },
        { FxType::tapeDelay, "tapeDelay", "Tape Delay", {
            { "time", "Time", ParamKind::Float, 20, 1500, 375, {}, "ms", true, true },
            { "sync", "Sync", ParamKind::Bool, 0, 1, 1 },
            { "syncDivision", "Division", ParamKind::Enum, 0, 7, 4, syncDivisions() },
            { "feedback", "Feedback", ParamKind::Float, 0, 90, 35 },
            { "tone", "Tone", ParamKind::Float, 1000, 12000, 5000, {}, "Hz", false, true },
            { "wowFlutter", "Wow/Flutter", ParamKind::Float, 0, 100, 25 },
            { "pingpong", "Ping-Pong", ParamKind::Bool, 0, 1, 0 },
            { "mix", "Mix", ParamKind::Float, 0, 100, 25 } } },
        { FxType::digitalDelay, "digitalDelay", "Digital Delay", {
            { "timeL", "Time L", ParamKind::Float, 20, 2000, 500, {}, "ms", true, true },
            { "timeR", "Time R", ParamKind::Float, 20, 2000, 750, {}, "ms", true, true },
            { "sync", "Sync", ParamKind::Bool, 0, 1, 1 },
            { "syncDivisionL", "Div L", ParamKind::Enum, 0, 7, 5, syncDivisions() },
            { "syncDivisionR", "Div R", ParamKind::Enum, 0, 7, 6, syncDivisions() },
            { "feedback", "Feedback", ParamKind::Float, 0, 90, 30 },
            { "filterHp", "HP", ParamKind::Float, 20, 500, 20, {}, "Hz", false, true },
            { "filterLp", "LP", ParamKind::Float, 2000, 20000, 20000, {}, "Hz", false, true },
            { "mix", "Mix", ParamKind::Float, 0, 100, 20 } } },
        { FxType::plateReverb, "plateReverb", "Plate Reverb", {
            { "size", "Size", ParamKind::Float, 0, 100, 50 },
            { "decay", "Decay", ParamKind::Float, 0.3f, 8.0f, 2.2f, {}, "s", false, true },
            { "predelay", "Pre-Delay", ParamKind::Float, 0, 200, 20, {}, "ms" },
            { "damp", "Damp", ParamKind::Float, 0, 100, 40 },
            { "mix", "Mix", ParamKind::Float, 0, 100, 25 } } },
        { FxType::hallShimmer, "hallShimmer", "Hall/Shimmer", {
            { "size", "Size", ParamKind::Float, 0, 100, 70 },
            { "decay", "Decay", ParamKind::Float, 0.5f, 20.0f, 4.0f, {}, "s", false, true },
            { "shimmer", "Shimmer", ParamKind::Float, 0, 100, 0 },
            { "mod", "Mod", ParamKind::Float, 0, 100, 30 },
            { "mix", "Mix", ParamKind::Float, 0, 100, 25 } } },
        { FxType::widener, "widener", "Widener/Ensemble", {
            { "width", "Width", ParamKind::Float, 0, 200, 120 },
            { "ensemble", "Ensemble", ParamKind::Float, 0, 100, 0 } } },
    } };
    return reg;
}

inline const FxSpec& fxSpec (FxType t) { return fxRegistry()[(size_t) t]; }

inline std::optional<FxType> fxTypeFromId (const std::string& id)
{
    for (auto& s : fxRegistry())
        if (id == s.typeId) return s.type;
    return std::nullopt;
}

inline const ParamSpec* findParam (const FxSpec& spec, const std::string& id)
{
    for (auto& p : spec.params)
        if (id == p.id) return &p;
    return nullptr;
}

/// Runtime value store for one FX unit (enum = choice index, bool = 0/1).
using ParamValues = std::map<std::string, float>;

inline ParamValues defaultParams (FxType t)
{
    ParamValues out;
    for (auto& p : fxSpec (t).params) out[p.id] = p.def;
    return out;
}

// ------------------------------------------------------- default racks (§4)
/// World index: 0 rhodes-ep, 1 mks, 2 fm, 3 strings (matches jmi::World).
inline const std::vector<FxType>& defaultRack (int world)
{
    static const std::vector<FxType> racks[4] = {
        { FxType::epPreamp, FxType::tremoloAutoPan, FxType::phaser, FxType::tapeDelay, FxType::plateReverb },
        { FxType::jmChorus, FxType::vintageEq, FxType::widener, FxType::hallShimmer },
        { FxType::compressor, FxType::jmChorus, FxType::digitalDelay, FxType::plateReverb },
        { FxType::widener, FxType::vintageEq, FxType::hallShimmer },
    };
    return racks[world < 0 || world > 3 ? 0 : world];
}
/// World-specific overrides on top of registry defaults (fx-params.ts DEFAULT_RACK_OVERRIDES).
inline void applyDefaultRackOverrides (int world, FxType t, ParamValues& v)
{
    if (world == 3 && t == FxType::hallShimmer) v["shimmer"] = 30.0f;
}

// --------------------------------------- world engine controls (§5.2)
inline const std::vector<ParamSpec>& worldEngineControls (int world)
{
    static const std::vector<ParamSpec> ctrls[4] = {
        { { "bark", "Bark", ParamKind::Float, 0, 100, 30 },
          { "tine", "Tine", ParamKind::Float, 0, 100, 50 },
          { "body", "Body", ParamKind::Float, 0, 100, 50 },
          { "tremolo", "Tremolo", ParamKind::Float, 0, 100, 40 },
          { "space", "Space", ParamKind::Float, 0, 100, 30 } },
        { { "chorusMode", "Chorus Mode", ParamKind::Enum, 0, 2, 0, { "I", "II", "I+II" } },
          { "chorusDepth", "Chorus Depth", ParamKind::Float, 0, 100, 60 },
          { "width", "Width", ParamKind::Float, 0, 100, 60 },
          { "ensemble", "Ensemble", ParamKind::Float, 0, 100, 30 },
          { "warmth", "Warmth", ParamKind::Float, 0, 100, 40 },
          { "air", "Air", ParamKind::Float, 0, 100, 30 } },
        { { "brightness", "Brightness", ParamKind::Float, 0, 100, 60 },
          { "attack", "Attack", ParamKind::Float, 0, 100, 20 },
          { "glass", "Glass", ParamKind::Float, 0, 100, 40 },
          { "chorusDepth", "Chorus Depth", ParamKind::Float, 0, 100, 40 },
          { "echo", "Echo", ParamKind::Float, 0, 100, 25 } },
        { { "swell", "Swell", ParamKind::Float, 0, 100, 30 },
          { "ensemble", "Ensemble", ParamKind::Float, 0, 100, 50 },
          { "shimmer", "Shimmer", ParamKind::Float, 0, 100, 20 },
          { "warmth", "Warmth", ParamKind::Float, 0, 100, 40 },
          { "motion", "Motion", ParamKind::Float, 0, 100, 30 } },
    };
    return ctrls[world < 0 || world > 3 ? 0 : world];
}

inline ParamValues defaultEngineParams (int world)
{
    ParamValues out;
    for (auto& p : worldEngineControls (world)) out[p.id] = p.def;
    return out;
}

inline const char* worldName (int w)
{
    switch (w) { case 0: return "RHODES / EP"; case 1: return "MKS"; case 2: return "FM"; case 3: return "STRINGS"; }
    return "PERFORM";
}
inline const char* worldJsonId (int w)
{
    switch (w) { case 0: return "rhodes-ep"; case 1: return "mks"; case 2: return "fm"; default: return "strings"; }
}

} // namespace jm
