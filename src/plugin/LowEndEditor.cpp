// LowEndEditor.cpp — see header.
#include "LowEndEditor.h"
#include "LowEndUi.h"

namespace lowend {

namespace {
    /// The test-build gate as the page sees it. Status names are the page's
    /// vocabulary for choosing wording; nothing here is shown to a person as is.
    juce::var testerVar (bool gated, const tester::Check& c)
    {
        static const char* names[] = { "none", "malformed", "badSignature", "wrongProduct", "clockBehind", "expired", "ok" };
        auto* o = new juce::DynamicObject();
        o->setProperty ("gated", gated);
        o->setProperty ("status", names[(int) c.status]);
        o->setProperty ("ok", ! gated || c.status == tester::Status::ok);
        o->setProperty ("expires", (double) c.expiresMs);
        o->setProperty ("daysLeft", c.daysLeft);
        return juce::var (o);
    }
    juce::var obj() { return juce::var (new juce::DynamicObject()); }
    void set (juce::var& o, const char* k, const juce::var& v) { o.getDynamicObject()->setProperty (k, v); }
    juce::String hex (juce::uint32 argb)
    {
        return "#" + juce::String::toHexString ((int) (argb & 0xffffff)).paddedLeft ('0', 6);
    }
    const char* stateName (Looper::State s)
    {
        switch (s)
        {
            case Looper::State::recording:     return "recording";
            case Looper::State::waitingForBar: return "closing";
            case Looper::State::playing:       return "playing";
            case Looper::State::overdubbing:   return "overdub";
            case Looper::State::stopped:       return "stopped";
            default:                           return "idle";
        }
    }
    /// A parameter as the UI needs it: value plus everything needed to draw the
    /// control, so the page never hard-codes a range.
    juce::var paramVar (const juce::String& path, const ParamSpec& p, float value)
    {
        auto o = obj();
        set (o, "path", path);
        set (o, "id", juce::String (p.id));
        set (o, "label", juce::String (p.label));
        set (o, "value", value);
        set (o, "min", p.min);
        set (o, "max", p.max);
        set (o, "def", p.def);
        set (o, "unit", juce::String (p.unit));
        set (o, "kind", p.kind == ParamKind::Bool ? "bool" : p.kind == ParamKind::Enum ? "enum" : "float");
        set (o, "log", p.logTaper);
        if (! p.choices.empty())
        {
            juce::Array<juce::var> ch;
            for (auto* c : p.choices) ch.add (juce::String (c));
            set (o, "choices", ch);
        }
        return o;
    }
    juce::var sectionVar (const juce::String& prefix, const std::vector<ParamSpec>& list, const ParamValues& values)
    {
        juce::Array<juce::var> arr;
        for (auto& p : list)
        {
            auto it = values.find (p.id);
            arr.add (paramVar (prefix + "." + p.id, p, it == values.end() ? p.def : it->second));
        }
        return arr;
    }
}

//==============================================================================
// YouTube view
//==============================================================================
namespace {
    /// Keeps links that YouTube opens "in a new window" inside the same view.
    struct YouTubeBrowser final : juce::WebBrowserComponent
    {
        using juce::WebBrowserComponent::WebBrowserComponent;
        void newWindowAttemptingToLoad (const juce::String& url) override { goToURL (url); }
        /// JUCE answers every failed navigation by loading a plain-text
        /// "Error code: N" page. On YouTube most "failures" are code 9 — a
        /// navigation aborted because another one started (a search typed while
        /// the page was still loading) — and JUCE's error page then CANCELS the
        /// navigation the user just asked for. Measured: the first search landed
        /// on "Error code: 9". Real failures still show WebView2's own error page.
        bool pageLoadHadNetworkError (const juce::String&) override { return false; }
    };

