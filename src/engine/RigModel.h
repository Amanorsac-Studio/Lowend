// RigModel.h — the model: a Rig (the whole signal chain), the Songs and
// Setlists that order them, the JSON they persist as, and the PATH SPACE that
// every one of them is edited through.
//
// THE PATH SPACE is the single idea this file exists for. One string names any
// settable value in the rig:
//
//     in.trim            amp.gain          cab.model        out.master
//     pre.2.drive        pre.2.bypass      pre.2.type       post.0.mix
//     synth.cutoff       loop.level
//
// The UI writes paths. MIDI learn stores paths. Footswitches toggle paths.
// Presets are maps of path to value. Automation resolves to paths. There is
// exactly one setter, so a control added to the UI is automatable, learnable and
// saveable the moment it exists, with no extra plumbing and no second list to
// keep in sync.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "BlockParams.h"
#include "PedalDsp.h"

namespace lowend {

//==============================================================================
/// `Documents/Amanorsac Studio/Low End`, or the LOWEND_DATA_DIR override that
/// the tests and the CI soak run use so they never touch a real user's library.
inline juce::File appDataDir()
{
    if (auto env = juce::SystemStats::getEnvironmentVariable ("LOWEND_DATA_DIR", {}); env.isNotEmpty())
        return juce::File (env);
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
             .getChildFile ("Amanorsac Studio").getChildFile ("Low End");
}
/// MACHINE STATE — caches, window positions, the web view's profile: things a
/// player would not miss and would not want in Documents. The studio's File &
/// Data Conventions fix this at %LOCALAPPDATA% on Windows (not Roaming, which is
/// where JUCE's own helper points) and ~/Library/Application Support on macOS.
inline juce::File machineStateDir()
{
    if (auto env = juce::SystemStats::getEnvironmentVariable ("LOWEND_STATE_DIR", {}); env.isNotEmpty())
        return juce::File (env);
   #if JUCE_WINDOWS
    auto base = juce::File (juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}));
    if (! base.isDirectory())
        base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #elif JUCE_MAC
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
    return base.getChildFile ("Amanorsac Studio").getChildFile ("Low End");
}
/// Moves something an earlier build kept in the wrong place. Only ever runs when
/// the new location does not exist yet, so it cannot overwrite anything.
inline void migrateOnce (const juce::File& from, const juce::File& to)
{
    if (from == to || ! from.exists() || to.exists()) return;
    to.getParentDirectory().createDirectory();
    if (! from.moveFileTo (to) && from.isDirectory() && from.copyDirectoryTo (to))
        from.deleteRecursively();
}
/// FACTORY CONTENT the installer places and the product only reads: the stem
/// separation model. One fixed location per platform, because the plug-in has to
/// find it without knowing where (or whether) the standalone was installed.
inline juce::File factoryDir()
{
    if (auto env = juce::SystemStats::getEnvironmentVariable ("LOWEND_FACTORY_DIR", {}); env.isNotEmpty())
        return juce::File (env);
   #if JUCE_WINDOWS
    return juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory)
             .getChildFile ("Amanorsac Studio").getChildFile ("Low End");
   #elif JUCE_MAC
    return juce::File ("/Library/Application Support/Amanorsac Studio/Low End");
   #else
    return juce::File ("/usr/share/amanorsac-studio/low-end");
   #endif
}
inline juce::File presetsDir()  { return appDataDir().getChildFile ("Presets"); }
inline juce::File songsDir()    { return appDataDir().getChildFile ("Songs"); }
inline juce::File setlistsDir() { return appDataDir().getChildFile ("Setlists"); }
inline juce::File irDir()       { return appDataDir().getChildFile ("IRs"); }
inline juce::File loopsDir()    { return appDataDir().getChildFile ("Loops"); }
inline juce::File stageDir()    { return appDataDir().getChildFile ("Stage"); }

