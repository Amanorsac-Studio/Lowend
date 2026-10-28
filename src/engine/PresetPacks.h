#pragma once
//==============================================================================
// PRESET PACKS. A pack is one file holding a set of presets with a name, an
// author and a few words about it, so players can make their own collections
// and give them to each other. The app never sends or fetches one: a pack is
// shared the way any file is (email, WhatsApp, a website), and installed by
// dropping it on the app.
//
// A pack comes from someone else, so everything in it is checked before it is
// used (Security Standard: untrusted files). It is plain JSON with nothing but
// sound settings; it is size-capped, preset-capped, every value is clamped to
// its control's range, every chain is cut to the board's capacity, unknown
// pedals are dropped, and every text field is trimmed. Installed presets get
// new ids, so a pack can never overwrite a preset the player already has.
//==============================================================================
#include <cmath>
#include "RigModel.h"

namespace lowend {

/// Packs the player made, ready to send. Shown to them with "Show the file".
inline juce::File packsDir()          { return appDataDir().getChildFile ("Packs"); }
/// What is known about each pack the player installed (its presets live with
/// the others in Presets/).
inline juce::File installedPacksDir() { return appDataDir().getChildFile ("Installed Packs"); }

namespace packs {

constexpr const char* kFormat = "amanorsac-preset-pack";
constexpr int kVersion = 1;
constexpr int kMaxPackBytes = 8 << 20;    ///< a 200-preset pack is well under 1 MB
constexpr int kMaxPresets = 200;

/// The product this build reads and writes packs for ("lowend" / "lowend").
inline juce::String productId() { return "lowend"; }
/// Both products' names, written split so the Low End port's renaming leaves
/// them alone (a Low End build must still say "this pack is for Low End").
inline juce::String productName (const juce::String& id)
{
    if (id == juce::String ("top") + "end") return juce::String ("Top") + " End";
    if (id == juce::String ("low") + "end") return juce::String ("Low") + " End";
    return id;
}
/// .tepack for Low End, .lepack for Low End: a pack is for one instrument.
inline juce::String extension() { return juce::String (".") + productId().substring (0, 1) + "epack"; }

struct Info
{
    juce::String id, name, author, description;
    juce::uint32 colour = 0xff2a8fa8;
    juce::int64 created = 0;
    int count = 0;
};
struct Pack
{
    Info info;
    std::vector<Rig> presets;
    int skippedPedals = 0;        ///< pedals this version does not have, left out
};

/// Text from a file someone else made: no control characters, a sane length.
inline juce::String cleanText (const juce::var& v, int maxLen, const juce::String& fallback = {})
{
    const auto s = v.toString().substring (0, maxLen * 4);
    juce::String out;
    for (auto p = s.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();
        if (c >= 0x20 || c == '\n') out += juce::String::charToString (c);
    }
    out = out.trim().substring (0, maxLen);
    return out.isEmpty() ? fallback : out;
}
inline bool isSafeId (const juce::String& id)
{
    return id.isNotEmpty() && id.length() <= 64 && id.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
}

inline void clampValues (ParamValues& values, const std::vector<ParamSpec>& specs)
{
    for (auto& s : specs)
    {
        auto it = values.find (s.id);
        if (it == values.end()) continue;
        it->second = std::isfinite (it->second) ? juce::jlimit (s.min, s.max, it->second) : s.def;
    }
}
/// Makes a preset from a pack safe to load: every value in range, no chain
/// longer than the board, text trimmed, and nothing personal carried over.
inline void sanitise (Rig& r)
{
    clampValues (r.in, inputParams());   clampValues (r.amp, ampParams());
    clampValues (r.cab, cabParams());    clampValues (r.out, outputParams());
    clampValues (r.synth, synthParams()); clampValues (r.loop, looperParams());
    if (r.pre.size() > (size_t) kMaxPreBlocks)   r.pre.resize ((size_t) kMaxPreBlocks);
    if (r.post.size() > (size_t) kMaxPostBlocks) r.post.resize ((size_t) kMaxPostBlocks);
    for (auto* chain : { &r.pre, &r.post })
        for (auto& b : *chain) clampValues (b.params, blockSpec (b.type).params);
    r.name = cleanText (r.name, 60, "Untitled");
    r.notes = cleanText (r.notes, 1000);
    r.category = cleanText (r.category, 24);
    r.history.clear();
    r.factoryKey = {};
}

//------------------------------------------------------------------ writing
inline juce::var toVar (const Info& info, const std::vector<Rig>& presets)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", kFormat);
    o->setProperty ("version", kVersion);
    o->setProperty ("product", productId());
    o->setProperty ("id", info.id);
    o->setProperty ("name", info.name);
    o->setProperty ("author", info.author);
    o->setProperty ("description", info.description);
    o->setProperty ("colour", (int) info.colour);
    o->setProperty ("created", (double) info.created);
    juce::Array<juce::var> arr;
    for (auto r : presets)
    {
        // What goes in a pack is the sound: not where the player keeps it.
        r.history.clear(); r.packId = {}; r.packName = {}; r.packAuthor = {}; r.factoryKey = {};
        arr.add (rigToVar (r));
    }
    o->setProperty ("presets", arr);
    return juce::var (o);
}

//------------------------------------------------------------------ reading
/// Reads and checks a pack. On failure the message says, in plain words, why.
inline juce::Result fromText (const juce::String& text, Pack& out)
{
    out = {};
    if (text.getNumBytesAsUTF8() > (size_t) kMaxPackBytes) return juce::Result::fail ("This file is too big to be a preset pack.");
    juce::var v;
    if (juce::JSON::parse (text, v).failed() || ! v.isObject()) return juce::Result::fail ("This is not a preset pack (it could not be read).");
    if (v.getProperty ("format", {}).toString() != kFormat) return juce::Result::fail ("This is not a preset pack.");
    if ((int) v.getProperty ("version", 0) > kVersion) return juce::Result::fail ("This pack was made by a newer version. Update the app to install it.");
    const auto product = v.getProperty ("product", {}).toString();
    if (product != productId())
        return juce::Result::fail ("This pack is for " + productName (product.substring (0, 20)) + ", not " + productName (productId()) + ".");
    auto* list = v.getProperty ("presets", {}).getArray();
    if (list == nullptr || list->isEmpty()) return juce::Result::fail ("This pack has no presets in it.");
    if (list->size() > kMaxPresets) return juce::Result::fail ("This pack has more than " + juce::String (kMaxPresets) + " presets.");

    out.info.id = v.getProperty ("id", {}).toString();
    if (! isSafeId (out.info.id)) out.info.id = juce::Uuid().toDashedString();
    out.info.name = cleanText (v.getProperty ("name", {}), 60, "Untitled pack");
    out.info.author = cleanText (v.getProperty ("author", {}), 60, "Unknown");
    out.info.description = cleanText (v.getProperty ("description", {}), 500);
    out.info.colour = (juce::uint32) (int) v.getProperty ("colour", (int) 0xff2a8fa8) | 0xff000000u;
    out.info.created = (juce::int64) (double) v.getProperty ("created", 0.0);
    for (auto& e : *list)
    {
        if (! e.isObject()) continue;
        juce::StringArray missing;
        auto r = rigFromVar (e, &missing);
        out.skippedPedals += missing.size();
        sanitise (r);
        out.presets.push_back (std::move (r));
    }
    if (out.presets.empty()) return juce::Result::fail ("None of the presets in this pack could be read.");
    out.info.count = (int) out.presets.size();
    return juce::Result::ok();
}

//------------------------------------------------------------------ the library
inline juce::var infoToVar (const Info& i)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", i.id); o->setProperty ("name", i.name); o->setProperty ("author", i.author);
    o->setProperty ("description", i.description); o->setProperty ("colour", (int) i.colour);
    o->setProperty ("created", (double) i.created); o->setProperty ("count", i.count);
    return juce::var (o);
}
inline Info infoFromVar (const juce::var& v)
{
    Info i;
    i.id = v.getProperty ("id", {}).toString();
    i.name = cleanText (v.getProperty ("name", {}), 60, "Untitled pack");
    i.author = cleanText (v.getProperty ("author", {}), 60, "Unknown");
    i.description = cleanText (v.getProperty ("description", {}), 500);
    i.colour = (juce::uint32) (int) v.getProperty ("colour", (int) 0xff2a8fa8) | 0xff000000u;
    i.created = (juce::int64) (double) v.getProperty ("created", 0.0);
    i.count = (int) v.getProperty ("count", 0);
    return i;
}
/// The packs installed on this machine, newest first.
inline std::vector<Info> installed()
{
    std::vector<Info> out;
    for (auto& f : installedPacksDir().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (f.getSize() > 64 * 1024) continue;
        auto i = infoFromVar (juce::JSON::parse (f));
        if (isSafeId (i.id)) out.push_back (i);
    }
    std::sort (out.begin(), out.end(), [] (const Info& a, const Info& b) { return a.created > b.created; });
    return out;
}