    /// Installed on every call (page navigations wipe it). Holds the loop points
    /// and the requested speed, and re-applies both every 40 ms — YouTube's own
    /// player resets playbackRate when it changes quality or plays an ad.
    const char* kYtBootstrap =
        "(function(){if(window.__le)return;window.__le={a:null,b:null,rate:null};"
        "setInterval(function(){var v=document.querySelector('video');if(!v)return;var L=window.__le;"
        "if(L.rate&&Math.abs(v.playbackRate-L.rate)>0.001)v.playbackRate=L.rate;"
        "if(L.a!=null&&L.b!=null&&L.b>L.a&&(v.currentTime>=L.b||v.currentTime<L.a-0.5))v.currentTime=L.a;},40);})();";
}

/// A web view profile: a cache, so it lives with the machine state, not in
/// Documents. Earlier builds kept it in Documents; the Learn tab's lessons are
/// inside it (IndexedDB), so it is MOVED on first run rather than abandoned.
static juce::File webProfile (const char* name)
{
    const auto to = machineStateDir().getChildFile (name);
    migrateOnce (appDataDir().getChildFile (name), to);
    return to;
}

juce::var LowEndEditor::stemCommand (const juce::var& a)
{
    auto& sep = proc_.stems();
    const auto action = a.getProperty ("action", {}).toString();
    const auto id = a.getProperty ("id", {}).toString();
    const auto quality = (int) a.getProperty ("quality", 0) == 1 ? StemSeparator::Quality::best
                                                                 : StemSeparator::Quality::standard;

    if (action == "status")
    {
        const auto st = sep.status();
        auto o = new juce::DynamicObject();
        static const char* names[] = { "idle", "receiving", "loading", "separating", "writing", "done", "error", "cancelled" };
        o->setProperty ("state", names[(int) st.state]);
        o->setProperty ("stage", st.stage);
        o->setProperty ("progress", st.progress);
        o->setProperty ("job", st.jobId);
        o->setProperty ("error", st.error);
        o->setProperty ("seconds", st.seconds);
        o->setProperty ("modelReady", sep.modelReady());
        o->setProperty ("supported", StemSeparator::cpuSupported());
        if (id.isNotEmpty()) o->setProperty ("hasStems", StemSeparator::hasStems (id));
        return juce::var (o);
    }
    if (action == "begin")    return sep.beginUpload (id, (int) (double) a.getProperty ("frames", 0));
    if (action == "chunk")    return sep.appendChunk (id, (int) (double) a.getProperty ("offset", 0),
                                                      a.getProperty ("data", {}).toString());
    if (action == "start")    return sep.start (id, quality);
    if (action == "cancel")   { sep.cancel(); return true; }
    if (action == "delete")   { StemSeparator::deleteStems (id); return true; }
    if (action == "has")      return StemSeparator::hasStems (id);
    return false;
}

juce::var LowEndEditor::ytCommand (const juce::var& a)
{
    const auto action = a.getProperty ("action", {}).toString();

    if (action == "hide")
    {
        ytVisible_ = false;
        if (yt_) yt_->setVisible (false);
        return true;
    }

    if (yt_ == nullptr)
    {
        auto opts = juce::WebBrowserComponent::Options {}
            .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
                // Its own profile: YouTube sign-in and cookies persist, and never
                // mix with the app UI's storage.
                .withUserDataFolder (webProfile ("WebView2-YouTube"))
                .withBackgroundColour (juce::Colour (0xff0A0C11)))
            .withKeepPageLoadedWhenBrowserIsHidden();
        yt_ = std::make_unique<YouTubeBrowser> (opts);
        addChildComponent (*yt_);          // added after web_, so it sits on top of it
        yt_->goToURL ("https://www.youtube.com/");
    }

    auto js = [this] (const juce::String& body)
    {
        yt_->evaluateJavascript (juce::String (kYtBootstrap)
            + "(function(){var v=document.querySelector('video');var L=window.__le;" + body + "})();");
    };

    if (action == "show")
    {
        const auto r = a.getProperty ("rect", {});
        const auto bounds = juce::Rectangle<int> ((int) r.getProperty ("x", 0), (int) r.getProperty ("y", 0),
                                                  (int) r.getProperty ("w", 0), (int) r.getProperty ("h", 0))
                              .getIntersection (getLocalBounds());
        ytVisible_ = bounds.getWidth() > 40 && bounds.getHeight() > 40;
        yt_->setBounds (bounds);
        yt_->setVisible (ytVisible_);
        return true;
    }
    if (action == "home")    { yt_->goToURL ("https://www.youtube.com/"); return true; }
    if (action == "back")    { yt_->goBack(); return true; }
    if (action == "forward") { yt_->goForward(); return true; }
    if (action == "reload")  { yt_->refresh(); return true; }
    if (action == "go")
    {
        auto url = a.getProperty ("url", {}).toString().trim();
        if (url.isEmpty()) return false;
        // Anything that is not a YouTube address is treated as a search, so
        // the view stays a YouTube browser rather than a general web browser.
        const bool isYouTube = url.containsIgnoreCase ("youtube.com/") || url.containsIgnoreCase ("youtu.be/");
        if (! isYouTube)
            url = "https://www.youtube.com/results?search_query=" + juce::URL::addEscapeChars (url, true);
        else if (! url.startsWithIgnoreCase ("http"))
            url = "https://" + url;
        yt_->goToURL (url);
        return true;
    }
    if (action == "toggle") { js ("if(v){v.paused?v.play():v.pause();}"); return true; }
    if (action == "seek")
    {
        js ("if(v){v.currentTime=Math.max(0,v.currentTime+(" + juce::String ((double) a.getProperty ("delta", 0.0)) + "));}");
        return true;
    }
    if (action == "seekTo")
    {
        js ("if(v){v.currentTime=" + juce::String ((double) a.getProperty ("t", 0.0)) + ";}");
        return true;
    }
    if (action == "rate")
    {
        const double rate = juce::jlimit (0.25, 2.0, (double) a.getProperty ("rate", 1.0));
        js ("L.rate=" + juce::String (rate) + ";if(v){v.preservesPitch=true;v.playbackRate=L.rate;}");
        return true;
    }
    if (action == "loop")
    {
        const auto A = a.getProperty ("a", {}), B = a.getProperty ("b", {});
        js ("L.a=" + (A.isVoid() ? juce::String ("null") : juce::String ((double) A)) + ";"
            "L.b=" + (B.isVoid() ? juce::String ("null") : juce::String ((double) B)) + ";");
        return true;
    }
    return false;
}