//==============================================================================
struct Rig
{
    juce::String id;                 ///< uuid; empty for an unsaved rig
    juce::String name { "New Rig" };
    juce::String notes;
    juce::uint32 colour = 0xff2E7DF7;
    /// Sort key for the preset list. Presets are files named by uuid, so without
    /// this the library would come back in whatever order the filesystem hands
    /// them over — which made the factory content appear shuffled. The factory
    /// writes its declaration order here; a preset the user saves gets the
    /// default and sorts after them, by name.
    int order = 1000;

    ParamValues in { defaultsFor (inputParams()) };
    ParamValues amp { defaultsFor (ampParams()) };
    ParamValues cab { defaultsFor (cabParams()) };
    ParamValues out { defaultsFor (outputParams()) };
    ParamValues synth { defaultsFor (synthParams()) };
    ParamValues loop { defaultsFor (looperParams()) };

    ChainState pre;                  ///< pedalboard, in front of the amp
    ChainState post;                 ///< after the cab
};

struct Song
{
    juce::String id, name { "New Song" }, notes;
    double tempo = 120.0;
    juce::uint32 colour = 0xff2E7DF7;
    juce::StringArray presetIds;     ///< the scenes, in order
};

struct Setlist
{
    juce::String id, name { "New Set" };
    juce::StringArray songIds;
};

/// One learned control: a MIDI CC (or a key) driving a path over a range.
struct Binding
{
    int cc = -1;                     ///< MIDI CC number, or -1
    int keyCode = 0;                 ///< computer-key code for the stage view, or 0
    juce::String path;               ///< what it moves
    float lo = 0.0f, hi = 1.0f;      ///< range, in the path's own units
    bool toggle = false;             ///< momentary switch rather than a continuous value
};

/// A footswitch role that is not a plain path (transport, tuner, panic...).
enum class SwitchRole : int
{
    none = 0, presetNext, presetPrev, tapTempo, tuner,
    loopRecord, loopStop, loopUndo, loopClear, panic, Count
};
inline const char* switchRoleId (SwitchRole r)
{
    static const char* ids[] = { "none", "presetNext", "presetPrev", "tapTempo", "tuner",
                                 "loopRecord", "loopStop", "loopUndo", "loopClear", "panic" };
    const int i = (int) r;
    return (i >= 0 && i < (int) SwitchRole::Count) ? ids[i] : ids[0];
}
inline SwitchRole switchRoleFromId (const juce::String& s)
{
    for (int i = 0; i < (int) SwitchRole::Count; ++i)
        if (s == switchRoleId ((SwitchRole) i)) return (SwitchRole) i;
    return SwitchRole::none;
}
struct SwitchBinding { int cc = -1; SwitchRole role = SwitchRole::none; };

//==============================================================================
// PATH SPACE
//==============================================================================
struct PathInfo
{
    bool valid = false;
    ParamKind kind = ParamKind::Float;
    float min = 0, max = 1, def = 0;
    juce::String label, unit;
    std::vector<const char*> choices;
};

namespace detail {
    /// Splits "pre.2.drive" into { "pre", 2, "drive" }; index is -1 when absent.
    inline bool splitPath (const juce::String& path, juce::String& section, int& index, juce::String& param)
    {
        auto parts = juce::StringArray::fromTokens (path, ".", "");
        if (parts.size() == 2) { section = parts[0]; index = -1; param = parts[1]; return true; }
        if (parts.size() == 3) { section = parts[0]; index = parts[1].getIntValue(); param = parts[2]; return true; }
        return false;
    }
    inline ChainState* chainFor (Rig& r, const juce::String& section)
    {
        if (section == "pre") return &r.pre;
        if (section == "post") return &r.post;
        return nullptr;
    }
    inline const ChainState* chainFor (const Rig& r, const juce::String& section)
    {
        if (section == "pre") return &r.pre;
        if (section == "post") return &r.post;
        return nullptr;
    }
    inline ParamValues* valuesFor (Rig& r, const juce::String& section)
    {
        if (section == "in") return &r.in;
        if (section == "amp") return &r.amp;
        if (section == "cab") return &r.cab;
        if (section == "out") return &r.out;
        if (section == "synth") return &r.synth;
        if (section == "loop") return &r.loop;
        return nullptr;
    }
    inline const ParamValues* valuesFor (const Rig& r, const juce::String& section)
    {
        return valuesFor (const_cast<Rig&> (r), section);
    }
}