/// Takes a pack's presets out of the library. Returns how many went.
inline int remove (RigLibrary& lib, const juce::String& packId)
{
    if (! isSafeId (packId)) return 0;
    juce::StringArray ids;
    for (auto& r : lib.presets()) if (r.packId == packId) ids.add (r.id);
    for (auto& id : ids) lib.deletePreset (id);
    installedPacksDir().getChildFile (packId + ".json").deleteFile();
    return ids.size();
}

/// Adds a checked pack's presets as new presets. Installing the same pack again
/// replaces the earlier copy rather than doubling it.
inline int install (RigLibrary& lib, const Pack& p)
{
    remove (lib, p.info.id);
    int order = 5000, added = 0;
    for (auto r : p.presets)
    {
        r.id = {};
        r.packId = p.info.id; r.packName = p.info.name; r.packAuthor = p.info.author;
        r.order = order++;
        r.history.push_back ({ juce::Time::currentTimeMillis(), "Installed from " + p.info.name + " by " + p.info.author });
        lib.savePreset (std::move (r));
        ++added;
    }
    installedPacksDir().createDirectory();
    auto info = p.info; info.count = added;
    if (info.created == 0) info.created = juce::Time::currentTimeMillis();
    installedPacksDir().getChildFile (info.id + ".json").replaceWithText (juce::JSON::toString (infoToVar (info), false));
    return added;
}

/// Writes a pack of the given presets to the Packs folder. Returns the file,
/// or a non-existent file if nothing could be written.
inline juce::File write (const RigLibrary& lib, Info info, const juce::StringArray& presetIds)
{
    std::vector<Rig> presets;
    for (auto& id : presetIds)
        if (const auto* r = lib.findPreset (id))
            if ((int) presets.size() < kMaxPresets) presets.push_back (*r);
    if (presets.empty()) return {};
    if (! isSafeId (info.id)) info.id = juce::Uuid().toDashedString();
    info.name = cleanText (info.name, 60, "My pack");
    info.author = cleanText (info.author, 60, "Unknown");
    info.description = cleanText (info.description, 500);
    info.created = juce::Time::currentTimeMillis();
    info.count = (int) presets.size();
    packsDir().createDirectory();
    auto file = packsDir().getNonexistentChildFile (juce::File::createLegalFileName (info.name), extension(), false);
    if (! file.replaceWithText (juce::JSON::toString (toVar (info, presets), false))) return {};
    return file;
}

} // namespace packs
} // namespace lowend