/// While the YouTube view is up, report its player state to the page four
/// times a second so the Learn transport shows the right time and speed.
void LowEndEditor::ytPoll()
{
    if (yt_ == nullptr || ! ytVisible_) return;
    juce::Component::SafePointer<LowEndEditor> self (this);
    yt_->evaluateJavascript (
        // The page title is reported even with no video on the page (the home or
        // search page), so the tab can show where the browser is.
        "(function(){var v=document.querySelector('video');var o={title:(document.title||'').replace(/ - YouTube$/,''),"
        "url:location.href,video:!!v,t:0,d:0,p:true,r:1};"
        "if(v){o.t=v.currentTime;o.d=isFinite(v.duration)?v.duration:0;o.p=v.paused;o.r=v.playbackRate;}return o;})()",
        [self] (juce::WebBrowserComponent::EvaluationResult r)
        {
            if (self == nullptr) return;
            if (const auto* v = r.getResult())
                self->emit ("ytStatus", *v);
        });
}

//==============================================================================
LowEndEditor::LowEndEditor (LowEndProcessor& p) : AudioProcessorEditor (p), proc_ (p)
{
    auto opts = juce::WebBrowserComponent::Options {}
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
            .withUserDataFolder (webProfile ("WebView2"))
            .withBackgroundColour (juce::Colour (0xff07090F)))
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const auto& url) { return provideResource (url); })
        .withNativeFunction ("cmd", [this] (const juce::Array<juce::var>& args,
                                            juce::WebBrowserComponent::NativeFunctionCompletion done)
        {
            const auto name = args.size() > 0 ? args[0].toString() : juce::String();
            const auto a = args.size() > 1 ? args[1] : juce::var();
            done (runCommand (name, a));
        });

    web_ = std::make_unique<juce::WebBrowserComponent> (opts);
    web_->setWantsKeyboardFocus (true);
    addAndMakeVisible (*web_);
    web_->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    proc_.addChangeListener (this);
    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (760, 560, 4096, 2400);   // down to iPad-portrait widths, where the nav moves to the bottom
    setSize (1280, 800);
    startTimerHz (30);

    juce::Component::SafePointer<LowEndEditor> self (this);
    juce::MessageManager::callAsync ([self] { if (self != nullptr) self->focusWebView(); });
}

LowEndEditor::~LowEndEditor() { proc_.removeChangeListener (this); }

void LowEndEditor::focusWebView()
{
    if (web_ == nullptr || ! isShowing()) return;
    web_->giveAwayKeyboardFocus();
    web_->grabKeyboardFocus();
}

void LowEndEditor::visibilityChanged() { if (isShowing()) focusWebView(); }
void LowEndEditor::parentHierarchyChanged() { if (isShowing()) focusWebView(); }
void LowEndEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colour (0xff07090F)); }
void LowEndEditor::resized() { web_->setBounds (getLocalBounds()); }