/// What a path is: range, kind, label. Used by the UI and by MIDI learn to scale
/// a 0..127 CC into the path's own units.
inline PathInfo pathInfo (const Rig& rig, const juce::String& path)
{
    PathInfo info;
    juce::String section, param; int index = -1;
    if (! detail::splitPath (path, section, index, param)) return info;

    if (auto* chain = detail::chainFor (rig, section))
    {
        if (index < 0 || index >= (int) chain->size()) return info;
        const auto& slot = (*chain)[(size_t) index];
        if (param == "bypass") { info = { true, ParamKind::Bool, 0, 1, 0, "Bypass", "", {} }; return info; }
        if (param == "type")   { info = { true, ParamKind::Enum, 0, (float) (kNumBlockTypes - 1), 0, "Type", "", {} }; return info; }
        if (auto* p = findBlockParam (blockSpec (slot.type), param.toStdString()))
        {
            info = { true, p->kind, p->min, p->max, p->def, p->label, p->unit, p->choices };
            return info;
        }
        return info;
    }
    if (auto* list = sectionParams (section.toStdString()))
        if (auto* p = findParamIn (*list, param.toStdString()))
            info = { true, p->kind, p->min, p->max, p->def, p->label, p->unit, p->choices };
    return info;
}

/// The one setter. Returns false for a path that does not resolve — callers log
/// that rather than guessing, so a preset written against an older block layout
/// never silently lands on the wrong control.
inline bool setPath (Rig& rig, const juce::String& path, float value)
{
    juce::String section, param; int index = -1;
    if (! detail::splitPath (path, section, index, param)) return false;

    if (auto* chain = detail::chainFor (rig, section))
    {
        if (index < 0 || index >= (int) chain->size()) return false;
        auto& slot = (*chain)[(size_t) index];
        if (param == "bypass") { slot.bypassed = value >= 0.5f; return true; }
        if (param == "type")
        {
            const auto t = (BlockType) juce::jlimit (0, kNumBlockTypes - 1, (int) std::round (value));
            if (t != slot.type) { slot.type = t; slot.params = defaultBlockParams (t); slot.bypassed = false; }
            return true;
        }
        auto* p = findBlockParam (blockSpec (slot.type), param.toStdString());
        if (! p) return false;
        slot.params[param.toStdString()] = juce::jlimit (p->min, p->max, value);
        return true;
    }

    auto* values = detail::valuesFor (rig, section);
    auto* list = sectionParams (section.toStdString());
    if (! values || ! list) return false;
    auto* p = findParamIn (*list, param.toStdString());
    if (! p) return false;
    (*values)[param.toStdString()] = juce::jlimit (p->min, p->max, value);
    return true;
}

inline float getPath (const Rig& rig, const juce::String& path, bool* ok = nullptr)
{
    if (ok) *ok = false;
    juce::String section, param; int index = -1;
    if (! detail::splitPath (path, section, index, param)) return 0.0f;

    if (auto* chain = detail::chainFor (rig, section))
    {
        if (index < 0 || index >= (int) chain->size()) return 0.0f;
        const auto& slot = (*chain)[(size_t) index];
        if (param == "bypass") { if (ok) *ok = true; return slot.bypassed ? 1.0f : 0.0f; }
        if (param == "type")   { if (ok) *ok = true; return (float) (int) slot.type; }
        auto it = slot.params.find (param.toStdString());
        if (it == slot.params.end()) return 0.0f;
        if (ok) *ok = true;
        return it->second;
    }
    auto* values = detail::valuesFor (rig, section);
    if (! values) return 0.0f;
    auto it = values->find (param.toStdString());
    if (it == values->end()) return 0.0f;
    if (ok) *ok = true;
    return it->second;
}

