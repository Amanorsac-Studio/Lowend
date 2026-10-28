// LowEndEditor.cpp — see header.
#include "LowEndEditor.h"
#include "LowEndUi.h"
#include "../engine/PresetPacks.h"
#include "../engine/NamModel.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace lowend {

/// Both web views are WebView2 windows parented to the same top-level window,
/// and the page's own window covers the whole editor. Whichever is higher in
/// the z-order wins, and the page tends to end up on top (it is focused, it was
/// created first and re-raised), which hides the YouTube view completely while
/// it goes on loading pages behind it. So every time the view is shown, its
/// window is raised: every WebView2 host window that is NOT the full size of
/// the editor is the YouTube view.
static void raiseOverlayWebViews (juce::Component& editor)
{
   #if JUCE_WINDOWS
    auto* peer = editor.getPeer();
    if (peer == nullptr) return;
    auto top = (HWND) peer->getNativeHandle();
    RECT client {};
    if (! GetClientRect (top, &client)) return;
    for (auto child = GetWindow (top, GW_CHILD); child != nullptr; child = GetWindow (child, GW_HWNDNEXT))
    {
        wchar_t cls[64] {};
        GetClassNameW (child, cls, 64);
        if (juce::String (cls) != "Chrome_WidgetWin_0" || ! IsWindowVisible (child)) continue;
        RECT r {};
        GetWindowRect (child, &r);
        const bool fullSize = (r.right - r.left) >= (client.right - client.left) - 2
                           && (r.bottom - r.top) >= (client.bottom - client.top) - 2;
        if (! fullSize)
            SetWindowPos (child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
   #else
    juce::ignoreUnused (editor);
   #endif
}

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
            case Looper::State::armed:         return "armed";
            case Looper::State::cued:          return "cued";
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

/// The Cover tab. The song is played and recorded by the engine (CoverPlayer);
/// the page only sends the transport and the mix and draws what it is told.
juce::var LowEndEditor::coverCommand (const juce::var& a)
{
    auto& cv = proc_.cover();
    const auto action = a.getProperty ("action", {}).toString();
    const auto num = [&a] (const char* k, double d) { return a.hasProperty (k) ? (double) a.getProperty (k, d) : d; };
    const auto id = a.getProperty ("id", {}).toString();

    if (action == "status") return cv.describe();
    if (action == "has")    return ! CoverPlayer::songFiles (id).isEmpty();
    if (action == "load")   return cv.load (id, a.getProperty ("name", {}).toString());
    if (action == "unload") { cv.unload(); return true; }
    if (action == "begin")  return cv.beginUpload (id, (int) num ("frames", 0));
    if (action == "chunk")  return cv.appendChunk (id, (int) num ("offset", 0), a.getProperty ("data", {}).toString());
    if (action == "end")    return cv.finishUpload (id, a.getProperty ("name", {}).toString());
    if (action == "play")   return cv.play (num ("from", 0), (int) num ("countIn", 0), num ("bpm", 120), (bool) a.getProperty ("record", false));
    if (action == "stop")   { cv.stop(); return true; }
    if (action == "punch")  { cv.setPunch (num ("a", -1), num ("b", -1)); return true; }
    if (action == "levels") { cv.setLevels ((float) num ("song", 0), (float) num ("take", 0), (bool) a.getProperty ("hearTake", true)); return true; }
    if (action == "stem")   { cv.setStem ((int) num ("index", -1), (float) num ("db", 0), (bool) a.getProperty ("mute", false), (bool) a.getProperty ("solo", false)); return true; }
    if (action == "click")  { cv.setClick ((bool) a.getProperty ("on", false), num ("bpm", 120), num ("t0", 0), (int) num ("beats", 4), (float) num ("db", -6)); return true; }
    if (action == "clickBeats")
    {
        std::vector<double> t;
        std::vector<bool> acc;
        if (auto* b = a.getProperty ("beats", {}).getArray()) for (auto& v : *b) t.push_back ((double) v);
        if (auto* b = a.getProperty ("accents", {}).getArray()) for (auto& v : *b) acc.push_back ((bool) v);
        cv.setClickBeats (t, std::move (acc));
        return true;
    }
    if (action == "take")   return cv.selectTake ((int) num ("number", 0));
    if (action == "deleteTake") return cv.deleteTake ((int) num ("number", 0));
    if (action == "export") return cv.exportMix ((int) num ("number", 0));
    if (action == "reveal")
    {
        const auto f = juce::File (a.getProperty ("path", {}).toString());
        // Only files Cover itself wrote.
        if (f.isAChildOf (CoverPlayer::coversDir())) { f.revealToUser(); return true; }
        return false;
    }
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
        if (ytVisible_)
        {
            yt_->toFront (false);
            // The native window appears a moment after the component does.
            juce::Component::SafePointer<LowEndEditor> self (this);
            raiseOverlayWebViews (*this);
            juce::Timer::callAfterDelay (150, [self] { if (self != nullptr && self->ytVisible_) raiseOverlayWebViews (*self); });
        }
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
    raiseOverlayWebViews (*this);         // the page can take the top of the z-order back (focus, reload)
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

    if (proc_.cover().poll()) emit ("cover", proc_.cover().describe());
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
        set (st, "dirty", proc_.isDirty());
        set (st, "override", proc_.usingOverride());
        // Every edit not yet saved, so lists can mark them and Close can ask.
        juce::Array<juce::var> pend;
        for (auto& e : proc_.pendingEdits())
        {
            auto o = obj();
            const bool ov = e.songId.isNotEmpty() && e.scene >= 0;
            const auto* sg = proc_.library().findSong (e.songId);
            const bool isOverride = sg != nullptr && sg->overrides.count (e.scene) > 0 && sg->presetIds[e.scene] == e.presetId;
            set (o, "key", LowEndProcessor::editKey (e.presetId, e.songId, e.scene, isOverride));
            set (o, "preset", e.presetId);
            set (o, "name", e.rig.name);
            set (o, "song", ov && sg ? e.songId : juce::String());
            set (o, "songName", ov && sg ? sg->name : juce::String());
            set (o, "scene", e.scene);
            set (o, "override", isOverride);
            pend.add (o);
        }
        set (st, "edits", pend);
        set (st, "liveKey", proc_.liveEditKey());
    }
    set (st, "songId", proc_.currentSongId());
    set (st, "scene", proc_.currentScene());
    set (st, "tempo", proc_.tempo());
    // What Low End itself adds (oversampling, convolution, the limiter lookahead),
    // in samples, so the Settings panel can show the round trip honestly.
    set (st, "latency", proc_.getLatencySamples());
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
            // Its controls at their defaults, so the pedal browser can draw the
            // real faceplate before the pedal is on the board.
            set (o, "params", sectionVar ("catalog." + juce::String (b.id), b.params, defaultBlockState (b.type).params));
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
            set (o, "category", r.category);
            set (o, "factory", r.factoryKey.isNotEmpty());
            if (r.packId.isNotEmpty()) { set (o, "packId", r.packId); set (o, "packName", r.packName); set (o, "packAuthor", r.packAuthor); }
            // What the Library needs to draw a preset without loading it: its
            // chain at a glance and its history.
            auto pv = [] (const ParamValues& v, const char* k, float d) { auto it = v.find (k); return it == v.end() ? d : it->second; };
            set (o, "amp", (int) pv (r.amp, "model", 0.0f));
            set (o, "cab", (int) pv (r.cab, "model", 1.0f));
            auto chain = [] (const ChainState& c)
            {
                juce::Array<juce::var> out;
                for (auto& b : c)
                {
                    auto e = juce::var (new juce::DynamicObject());
                    e.getDynamicObject()->setProperty ("id", juce::String (blockSpec (b.type).id));
                    e.getDynamicObject()->setProperty ("name", juce::String (blockSpec (b.type).name));
                    e.getDynamicObject()->setProperty ("category", juce::String (blockSpec (b.type).category));
                    e.getDynamicObject()->setProperty ("off", b.bypassed);
                    out.add (e);
                }
                return out;
            };
            set (o, "pre", chain (r.pre));
            set (o, "post", chain (r.post));
            juce::Array<juce::var> hist;
            for (auto& c : r.history)
            {
                auto e = obj();
                set (e, "t", (double) c.time);
                set (e, "x", c.text);
                hist.add (e);
            }
            set (o, "history", hist);
            presets.add (o);
        }
        set (st, "presets", presets);

        // Preset packs the player installed, and the categories the Library filters by.
        juce::Array<juce::var> installed;
        for (auto& i : packs::installed()) installed.add (packs::infoToVar (i));
        set (st, "packs", installed);
        set (st, "packExt", packs::extension());
        juce::Array<juce::var> cats;
        for (auto* c : factoryCategories()) cats.add (juce::String (c));
        set (st, "categories", cats);

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

        // Your Amp: the captures in the Amp Captures folder, each with the key a
        // preset keeps for it.
        juce::Array<juce::var> caps;
        for (auto& f : proc_.library().captures())
        {
            auto c = obj();
            set (c, "key", NamModel::keyFor (f));
            set (c, "name", juce::File::createFileWithoutCheckingPath (f).getFileNameWithoutExtension());
            caps.add (c);
        }
        set (st, "captures", caps);
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
        set (st, "bypassAll", proc_.bypassAll());
        set (st, "tunerRef", proc_.tunerReference());
        set (st, "tunerMutes", proc_.tunerMutes());
        set (st, "gateAuto", proc_.gateAuto());
        set (st, "humHz", proc_.humHz());
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
    set (t, "floor", m.noiseFloorDb.load());
    set (t, "clip", m.clipping.load());
    set (t, "tempo", proc_.tempo());
    {
        auto& cv = proc_.cover();
        auto c = obj();
        set (c, "pos", cv.positionSeconds());
        set (c, "playing", cv.isPlaying());
        set (c, "recording", cv.isRecording());
        set (c, "countIn", cv.isCountingIn());
        set (t, "cover", c);
    }

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
    set (loop, "countingIn", proc_.countingIn());
    set (loop, "grid", lp.gridActive());
    set (t, "loop", loop);

    // The click: settings plus what the player needs to see live.
    const auto& cs = proc_.clickSettings();
    auto click = obj();
    set (click, "on", cs.on);
    set (click, "level", cs.levelDb);
    set (click, "sound", cs.sound);
    set (click, "accent", cs.accent);
    set (click, "beats", cs.beatsPerBar);
    set (click, "unit", cs.beatUnit);
    set (click, "loopBars", cs.loopBars);
    set (click, "eighths", cs.eighths);
    set (click, "countIn", cs.countInBars);
    set (click, "output", cs.output);
    set (click, "beat", proc_.clickBeat());
    set (click, "separate", proc_.clickSeparateAvailable());
    set (click, "standalone", proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    set (t, "click", click);

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
    if (name == "transferBlock")
        return proc_.transferBlock (str ("from") == "post", (int) num ("fromIndex", -1),
                                    str ("to") == "post", (int) num ("toIndex", -1));

    if (name == "loadPreset") { proc_.loadPreset (str ("id")); return true; }
    if (name == "loadSong")   { proc_.loadSong (str ("id"), (int) num ("scene", 0)); return true; }

    if (name == "resolveEdit")
    {
        const auto how = str ("how");
        return proc_.resolveEdit (str ("key"), how == "set" ? LowEndProcessor::Resolve::toSet
                                              : how == "discard" ? LowEndProcessor::Resolve::discard
                                              : LowEndProcessor::Resolve::toPreset);
    }
    if (name == "quit")
    {
        // The close sheet has been answered: quit for real (standalone only).
        if (juce::JUCEApplicationBase::isStandaloneApp())
            juce::MessageManager::callAsync ([] { if (auto* app = juce::JUCEApplicationBase::getInstance()) app->quit(); });
        return true;
    }
    if (name == "savePreset")
    {
        auto r = proc_.rig();
        const auto newName = str ("name");
        if (newName.isNotEmpty()) r.name = newName;
        if ((bool) a.getProperty ("asNew", false)) { r.id = {}; r.history.clear(); noteChange (r, "Saved as a new preset"); }
        else if (const auto* before = proc_.library().findPreset (r.id))
        {
            r.history = before->history;
            noteChange (r, describeRigChange (*before, r));
        }
        else noteChange (r, "Created");
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
        noteChange (r, "Created");
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
        const auto from = r.name;
        r.name = str ("name").isNotEmpty() ? str ("name") : r.name + " copy";
        r.history.clear();
        noteChange (r, "Created from " + from);
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
    if (name == "setCategory")
    {
        const auto id = str ("id");
        const auto cat = packs::cleanText (a.getProperty ("category", {}), 24);
        if (const auto* p = proc_.library().findPreset (id))
        {
            auto r = *p;
            r.category = cat;
            proc_.library().savePreset (r);
            if (proc_.currentPresetId() == id) proc_.editableRig().category = cat;
            stateDirty_ = true;
            return true;
        }
        return false;
    }

    //---------------------------------------------------------- preset packs
    // A pack arrives as the text of a file the player dropped or chose; the
    // page reads it (so no path from the page is ever opened here) and every
    // byte of it is checked in packs::fromText before anything is installed.
    if (name == "readPack" || name == "installPack")
    {
        packs::Pack pack;
        const auto res = packs::fromText (str ("text"), pack);
        auto o = obj();
        set (o, "ok", res.wasOk());
        set (o, "error", res.getErrorMessage());
        if (res.failed()) return o;
        if (name == "installPack")
        {
            set (o, "added", packs::install (proc_.library(), pack));
            stateDirty_ = true;
            return o;
        }
        set (o, "info", packs::infoToVar (pack.info));
        set (o, "skipped", pack.skippedPedals);
        bool already = false;
        for (auto& i : packs::installed()) already = already || i.id == pack.info.id;
        set (o, "installed", already);
        juce::Array<juce::var> list;
        for (auto& r : pack.presets)
        {
            auto e = obj();
            set (e, "name", r.name);
            set (e, "category", r.category);
            list.add (e);
        }
        set (o, "presets", list);
        return o;
    }
    if (name == "removePack")
    {
        const int n = packs::remove (proc_.library(), str ("id"));
        stateDirty_ = true;
        return n;
    }
    if (name == "writePack")
    {
        auto ids = a.getProperty ("ids", {});
        if (! ids.isArray() || ids.size() == 0) return false;
        juce::StringArray list;
        for (auto& v : *ids.getArray()) list.add (v.toString());
        packs::Info info;
        info.name = str ("name");
        info.author = str ("author");
        info.description = str ("description");
        info.colour = (juce::uint32) juce::jlimit (0.0, 4294967295.0, (double) a.getProperty ("colour", (double) 0xff2a8fa8)) | 0xff000000u;
        info.id = str ("id");     // re-exporting an installed pack keeps its identity
        const auto file = packs::write (proc_.library(), info, list);
        if (! file.existsAsFile()) return false;
        auto o = obj();
        set (o, "file", file.getFileName());
        set (o, "bytes", (double) file.getSize());
        lastPackFile_ = file;
        return o;
    }
    if (name == "showPackFile")
    {
        // Only ever the file this session wrote, or the folder itself.
        if (lastPackFile_.existsAsFile() && lastPackFile_.isAChildOf (packsDir())) lastPackFile_.revealToUser();
        else { packsDir().createDirectory(); packsDir().revealToUser(); }
        return true;
    }
    //---------------------------------------------------------- Your Amp captures
    // Like a pack, a capture arrives as the text of a file the player chose or
    // dropped: nothing from the page is ever opened as a path. It is built and
    // timed (NamModel) before it is copied into the Amp Captures folder.
    if (name == "importCapture")
    {
        auto o = obj();
        const auto text = str ("text");
        juce::String err;
        NamModel::Info info;
        if (! NamModel::check (text, err, &info)) { set (o, "ok", false); set (o, "error", err); return o; }
        auto base = juce::File::createLegalFileName (str ("name").upToLastOccurrenceOf (".", false, false).trim()).substring (0, 80);
        if (base.isEmpty()) base = info.name.isNotEmpty() ? juce::File::createLegalFileName (info.name) : juce::String ("My Amp");
        namDir().createDirectory();
        auto file = namDir().getChildFile (base + ".nam");
        for (int k = 2; file.existsAsFile() && file.loadFileAsString() != text && k < 100; ++k)
            file = namDir().getChildFile (base + " (" + juce::String (k) + ").nam");
        if (! file.existsAsFile() && ! file.replaceWithText (text, false, false, nullptr))
        {
            set (o, "ok", false); set (o, "error", "The capture could not be saved."); return o;
        }
        proc_.library().scan();
        stateDirty_ = true;
        set (o, "ok", true);
        set (o, "key", NamModel::keyFor (file.getFileName()));
        set (o, "name", file.getFileNameWithoutExtension());
        set (o, "gear", info.gear);
        set (o, "rate", info.modelRate);
        return o;
    }
    if (name == "removeCapture")
    {
        const auto f = NamModel::fileFor ((int) num ("key", 0.0), namDir());
        if (! f.existsAsFile() || ! f.isAChildOf (namDir())) return false;
        const bool ok = f.moveToTrash();
        proc_.library().scan();
        stateDirty_ = true;
        return ok;
    }
    if (name == "showCaptures") { namDir().createDirectory(); namDir().revealToUser(); return true; }

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
        // Record goes through the processor: a count-in may run first.
        if (action == "record") proc_.requestLoopRecord();
        else if (action == "stop") { proc_.requestCountInCancel(); lp.stopPressed(); }
        else if (action == "undo") lp.undoPressed();
        else if (action == "clear") { proc_.requestCountInCancel(); lp.clearPressed(); }
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


    if (name == "click")
    {
        // Any subset of the settings; the rest keep their values.
        auto cs = proc_.clickSettings();
        if (a.hasProperty ("on"))      cs.on = (bool) a.getProperty ("on", false);
        if (a.hasProperty ("level"))   cs.levelDb = (float) num ("level", -6.0);
        if (a.hasProperty ("sound"))   cs.sound = (int) num ("sound", 0);
        if (a.hasProperty ("accent"))  cs.accent = (bool) a.getProperty ("accent", true);
        if (a.hasProperty ("beats"))   cs.beatsPerBar = juce::jlimit (1, 12, (int) num ("beats", 4));
        if (a.hasProperty ("unit"))    cs.beatUnit = (int) num ("unit", 4) == 8 ? 8 : 4;
        if (a.hasProperty ("loopBars")) cs.loopBars = (int) num ("loopBars", 0);
        if (a.hasProperty ("eighths")) cs.eighths = (bool) a.getProperty ("eighths", false);
        if (a.hasProperty ("countIn")) cs.countInBars = (int) num ("countIn", 0);
        if (a.hasProperty ("output"))  cs.output = (int) num ("output", 0);
        proc_.setClickSettings (cs);
        stateDirty_ = true;
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
    if (name == "exportPreset")
    {
        const auto* p = proc_.library().findPreset (str ("id"));
        if (p == nullptr) return false;
        auto dir = exportsDir();
        dir.createDirectory();
        auto file = dir.getNonexistentChildFile (juce::File::createLegalFileName (p->name), ".rig.json", false);
        if (! file.replaceWithText (juce::JSON::toString (rigToVar (*p), false))) return false;
        file.revealToUser();
        return file.getFullPathName();
    }
    if (name == "sharePresets")
    {
        // Beta: the presets a tester chose, copied with readable names into
        // Documents/Amanorsac Studio/<Product>/Beta Presets, with a note on how
        // to send them. Nothing leaves the computer: the tester sends the files.
        auto ids = a.getProperty ("ids", {});
        if (! ids.isArray() || ids.size() == 0 || ids.size() > 200) return false;
        const auto credit = a.getProperty ("credit", {}).toString().trim().substring (0, 60);
        auto dir = betaPresetsDir();
        dir.createDirectory();
        int written = 0;
        for (auto& v : *ids.getArray())
        {
            const auto* p = proc_.library().findPreset (v.toString());
            if (p == nullptr) continue;
            auto copy = *p;
            if (credit.isNotEmpty()) copy.notes = (copy.notes.isNotEmpty() ? copy.notes + "\n" : juce::String()) + "Shared by " + credit;
            auto obj = rigToVar (copy);
            if (auto* d = obj.getDynamicObject())
            {
                d->setProperty ("sharedBy", credit);
                d->setProperty ("sharedFrom", juce::String (JucePlugin_Name) + " " + JucePlugin_VersionString);
                d->setProperty ("sharedOn", juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
            }
            auto file = dir.getNonexistentChildFile (juce::File::createLegalFileName (copy.name), ".rig.json", false);
            if (file.replaceWithText (juce::JSON::toString (obj, false))) ++written;
        }
        dir.getChildFile ("READ ME - how to send these.txt").replaceWithText (
            juce::String (JucePlugin_Name) + " beta: presets to send back\n"
            "\n"
            "This folder holds the presets you chose in Library > Share presets.\n"
            "Each file is one preset. Nothing here is sent automatically.\n"
            "\n"
            "To send them:\n"
            "  1. Select the .rig.json files you want to share (or zip the folder).\n"
            "  2. Attach them to a new issue on the beta's GitHub page, using the\n"
            "     \"Preset submission\" form, or email them to hello@amanorsac.studio.\n"
            "\n"
            "By sending a preset you agree that Amanorsac Studio may include it,\n"
            "with your credit if you gave one, in a future version of the product.\n");
        dir.revealToUser();
        return written;
    }
    if (name == "backup")
    {
        // Everything the player made, in one zip they can keep somewhere else.
        const auto src = LowEndProcessor::appDataDirectory();
        auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Low End Backups");
        dir.createDirectory();
        auto zip = dir.getNonexistentChildFile ("Low End backup " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M"), ".zip", false);
        juce::ZipFile::Builder b;
        for (auto& f : src.findChildFiles (juce::File::findFiles, true))
            // Stems (Learn) are rebuilt from the song and the model is reinstalled;
            // both are hundreds of megabytes, so neither goes in the backup.
            if (! f.isAChildOf (src.getChildFile ("Learn")) && ! f.isAChildOf (src.getChildFile ("Models")))
                b.addFile (f, 6, f.getRelativePathFrom (src).replaceCharacter ('\\', '/'));
        juce::FileOutputStream os (zip);
        if (! os.openedOk()) return false;
        b.writeToStream (os, nullptr);
        os.flush();
        zip.revealToUser();
        return zip.getFullPathName();
    }
    if (name == "bypassAll") { proc_.setBypassAll ((bool) a.getProperty ("on", ! proc_.bypassAll())); stateDirty_ = true; return proc_.bypassAll(); }
    if (name == "gateAuto")  { proc_.setGateAuto ((bool) a.getProperty ("on", ! proc_.gateAuto())); stateDirty_ = true; return proc_.gateAuto(); }
    if (name == "hum")       { proc_.setHumHz ((int) num ("hz", 0)); stateDirty_ = true; return proc_.humHz(); }
    if (name == "tunerRef")  { proc_.setTunerReference ((float) num ("hz", 440.0)); stateDirty_ = true; return proc_.tunerReference(); }
    if (name == "tunerMute") { proc_.setTunerMutes ((bool) a.getProperty ("on", true)); stateDirty_ = true; return true; }
    if (name == "scene")     { if (a.hasProperty ("index")) proc_.jumpToScene ((int) num ("index", 0)); else proc_.nextScene ((int) num ("delta", 1)); return true; }
    if (name == "openFolder") { LowEndProcessor::appDataDirectory().revealToUser(); return true; }
    if (name == "refresh")    { stateDirty_ = true; return true; }
    if (name == "yt")         return ytCommand (a);
    if (name == "stem")       return stemCommand (a);
    if (name == "cover")      return coverCommand (a);

    juce::ignoreUnused (arg);
    return false;
}

} // namespace lowend