std::optional<juce::WebBrowserComponent::Resource> LowEndEditor::provideResource (const juce::String& url)
{
    if (url == "/" || url == "/index.html")
    {
        juce::WebBrowserComponent::Resource r;
        r.data.assign ((const std::byte*) LowEndUi::index_html,
                       (const std::byte*) LowEndUi::index_html + LowEndUi::index_htmlSize);
        r.mimeType = "text/html";
        return r;
    }
    // The Learn tab's song analyser, loaded by the page into a Web Worker.
    if (url == "/analyzer.js")
    {
        juce::WebBrowserComponent::Resource r;
        r.data.assign ((const std::byte*) LowEndUi::analyzer_js,
                       (const std::byte*) LowEndUi::analyzer_js + LowEndUi::analyzer_jsSize);
        r.mimeType = "text/javascript";
        return r;
    }
    // Fonts, the studio lockup and the third-party notices: all shipped inside
    // the binary, because nothing in this product is fetched at run time.
    {
        struct Item { const char* url; const char* data; int size; const char* mime; };
        const Item items[] = {
            { "/fonts/Inter.woff2", LowEndUi::Inter_woff2, LowEndUi::Inter_woff2Size, "font/woff2" },
            { "/fonts/JetBrainsMono.woff2", LowEndUi::JetBrainsMono_woff2, LowEndUi::JetBrainsMono_woff2Size, "font/woff2" },
            { "/fonts/BarlowSemiCondensed-600.woff2", LowEndUi::BarlowSemiCondensed600_woff2, LowEndUi::BarlowSemiCondensed600_woff2Size, "font/woff2" },
            { "/fonts/BarlowSemiCondensed-700.woff2", LowEndUi::BarlowSemiCondensed700_woff2, LowEndUi::BarlowSemiCondensed700_woff2Size, "font/woff2" },
            { "/about-lockup.png", LowEndUi::aboutlockup_png, LowEndUi::aboutlockup_pngSize, "image/png" },
            { "/notices.txt", LowEndUi::THIRD_PARTY_NOTICES_txt, LowEndUi::THIRD_PARTY_NOTICES_txtSize, "text/plain" } };
        for (auto& it : items)
            if (url == it.url)
            {
                juce::WebBrowserComponent::Resource r;
                r.data.assign ((const std::byte*) it.data, (const std::byte*) it.data + it.size);
                r.mimeType = it.mime;
                return r;
            }
    }
    // The stem player, loaded as an AudioWorklet module (so it must be served
    // from this same origin, not inlined into the page).
    if (url == "/stemplayer.js")
    {
        juce::WebBrowserComponent::Resource r;
        r.data.assign ((const std::byte*) LowEndUi::stemplayer_js,
                       (const std::byte*) LowEndUi::stemplayer_js + LowEndUi::stemplayer_jsSize);
        r.mimeType = "text/javascript";
        return r;
    }
    // Separated stems: /stems/<lesson>/<drums|bass|other|vocals>.wav. The lesson
    // id and the stem name are both checked against what we generate, never used
    // to build a path from whatever the page asked for.
    if (url.startsWith ("/stems/"))
    {
        const auto parts = juce::StringArray::fromTokens (url.substring (7), "/", "");
        if (parts.size() != 2) return std::nullopt;
        const auto id = parts[0];
        const auto stem = parts[1].upToLastOccurrenceOf (".wav", false, false);
        if (! StemSeparator::validId (id)) return std::nullopt;
        int index = -1;
        for (int i = 0; i < StemSeparator::kNumStems; ++i)
            if (stem == StemSeparator::stemName (i)) index = i;
        if (index < 0) return std::nullopt;

        const auto file = StemSeparator::stemDir (id).getChildFile (stem + ".wav");
        juce::MemoryBlock mb;
        if (! file.existsAsFile() || ! file.loadFileAsData (mb)) return std::nullopt;
        juce::WebBrowserComponent::Resource r;
        r.data.assign ((const std::byte*) mb.getData(), (const std::byte*) mb.getData() + mb.getSize());
        r.mimeType = "audio/wav";
        return r;
    }
    return std::nullopt;
}

void LowEndEditor::changeListenerCallback (juce::ChangeBroadcaster*) { stateDirty_ = true; }

void LowEndEditor::timerCallback()
{
    // MIDI learn completes here: the audio thread only ever stored a CC number.
    if (proc_.learnTarget().isNotEmpty())
    {
        if (const int cc = proc_.learnedCc(); cc >= 0)
        {
            const auto path = proc_.learnTarget();
            const auto info = pathInfo (proc_.rig(), path);
            auto binds = proc_.bindings();
            binds.erase (std::remove_if (binds.begin(), binds.end(),
                                         [&path] (const Binding& b) { return b.path == path; }), binds.end());
            Binding b;
            b.cc = cc;
            b.path = path;
            b.lo = info.valid ? info.min : 0.0f;
            b.hi = info.valid ? info.max : 1.0f;
            b.toggle = info.kind == ParamKind::Bool;
            binds.push_back (b);
            proc_.setBindings (std::move (binds));
            proc_.cancelLearn();
            stateDirty_ = true;
        }
    }

    if (stateDirty_) { stateDirty_ = false; emit ("state", buildState()); }
    emit ("tick", buildTick());
    if (++ticksSinceState_ % 30 == 0) emit ("state", buildState());   // once a second, catch-all
    if (++ytPollTicks_ % 8 == 0) ytPoll();
}

void LowEndEditor::emit (const juce::String& event, const juce::var& payload)
{
    if (web_ != nullptr) web_->emitEventIfBrowserIsVisible (event, payload);
}