/// Every path a rig currently exposes, in UI order. Used by MIDI learn's list
/// and by the "what changed" diff when a preset is saved.
inline juce::StringArray allPaths (const Rig& rig)
{
    juce::StringArray out;
    auto addSection = [&out] (const char* name, const std::vector<ParamSpec>& list)
    {
        for (auto& p : list) out.add (juce::String (name) + "." + p.id);
    };
    addSection ("in", inputParams());
    for (size_t i = 0; i < rig.pre.size(); ++i)
    {
        const juce::String base = "pre." + juce::String ((int) i) + ".";
        out.add (base + "bypass");
        for (auto& p : blockSpec (rig.pre[i].type).params) out.add (base + p.id);
    }
    addSection ("amp", ampParams());
    addSection ("cab", cabParams());
    for (size_t i = 0; i < rig.post.size(); ++i)
    {
        const juce::String base = "post." + juce::String ((int) i) + ".";
        out.add (base + "bypass");
        for (auto& p : blockSpec (rig.post[i].type).params) out.add (base + p.id);
    }
    addSection ("synth", synthParams());
    addSection ("loop", looperParams());
    addSection ("out", outputParams());
    return out;
}

//==============================================================================
// JSON
//==============================================================================
namespace detail {
    inline juce::var obj() { return juce::var (new juce::DynamicObject()); }
    inline void set (juce::var& o, const char* k, const juce::var& v) { o.getDynamicObject()->setProperty (k, v); }

    inline juce::var valuesToVar (const ParamValues& v)
    {
        auto o = obj();
        for (auto& [k, val] : v) set (o, k.c_str(), val);
        return o;
    }
    inline void valuesFromVar (const juce::var& v, ParamValues& out)
    {
        if (auto* d = v.getDynamicObject())
            for (auto& prop : d->getProperties())
            {
                const auto key = prop.name.toString().toStdString();
                if (out.find (key) != out.end()) out[key] = (float) (double) prop.value;
            }
    }
    inline juce::var chainToVar (const ChainState& c)
    {
        juce::Array<juce::var> arr;
        for (auto& b : c)
        {
            auto o = obj();
            set (o, "type", juce::String (blockSpec (b.type).id));
            set (o, "bypassed", b.bypassed);
            set (o, "params", valuesToVar (b.params));
            arr.add (o);
        }
        return arr;
    }
    inline ChainState chainFromVar (const juce::var& v, juce::StringArray* unresolved = nullptr)
    {
        ChainState out;
        if (auto* arr = v.getArray())
            for (auto& e : *arr)
            {
                const auto id = e.getProperty ("type", "none").toString();
                const auto t = blockTypeFromId (id.toStdString());
                if (! t.has_value())
                {
                    if (unresolved) unresolved->add (id);
                    continue;
                }
                BlockState b = defaultBlockState (*t, (bool) e.getProperty ("bypassed", false));
                valuesFromVar (e.getProperty ("params", {}), b.params);
                out.push_back (std::move (b));
            }
        return out;
    }
}

inline juce::var rigToVar (const Rig& r)
{
    using detail::set;
    auto o = detail::obj();
    set (o, "schema", 1);
    set (o, "id", r.id);
    set (o, "name", r.name);
    set (o, "notes", r.notes);
    set (o, "colour", (int) r.colour);
    set (o, "order", r.order);
    set (o, "in", detail::valuesToVar (r.in));
    set (o, "amp", detail::valuesToVar (r.amp));
    set (o, "cab", detail::valuesToVar (r.cab));
    set (o, "out", detail::valuesToVar (r.out));
    set (o, "synth", detail::valuesToVar (r.synth));
    set (o, "loop", detail::valuesToVar (r.loop));
    set (o, "pre", detail::chainToVar (r.pre));
    set (o, "post", detail::chainToVar (r.post));
    return o;
}

inline Rig rigFromVar (const juce::var& v, juce::StringArray* unresolved = nullptr)
{
    Rig r;
    if (! v.isObject()) return r;
    r.id = v.getProperty ("id", {}).toString();
    r.name = v.getProperty ("name", "New Rig").toString();
    r.notes = v.getProperty ("notes", {}).toString();
    r.colour = (juce::uint32) (int) v.getProperty ("colour", (int) 0xff2E7DF7);
    r.order = (int) v.getProperty ("order", 1000);
    detail::valuesFromVar (v.getProperty ("in", {}), r.in);
    detail::valuesFromVar (v.getProperty ("amp", {}), r.amp);
    detail::valuesFromVar (v.getProperty ("cab", {}), r.cab);
    detail::valuesFromVar (v.getProperty ("out", {}), r.out);
    detail::valuesFromVar (v.getProperty ("synth", {}), r.synth);
    detail::valuesFromVar (v.getProperty ("loop", {}), r.loop);
    r.pre = detail::chainFromVar (v.getProperty ("pre", {}), unresolved);
    r.post = detail::chainFromVar (v.getProperty ("post", {}), unresolved);
    return r;
}

inline juce::var songToVar (const Song& s)
{
    using detail::set;
    auto o = detail::obj();
    set (o, "schema", 1);
    set (o, "id", s.id);
    set (o, "name", s.name);
    set (o, "notes", s.notes);
    set (o, "tempo", s.tempo);
    set (o, "colour", (int) s.colour);
    juce::Array<juce::var> ids;
    for (auto& p : s.presetIds) ids.add (p);
    set (o, "presets", ids);
    return o;
}
inline Song songFromVar (const juce::var& v)
{
    Song s;
    s.id = v.getProperty ("id", {}).toString();
    s.name = v.getProperty ("name", "New Song").toString();
    s.notes = v.getProperty ("notes", {}).toString();
    s.tempo = (double) v.getProperty ("tempo", 120.0);
    s.colour = (juce::uint32) (int) v.getProperty ("colour", (int) 0xff2E7DF7);
    if (auto* arr = v.getProperty ("presets", {}).getArray())
        for (auto& e : *arr) s.presetIds.add (e.toString());
    return s;
}

inline juce::var setlistToVar (const Setlist& s)
{
    using detail::set;
    auto o = detail::obj();
    set (o, "schema", 1);
    set (o, "id", s.id);
    set (o, "name", s.name);
    juce::Array<juce::var> ids;
    for (auto& g : s.songIds) ids.add (g);
    set (o, "songs", ids);
    return o;
}
inline Setlist setlistFromVar (const juce::var& v)
{
    Setlist s;
    s.id = v.getProperty ("id", {}).toString();
    s.name = v.getProperty ("name", "New Set").toString();
    if (auto* arr = v.getProperty ("songs", {}).getArray())
        for (auto& e : *arr) s.songIds.add (e.toString());
    return s;
}

//==============================================================================
/// Default chains. A new rig is a working bass rig, not an empty board: gate,
/// compressor, amp, 4x10. Everything else is added by the player.
inline ChainState defaultPreChain()
{
    return { defaultBlockState (BlockType::noiseGate),
             defaultBlockState (BlockType::optoComp),
             defaultBlockState (BlockType::bassOd, true) };
}
inline ChainState defaultPostChain()
{
    return { defaultBlockState (BlockType::roomVerb, true) };
}
inline Rig defaultRig (const juce::String& name = "Studio Clean")
{
    Rig r;
    r.name = name;
    r.pre = defaultPreChain();
    r.post = defaultPostChain();
    return r;
}
/// A rig with NOTHING on the board: amp, cab and the fixed input/output stages
/// only. This is what "New preset" makes — a player building a sound wants to
/// start from their bass and the amp, not from someone else's three pedals,
/// and deleting what you did not ask for is the most tedious way to begin.
inline Rig emptyRig (const juce::String& name = "New Rig")
{
    Rig r;
    r.name = name;
    return r;
}