//==============================================================================
juce::var LowEndEditor::buildState() const
{
    const auto& rig = proc_.rig();
    auto st = obj();

    set (st, "name", rig.name);
    set (st, "notes", rig.notes);
    set (st, "colour", hex (rig.colour));
    set (st, "presetId", proc_.currentPresetId());
    // Does the live rig still match what is on disk? The UI uses this to show
    // "unsaved" and to decide whether Save is meaningful — a player should never
    // have to guess whether their tweak survived the next preset change.
    {
        bool dirty = proc_.currentPresetId().isEmpty();
        if (const auto* saved = proc_.library().findPreset (proc_.currentPresetId()))
        {
            auto live = rig;
            live.id = saved->id;
            live.name = saved->name;
            live.order = saved->order;
            dirty = juce::JSON::toString (rigToVar (live), true) != juce::JSON::toString (rigToVar (*saved), true);
        }
        set (st, "dirty", dirty);
    }
    set (st, "songId", proc_.currentSongId());
    set (st, "scene", proc_.currentScene());
    set (st, "tempo", proc_.tempo());
    set (st, "hostTempo", proc_.usingHostTempo());
    set (st, "tunerActive", proc_.tunerActive());
    set (st, "tester", testerVar (proc_.testerGated(), proc_.testerStatus()));

    set (st, "in", sectionVar ("in", inputParams(), rig.in));
    set (st, "amp", sectionVar ("amp", ampParams(), rig.amp));
    set (st, "cab", sectionVar ("cab", cabParams(), rig.cab));
    set (st, "out", sectionVar ("out", outputParams(), rig.out));
    set (st, "synth", sectionVar ("synth", synthParams(), rig.synth));
    set (st, "loop", sectionVar ("loop", looperParams(), rig.loop));

    // Which amp knobs the current model actually has.
    {
        const auto model = (AmpModel) juce::jlimit (0, kNumAmpModels - 1,
                              (int) std::lround (proc_.getPath ("amp.model")));
        juce::Array<juce::var> vis;
        for (auto* id : ampVisibleParams (model)) vis.add (juce::String (id));
        set (st, "ampVisible", vis);
        set (st, "ampModelName", juce::String (ampModelNames()[(size_t) model]));
    }

    auto chainVar = [] (const juce::String& prefix, const ChainState& chain)
    {
        juce::Array<juce::var> arr;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const auto& b = chain[i];
            const auto& spec = blockSpec (b.type);
            auto o = obj();
            set (o, "index", (int) i);
            set (o, "type", juce::String (spec.id));
            set (o, "name", juce::String (spec.name));
            set (o, "category", juce::String (spec.category));
            set (o, "bypassed", b.bypassed);
            set (o, "bypassPath", prefix + "." + juce::String ((int) i) + ".bypass");
            set (o, "params", sectionVar (prefix + "." + juce::String ((int) i), spec.params, b.params));
            arr.add (o);
        }
        return juce::var (arr);
    };
    set (st, "pre", chainVar ("pre", rig.pre));
    set (st, "post", chainVar ("post", rig.post));
    set (st, "preCapacity", kMaxPreBlocks);
    set (st, "postCapacity", kMaxPostBlocks);

    // The catalog the "add a pedal" sheet is built from.
    {
        juce::Array<juce::var> cat;
        for (auto& b : blockRegistry())
        {
            if (b.type == BlockType::none) continue;
            auto o = obj();
            set (o, "type", juce::String (b.id));
            set (o, "name", juce::String (b.name));
            set (o, "category", juce::String (b.category));
            set (o, "typeIndex", (int) b.type);
            cat.add (o);
        }
        set (st, "catalog", cat);
    }

    // Library
    {
        juce::Array<juce::var> presets;
        for (auto& r : proc_.library().presets())
        {
            auto o = obj();
            set (o, "id", r.id);
            set (o, "name", r.name);
            set (o, "colour", hex (r.colour));
            presets.add (o);
        }
        set (st, "presets", presets);

        juce::Array<juce::var> songs;
        for (auto& s : proc_.library().songs())
        {
            auto o = obj();
            set (o, "id", s.id);
            set (o, "name", s.name);
            set (o, "tempo", s.tempo);
            set (o, "colour", hex (s.colour));
            juce::Array<juce::var> scenes;
            for (auto& pid : s.presetIds)
            {
                auto sc = obj();
                set (sc, "id", pid);
                const auto* pr = proc_.library().findPreset (pid);
                set (sc, "name", pr ? pr->name : juce::String ("Missing"));
                set (sc, "missing", pr == nullptr);
                scenes.add (sc);
            }
            set (o, "scenes", scenes);
            songs.add (o);
        }
        set (st, "songs", songs);

        juce::Array<juce::var> irs;
        for (auto& f : proc_.library().irs()) irs.add (juce::File (f).getFileNameWithoutExtension());
        set (st, "irs", irs);
    }

    // Control map
    {
        juce::Array<juce::var> binds;
        for (auto& b : proc_.bindings())
        {
            auto o = obj();
            set (o, "cc", b.cc);
            set (o, "path", b.path);
            const auto info = pathInfo (proc_.rig(), b.path);
            set (o, "label", info.valid ? info.label : b.path);
            set (o, "lo", b.lo);
            set (o, "hi", b.hi);
            set (o, "toggle", b.toggle);
            binds.add (o);
        }
        set (st, "bindings", binds);

        juce::Array<juce::var> sw;
        for (auto& s : proc_.switchBindings())
        {
            auto o = obj();
            set (o, "cc", s.cc);
            set (o, "role", juce::String (switchRoleId (s.role)));
            sw.add (o);
        }
        set (st, "switches", sw);

        juce::Array<juce::var> roles;
        for (int i = 1; i < (int) SwitchRole::Count; ++i) roles.add (juce::String (switchRoleId ((SwitchRole) i)));
        set (st, "switchRoles", roles);
        set (st, "learning", proc_.learnTarget());
    }

    return st;
}