//==============================================================================
/// The on-disk library. Scans, loads and saves; nothing here touches audio.
class RigLibrary
{
public:
    void scan()
    {
        presets_.clear(); songs_.clear(); setlists_.clear(); irs_.clear();
        for (auto& d : { presetsDir(), songsDir(), setlistsDir(), irDir(), loopsDir(), stageDir() })
            d.createDirectory();

        for (auto& f : presetsDir().findChildFiles (juce::File::findFiles, false, "*.rig.json"))
        {
            auto v = juce::JSON::parse (f);
            juce::StringArray missing;
            auto r = rigFromVar (v, &missing);
            if (r.id.isEmpty()) r.id = f.getFileNameWithoutExtension().upToFirstOccurrenceOf (".", false, false);
            if (! missing.isEmpty())
                juce::Logger::writeToLog ("Low End: preset '" + r.name + "' references unknown blocks: " + missing.joinIntoString (", "));
            presets_.push_back (std::move (r));
        }
        std::sort (presets_.begin(), presets_.end(), [] (const Rig& a, const Rig& b)
        {
            if (a.order != b.order) return a.order < b.order;
            return a.name.compareIgnoreCase (b.name) < 0;
        });
        for (auto& f : songsDir().findChildFiles (juce::File::findFiles, false, "*.song.json"))
            songs_.push_back (songFromVar (juce::JSON::parse (f)));
        for (auto& f : setlistsDir().findChildFiles (juce::File::findFiles, false, "*.set.json"))
            setlists_.push_back (setlistFromVar (juce::JSON::parse (f)));
        for (auto& f : irDir().findChildFiles (juce::File::findFiles, false, "*.wav"))
            irs_.add (f.getFullPathName());
        irs_.sort (true);
    }

    const std::vector<Rig>& presets() const { return presets_; }
    const std::vector<Song>& songs() const { return songs_; }
    const std::vector<Setlist>& setlists() const { return setlists_; }
    const juce::StringArray& irs() const { return irs_; }

    const Rig* findPreset (const juce::String& id) const
    {
        for (auto& r : presets_) if (r.id == id) return &r;
        return nullptr;
    }
    const Song* findSong (const juce::String& id) const
    {
        for (auto& s : songs_) if (s.id == id) return &s;
        return nullptr;
    }

    juce::String savePreset (Rig r)
    {
        if (r.id.isEmpty()) r.id = juce::Uuid().toDashedString();
        presetsDir().createDirectory();
        presetsDir().getChildFile (r.id + ".rig.json").replaceWithText (juce::JSON::toString (rigToVar (r), false));
        for (auto& existing : presets_)
            if (existing.id == r.id) { existing = r; return r.id; }
        presets_.push_back (std::move (r));
        return presets_.back().id;
    }
    juce::String saveSong (Song s)
    {
        if (s.id.isEmpty()) s.id = juce::Uuid().toDashedString();
        songsDir().createDirectory();
        songsDir().getChildFile (s.id + ".song.json").replaceWithText (juce::JSON::toString (songToVar (s), false));
        for (auto& existing : songs_)
            if (existing.id == s.id) { existing = s; return s.id; }
        songs_.push_back (std::move (s));
        return songs_.back().id;
    }
    juce::String saveSetlist (Setlist s)
    {
        if (s.id.isEmpty()) s.id = juce::Uuid().toDashedString();
        setlistsDir().createDirectory();
        setlistsDir().getChildFile (s.id + ".set.json").replaceWithText (juce::JSON::toString (setlistToVar (s), false));
        for (auto& existing : setlists_)
            if (existing.id == s.id) { existing = s; return s.id; }
        setlists_.push_back (std::move (s));
        return setlists_.back().id;
    }
    bool deletePreset (const juce::String& id)
    {
        const bool ok = presetsDir().getChildFile (id + ".rig.json").deleteFile();
        presets_.erase (std::remove_if (presets_.begin(), presets_.end(),
                                        [&id] (const Rig& r) { return r.id == id; }), presets_.end());
        // A preset that songs referenced would leave holes in them. Drop the
        // references too, so a song never lists a scene that cannot load.
        for (auto& s : songs_)
            if (s.presetIds.contains (id))
            {
                s.presetIds.removeString (id);
                saveSong (s);
            }
        return ok;
    }
    bool deleteSong (const juce::String& id)
    {
        const bool ok = songsDir().getChildFile (id + ".song.json").deleteFile();
        songs_.erase (std::remove_if (songs_.begin(), songs_.end(),
                                      [&id] (const Song& s) { return s.id == id; }), songs_.end());
        for (auto& set : setlists_)
            if (set.songIds.contains (id))
            {
                set.songIds.removeString (id);
                saveSetlist (set);
            }
        return ok;
    }
    bool deleteSetlist (const juce::String& id)
    {
        const bool ok = setlistsDir().getChildFile (id + ".set.json").deleteFile();
        setlists_.erase (std::remove_if (setlists_.begin(), setlists_.end(),
                                         [&id] (const Setlist& s) { return s.id == id; }), setlists_.end());
        return ok;
    }
    /// Mutable access for the editor's song builder. Callers must `saveSong`.
    Song* findSongMutable (const juce::String& id)
    {
        for (auto& s : songs_) if (s.id == id) return &s;
        return nullptr;
    }