juce::var LowEndEditor::buildTick() const
{
    const auto& m = proc_.meters();
    auto t = obj();
    set (t, "in", m.inPeak.load());
    set (t, "out", m.outPeak.load());
    set (t, "synth", m.synthPeak.load());
    set (t, "gr", m.gainReduction.load());
    set (t, "supply", m.ampSupply.load());
    set (t, "gate", m.gateOpen.load());
    set (t, "clip", m.clipping.load());
    set (t, "tempo", proc_.tempo());

    auto& lp = proc_.looper();
    auto loop = obj();
    set (loop, "state", juce::String (stateName (lp.state())));
    set (loop, "position", lp.position());
    set (loop, "seconds", lp.lengthSeconds());
    set (loop, "bars", lp.lengthBars());
    set (loop, "layer", lp.currentLayer());
    set (loop, "canUndo", lp.canUndo());
    juce::Array<juce::var> layers;
    for (int i = 0; i < kMaxLoopLayers; ++i) layers.add (lp.layerHasAudio (i));
    set (loop, "layers", layers);
    set (t, "loop", loop);

    auto& tn = proc_.tuner();
    auto tuner = obj();
    set (tuner, "has", tn.hasReading());
    set (tuner, "note", tn.noteName());
    set (tuner, "cents", tn.cents());
    set (tuner, "hz", tn.hz());
    set (tuner, "inTune", tn.inTune());
    set (tuner, "string", tn.nearestString());
    set (t, "tuner", tuner);

    set (t, "trackedNote", proc_.trackedNote());
    set (t, "tracking", proc_.trackedVoiced());

    // Learn tab: what the player is playing right now.
    auto live = obj();
    set (live, "note", proc_.liveNote());
    set (live, "cents", proc_.liveCents());
    set (live, "count", proc_.liveNoteCount());
    set (t, "live", live);
    juce::Array<juce::var> held;
    for (auto n : proc_.heldMidiNotes()) held.add (n);
    set (t, "midi", held);
    return t;
}

//==============================================================================
juce::var LowEndEditor::runCommand (const juce::String& name, const juce::var& a)
{
    auto arg = [&a] (const char* k) { return a.getProperty (k, {}); };
    auto num = [&a] (const char* k, double def = 0.0) { return (double) a.getProperty (k, def); };
    auto str = [&a] (const char* k) { return a.getProperty (k, {}).toString(); };

    if (name == "set")
    {
        const bool ok = proc_.setPath (str ("path"), (float) num ("value"));
        stateDirty_ = true;
        return ok;
    }
    if (name == "addBlock")
    {
        const auto t = blockTypeFromId (str ("type").toStdString());
        if (! t.has_value()) return false;
        return proc_.addBlock (str ("chain") == "post", *t, (int) num ("index", -1));
    }
    if (name == "removeBlock") return proc_.removeBlock (str ("chain") == "post", (int) num ("index", -1));
    if (name == "moveBlock")   return proc_.moveBlock (str ("chain") == "post", (int) num ("from", -1), (int) num ("to", -1));

    if (name == "loadPreset") { proc_.loadPreset (str ("id")); return true; }
    if (name == "loadSong")   { proc_.loadSong (str ("id"), (int) num ("scene", 0)); return true; }
    if (name == "scene")      { proc_.nextScene ((int) num ("delta", 1)); return true; }
    if (name == "savePreset")
    {
        auto r = proc_.rig();
        const auto newName = str ("name");
        if (newName.isNotEmpty()) r.name = newName;
        if ((bool) a.getProperty ("asNew", false)) r.id = {};
        const auto id = proc_.library().savePreset (r);
        proc_.editableRig().id = id;
        proc_.editableRig().name = r.name;
        stateDirty_ = true;
        return id;
    }
    //-------------------------------------------------- presets: make and manage
    // The whole point of these is that a player can build their own rig list
    // without touching a file. "New" starts from the default board rather than
    // from whatever is loaded, so it is a genuinely blank slate; "Duplicate"
    // is the one that copies what you are hearing.
    if (name == "newPreset")
    {
        auto r = emptyRig (str ("name").isNotEmpty() ? str ("name") : juce::String ("New Rig"));
        r.order = 1000;
        const auto id = proc_.library().savePreset (r);
        proc_.loadPreset (id);
        stateDirty_ = true;
        return id;
    }
    if (name == "duplicatePreset")
    {
        const auto* src = proc_.library().findPreset (str ("id"));
        auto r = src != nullptr ? *src : proc_.rig();
        r.id = {};
        r.order = 1000;
        r.name = str ("name").isNotEmpty() ? str ("name") : r.name + " copy";
        const auto id = proc_.library().savePreset (r);
        stateDirty_ = true;
        return id;
    }
    if (name == "renamePreset")
    {
        const auto id = str ("id");
        const auto newName = str ("name");
        if (id.isEmpty() || newName.isEmpty()) return false;
        if (const auto* p = proc_.library().findPreset (id))
        {
            auto r = *p;
            r.name = newName;
            proc_.library().savePreset (r);
            if (proc_.currentPresetId() == id) proc_.editableRig().name = newName;
            stateDirty_ = true;
            return true;
        }
        return false;
    }
    if (name == "deletePreset") { const bool ok = proc_.library().deletePreset (str ("id")); stateDirty_ = true; return ok; }

    //---------------------------------------------------------- songs and scenes
    // A SONG is an ordered list of presets. Each entry is a SCENE — the same
    // preset can appear in several songs, and editing the preset changes it
    // everywhere, which is what a player expects from a setlist.
    if (name == "newSong")
    {
        Song s;
        s.name = str ("name").isNotEmpty() ? str ("name") : juce::String ("New Song");
        s.tempo = proc_.tempo();
        // A set made from the Stage's "New" starts empty and is filled from the
        // picker; one made anywhere else begins with what is playing.
        if (! (bool) a.getProperty ("empty", false) && proc_.currentPresetId().isNotEmpty())
            s.presetIds.add (proc_.currentPresetId());
        const auto id = proc_.library().saveSong (std::move (s));
        proc_.loadSong (id, 0);
        stateDirty_ = true;
        return id;
    }
    if (name == "renameSong")
    {
        if (auto* s = proc_.library().findSongMutable (str ("id")))
        {
            if (str ("name").isNotEmpty()) s->name = str ("name");
            if (a.hasProperty ("tempo")) s->tempo = num ("tempo", s->tempo);
            proc_.library().saveSong (*s);
            stateDirty_ = true;
            return true;
        }
        return false;
    }
    if (name == "deleteSong") { const bool ok = proc_.library().deleteSong (str ("id")); stateDirty_ = true; return ok; }
    if (name == "songScene")
    {
        auto* s = proc_.library().findSongMutable (str ("song"));
        if (s == nullptr) return false;
        const auto action = str ("action");
        const int index = (int) num ("index", -1);
        if (action == "add")
        {
            const auto pid = str ("preset").isNotEmpty() ? str ("preset") : proc_.currentPresetId();
            if (pid.isEmpty()) return false;
            s->presetIds.add (pid);
        }
        else if (action == "remove")
        {
            if (index < 0 || index >= s->presetIds.size()) return false;
            s->presetIds.remove (index);
        }
        else if (action == "move")
        {
            const int to = (int) num ("to", -1);
            if (index < 0 || index >= s->presetIds.size() || to < 0 || to >= s->presetIds.size()) return false;
            s->presetIds.move (index, to);
        }
        else return false;
        proc_.library().saveSong (*s);
        stateDirty_ = true;
        return true;
    }
    if (name == "closeSong") { proc_.clearSong(); stateDirty_ = true; return true; }
    if (name == "rescan")       { proc_.library().scan(); proc_.refreshIrs(); stateDirty_ = true; return true; }

    if (name == "tap")     { proc_.tapTempo(); return proc_.tempo(); }
    if (name == "tempo")   { proc_.setTempo (num ("bpm", 120.0)); stateDirty_ = true; return true; }
    // TEST BUILDS ONLY. In a release build the page never sees "gated" and never
    // asks; the command is answered with an open gate regardless.
    if (name == "tester")
    {
        if (str ("action") == "submit")
        {
            const auto c = proc_.submitTesterKey (str ("key"));
            stateDirty_ = true;
            return testerVar (proc_.testerGated(), c);
        }
        return testerVar (proc_.testerGated(), proc_.testerStatus());
    }
    if (name == "tuner")   { proc_.setTunerActive ((bool) a.getProperty ("on", ! proc_.tunerActive())); return proc_.tunerActive(); }
    if (name == "panic")   { proc_.panic(); return true; }

    if (name == "loop")
    {
        const auto action = str ("action");
        auto& lp = proc_.looper();
        if (action == "record") lp.recordPressed();
        else if (action == "stop") lp.stopPressed();
        else if (action == "undo") lp.undoPressed();
        else if (action == "clear") lp.clearPressed();
        else if (action == "layer") lp.selectLayer ((int) num ("index", 0));
        else if (action == "export")
        {
            const auto file = loopsDir().getChildFile (juce::Time::getCurrentTime().formatted ("Loop %Y-%m-%d %H%M%S") + ".wav");
            // Rendering is a file write, so it runs off the message thread.
            juce::Thread::launch ([&lp, file] { lp.exportToFile (file); });
            return file.getFullPathName();
        }
        return true;
    }


    if (name == "learn")
    {
        const auto path = str ("path");
        if (path.isEmpty()) proc_.cancelLearn();
        else proc_.startLearn (path);
        stateDirty_ = true;
        return true;
    }
    if (name == "unbind")
    {
        auto binds = proc_.bindings();
        const auto path = str ("path");
        binds.erase (std::remove_if (binds.begin(), binds.end(),
                                     [&path] (const Binding& b) { return b.path == path; }), binds.end());
        proc_.setBindings (std::move (binds));
        stateDirty_ = true;
        return true;
    }
    if (name == "bindSwitch")
    {
        auto sw = proc_.switchBindings();
        const int cc = (int) num ("cc", -1);
        const auto role = switchRoleFromId (str ("role"));
        sw.erase (std::remove_if (sw.begin(), sw.end(),
                                  [cc, role] (const SwitchBinding& s) { return s.cc == cc || s.role == role; }), sw.end());
        if (cc >= 0 && role != SwitchRole::none) sw.push_back ({ cc, role });
        proc_.setSwitchBindings (std::move (sw));
        stateDirty_ = true;
        return true;
    }

    // The audio device panel. In a plugin every hook is null and `available` is
    // false, which is how the page knows to say the host owns the audio rather
    // than offering settings that could not do anything.
    if (name == "audio")
    {
        auto& h = LowEndProcessor::audioHooks();
        const auto action = str ("action");
        if (action == "describe")
        {
            if (! h.describe) { auto* o = new juce::DynamicObject(); o->setProperty ("available", false); return juce::var (o); }
            auto v = h.describe();
            if (auto* o = v.getDynamicObject()) o->setProperty ("available", true);
            return v;
        }
        if (action == "apply")   return h.apply ? h.apply (a) : false;
        if (action == "panel")   return h.openDriverPanel ? h.openDriverPanel() : false;
        if (action == "lowest")  return h.chooseLowest ? juce::var (h.chooseLowest()) : juce::var (false);
        return false;
    }
    if (name == "about")
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", "Low End");
        o->setProperty ("version", LOWEND_VERSION);
        o->setProperty ("standalone", (bool) LowEndProcessor::audioHooks().describe);
        o->setProperty ("licensed", false);
        o->setProperty ("tester", proc_.testerGated());
        return juce::var (o);
    }
    // Links leave the product and open in the player's own browser. Only the
    // studio's own addresses can be opened this way: the page cannot be used to
    // launch anything else.
    if (name == "openLink")
    {
        const auto which = str ("which");
        juce::String target;
        if (which == "legal")        target = "https://amanorsac.studio/legal";
        else if (which == "privacy") target = "https://amanorsac.studio/privacy";
        else if (which == "site")    target = "https://amanorsac.studio";
        else if (which == "support") target = "mailto:hello@amanorsac.studio";
        return target.isNotEmpty() && juce::URL (target).launchInDefaultBrowser();
    }
    if (name == "openFolder") { LowEndProcessor::appDataDirectory().revealToUser(); return true; }
    if (name == "refresh")    { stateDirty_ = true; return true; }
    if (name == "yt")         return ytCommand (a);
    if (name == "stem")       return stemCommand (a);

    juce::ignoreUnused (arg);
    return false;
}

} // namespace lowend