    /// Writes the factory library once, marked by a file so a player who deletes
    /// a factory preset does not get it back on the next launch.
    void generateFactoryIfNeeded()
    {
        auto marker = appDataDir().getChildFile ("factory-v1.txt");
        if (marker.existsAsFile()) return;
        appDataDir().createDirectory();
        buildFactory();
        marker.replaceWithText ("Low End factory content v1\n");
    }

private:
    void buildFactory();

    std::vector<Rig> presets_;
    std::vector<Song> songs_;
    std::vector<Setlist> setlists_;
    juce::StringArray irs_;
};

//==============================================================================
/// The twelve factory rigs. Each is written as a small set of path edits over
/// the default rig, which is also the clearest documentation of how a rig is
/// built — and it exercises `setPath` on every launch, so a broken path shows up
/// immediately rather than in a user's preset a month later.
inline void RigLibrary::buildFactory()
{
    struct Edit { const char* path; float value; };
    struct Factory
    {
        const char* name;
        juce::uint32 colour;
        std::vector<BlockType> pre, post;
        std::vector<Edit> edits;
    };

    const std::vector<Factory> factory {
        { "Studio Clean", 0xff2E7DF7,
          { BlockType::noiseGate, BlockType::optoComp },
          {},
          { { "amp.model", 0 }, { "amp.gain", 30 }, { "amp.bass", 58 }, { "amp.mid", 52 },
            { "amp.treble", 55 }, { "cab.model", 2 }, { "pre.1.sustain", 45 } } },

        { "Fingerstyle Warm", 0xff3FA9A0,
          { BlockType::noiseGate, BlockType::optoComp, BlockType::graphicEq },
          {},
          { { "amp.model", 1 }, { "amp.gain", 35 }, { "amp.bass", 62 }, { "amp.mid", 45 },
            { "amp.deep", 1 }, { "cab.model", 0 }, { "pre.2.g400", -3 }, { "pre.2.g120", 2 } } },

        { "Pick Attack", 0xffE0A22B,
          { BlockType::noiseGate, BlockType::fetComp, BlockType::boost },
          {},
          { { "amp.model", 3 }, { "amp.gain", 45 }, { "amp.treble", 65 }, { "amp.ultraHi", 1 },
            { "cab.model", 2 }, { "pre.1.attack", 2 }, { "pre.2.gain", 4 } } },

        { "Rock Drive", 0xffE2574C,
          { BlockType::noiseGate, BlockType::optoComp, BlockType::bassOd },
          {},
          { { "amp.model", 1 }, { "amp.gain", 62 }, { "amp.sag", 45 }, { "amp.presence", 55 },
            { "cab.model", 3 }, { "pre.2.drive", 55 }, { "pre.2.blend", 65 } } },

        { "Fuzz Monster", 0xff8B5CF6,
          { BlockType::noiseGate, BlockType::fuzz, BlockType::graphicEq },
          {},
          { { "amp.model", 2 }, { "amp.gain", 55 }, { "cab.model", 3 },
            { "pre.1.fuzz", 70 }, { "pre.1.blend", 75 }, { "pre.2.g50", 3 }, { "pre.2.g4k", -4 } } },

        { "Motown Flip", 0xffD08B4F,
          { BlockType::noiseGate, BlockType::optoComp },
          {},
          { { "amp.model", 2 }, { "amp.gain", 42 }, { "amp.bass", 65 }, { "amp.treble", 35 },
            { "amp.sag", 60 }, { "cab.model", 0 }, { "cab.mic", 2 }, { "cab.highCut", 4000 } } },

        { "Slap Contour", 0xff2BC4C0,
          { BlockType::noiseGate, BlockType::fetComp, BlockType::graphicEq },
          {},
          { { "amp.model", 0 }, { "amp.contour", 70 }, { "amp.bass", 68 }, { "amp.treble", 68 },
            { "amp.bright", 1 }, { "cab.model", 2 }, { "pre.2.g400", -6 }, { "pre.2.g4k", 4 } } },

        { "Funk Filter", 0xffB2D235,
          { BlockType::noiseGate, BlockType::optoComp, BlockType::envFilter },
          {},
          { { "amp.model", 0 }, { "cab.model", 1 }, { "pre.2.sens", 65 }, { "pre.2.range", 70 },
            { "pre.2.q", 65 } } },

        { "Sub Bomb", 0xff4C6EF5,
          { BlockType::noiseGate, BlockType::octaver, BlockType::optoComp },
          {},
          { { "amp.model", 3 }, { "amp.ultraLo", 1 }, { "cab.model", 3 },
            { "pre.1.sub1", 70 }, { "pre.1.dry", 90 }, { "pre.1.tone", 35 } } },

        { "Synth Bass", 0xffE056A0,
          { BlockType::noiseGate, BlockType::optoComp },
          {},
          { { "amp.model", 0 }, { "cab.model", 2 },
            { "synth.on", 1 }, { "synth.wave", 0 }, { "synth.cutoff", 700 }, { "synth.reso", 45 },
            { "synth.envAmt", 60 }, { "synth.sub", 55 }, { "synth.level", -6 }, { "synth.route", 1 } } },

        // Replaced "Arp Pulse" when the arpeggiator was removed (September 2026).
        { "Dub Echo", 0xff9B6BF5,
          { BlockType::noiseGate, BlockType::optoComp },
          { BlockType::analogDelay },
          { { "amp.model", 2 }, { "amp.bass", 62 }, { "amp.treble", 38 }, { "cab.model", 0 },
            { "post.0.feedback", 48 }, { "post.0.wowFlutter", 35 }, { "post.0.mix", 28 } } },

        { "Ambient Bed", 0xff6EC1E4,
          { BlockType::noiseGate, BlockType::optoComp },
          { BlockType::bassChorus, BlockType::ambientVerb },
          { { "amp.model", 0 }, { "amp.gain", 25 }, { "cab.model", 0 },
            { "post.0.mix", 30 }, { "post.1.mix", 35 }, { "loop.quantize", 1 } } },
    };

    juce::StringArray madeIds;
    int order = 0;
    for (auto& f : factory)
    {
        Rig r = defaultRig (f.name);
        r.colour = f.colour;
        r.order = order++;          // keep the list in the order it reads here
        r.pre.clear();
        for (auto t : f.pre) r.pre.push_back (defaultBlockState (t));
        r.post.clear();
        for (auto t : f.post) r.post.push_back (defaultBlockState (t));
        for (auto& e : f.edits)
            if (! setPath (r, e.path, e.value))
                juce::Logger::writeToLog (juce::String ("Low End factory: unknown path ") + e.path
                                          + " in preset " + f.name);
        madeIds.add (savePreset (std::move (r)));
    }

    // Three demo songs, each a handful of scenes, and one setlist holding them.
    struct Demo { const char* name; double tempo; std::vector<int> presets; };
    const std::vector<Demo> demos {
        { "Sunday Set", 74.0, { 0, 1, 11 } },
        { "Rock Night", 128.0, { 0, 3, 4 } },
        { "Groove Session", 104.0, { 6, 7, 9, 10 } } };

    juce::StringArray songIds;
    for (auto& d : demos)
    {
        Song s;
        s.name = d.name;
        s.tempo = d.tempo;
        for (int i : d.presets)
            if (i >= 0 && i < madeIds.size()) s.presetIds.add (madeIds[i]);
        songIds.add (saveSong (std::move (s)));
    }
    Setlist set;
    set.name = "Demo Set";
    set.songIds = songIds;
    saveSetlist (std::move (set));
}

} // namespace lowend
