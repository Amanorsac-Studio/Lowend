// StandaloneApp.cpp — Low End's own standalone window.
//
// Subclasses JUCE's StandaloneFilterWindow to keep its device manager and state
// handling, hides the "Options" button (the gear in the Setup view replaces it),
// remembers window bounds, and — the part that matters on stage — prefers a
// low-latency driver on first launch instead of whatever the OS defaults to.
#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone

#include "LowEndEditor.h"
#include "LowEndProcessor.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace lowend {

class LowEndWindow final : public juce::StandaloneFilterWindow
{
public:
    LowEndWindow (const juce::String& title, juce::PropertySet* settings, bool firstLaunch)
        : juce::StandaloneFilterWindow (title, juce::Colour (0xff07090F), settings, false), settings_ (settings)
    {
        for (auto* c : getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (c))
                if (b->getButtonText() == "Options") { b->setVisible (false); b->setEnabled (false); }
        setTitleBarButtonsRequired (juce::DocumentWindow::minimiseButton
                                    | juce::DocumentWindow::maximiseButton
                                    | juce::DocumentWindow::closeButton, false);
        setResizable (true, false);
        // Smaller than this and the screens would have to hide things; every
        // layout is built and tested to fit at this size and above (iPad width).
        setResizeLimits (1024, 680, 16384, 16384);
        setUsingNativeTitleBar (true);

        // JUCE's standalone wrapper mutes the audio INPUT by default and shows a
        // "muted to avoid feedback loop" banner. That default is right for a
        // synth on a laptop with a built-in mic; it is wrong for an amp, where a
        // muted input means the app does nothing at all and the player has no
        // idea why. Low End is an effect on a jack, so the input is live from the
        // first launch. (The mute is still reachable in the audio settings.)
        if (pluginHolder != nullptr)
            pluginHolder->getMuteInputValue().setValue (false);

        restoreBounds();
        if (firstLaunch) chooseLowLatencyDefaults();
        loadRouting();
        getDeviceManager().addChangeListener (&deviceListener_);
        deviceListener_.fn = [this] { reapplyRouting(); notifyPage(); };
        watch_.fn = [this] { rescanDevices(); };
        watch_.startTimer (4000);
        juce::MessageManager::callAsync ([this] { reapplyRouting(); });
    }
    ~LowEndWindow() override
    {
        watch_.stopTimer();
        getDeviceManager().removeChangeListener (&deviceListener_);
        saveBounds();
    }

    void closeButtonPressed() override
    {
        // Edits not yet saved: the page asks what to do with each one, and
        // quits itself once answered. Nothing is lost by closing either way —
        // the edits are also kept on disk — but the player is asked first.
        if (pluginHolder != nullptr)
            if (auto* p = dynamic_cast<LowEndProcessor*> (pluginHolder->processor.get()))
                if (p->hasPendingEdits())
                    if (auto* ed = dynamic_cast<LowEndEditor*> (p->getActiveEditor()))
                    {
                        ed->askToClose();
                        toFront (true);
                        return;
                    }
        saveBounds();
        juce::JUCEApplicationBase::quit();
    }

    void resized() override
    {
        juce::StandaloneFilterWindow::resized();
        if (! isFullScreen() && ! isMinimised() && settings_ != nullptr) lastBounds_ = getBounds();
    }
    void moved() override
    {
        juce::StandaloneFilterWindow::moved();
        if (! isFullScreen() && ! isMinimised()) lastBounds_ = getBounds();
    }

    void activeWindowStatusChanged() override
    {
        juce::StandaloneFilterWindow::activeWindowStatusChanged();
        if (isActiveWindow() && pluginHolder != nullptr && pluginHolder->processor != nullptr)
            if (auto* editor = dynamic_cast<LowEndEditor*> (pluginHolder->processor->getActiveEditor()))
                editor->focusWebView();
    }

private:
    void restoreBounds()
    {
        if (settings_ == nullptr) return;
        const auto s = settings_->getValue ("lowEndWindowBounds");
        if (s.isEmpty()) return;
        const auto b = juce::Rectangle<int>::fromString (s);
        for (auto& d : juce::Desktop::getInstance().getDisplays().displays)
        {
            const auto ua = d.userBounds.toNearestInt();
            if (ua.intersects (b) && ua.contains (b.getPosition().translated (40, 20)))
            {
                setBounds (b.constrainedWithin (ua));
                return;
            }
        }
    }
    void saveBounds()
    {
        if (settings_ != nullptr && ! lastBounds_.isEmpty())
            settings_->setValue ("lowEndWindowBounds", lastBounds_.toString());
    }
public:
    /// Drivers best-first. The order IS the recommendation: ASIO talks to the
    /// interface directly, the exclusive WASAPI modes bypass the Windows mixer,
    /// and shared Windows Audio and DirectSound are the fallbacks that add
    /// tens of milliseconds. On macOS there is only Core Audio, and it is fine.
    static juce::StringArray driverPreference()
    {
        return { "ASIO", "Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)",
                 "CoreAudio", "Windows Audio", "DirectSound" };
    }
    /// How a driver is described to the player, since the raw names do not say
    /// which of them will actually be quick.
    static juce::String driverNote (const juce::String& type)
    {
        if (type == "ASIO")                            return "Native, lowest latency";
        if (type == "Windows Audio (Exclusive Mode)")  return "Exclusive, low latency";
        if (type == "Windows Audio (Low Latency Mode)")return "Shared, low latency";
        if (type == "CoreAudio")                       return "Native";
        if (type == "Windows Audio")                   return "Shared, higher latency";
        if (type == "DirectSound")                     return "Fallback, highest latency";
        return {};
    }

    /// Everything the settings panel needs, in one call.
    juce::var describeAudio()
    {
        auto& dm = getDeviceManager();
        auto* o = new juce::DynamicObject();
        auto* type = dm.getCurrentDeviceTypeObject();
        auto* dev  = dm.getCurrentAudioDevice();

        juce::Array<juce::var> drivers;
        const auto pref = driverPreference();
        juce::StringArray have;
        for (auto* t : dm.getAvailableDeviceTypes()) have.add (t->getTypeName());
        juce::StringArray ordered;
        for (auto& p : pref) if (have.contains (p)) ordered.add (p);
        for (auto& h : have) if (! ordered.contains (h)) ordered.add (h);
        for (auto& name : ordered)
        {
            auto* d = new juce::DynamicObject();
            d->setProperty ("name", name);
            d->setProperty ("note", driverNote (name));
            drivers.add (juce::var (d));
        }
        o->setProperty ("drivers", drivers);
        o->setProperty ("driver", type != nullptr ? type->getTypeName() : juce::String());
        if (auto* cur = dm.getCurrentAudioDevice()) o->setProperty ("wrapper", isWrapperDevice (cur->getName()));

        auto names = [] (const juce::StringArray& a)
        {
            juce::Array<juce::var> v;
            for (auto& s : a) v.add (s);
            return v;
        };
        if (type != nullptr)
        {
            o->setProperty ("outputs", names (type->getDeviceNames (false)));
            o->setProperty ("inputs",  names (type->getDeviceNames (true)));
        }
        const auto setup = dm.getAudioDeviceSetup();
        o->setProperty ("output", setup.outputDeviceName);
        o->setProperty ("input",  setup.inputDeviceName);

        juce::Array<juce::var> rates, buffers;
        if (dev != nullptr)
        {
            // Rates below 44.1 kHz are offered by some drivers but are of no use
            // for an instrument amp; they are left out so the menu is short.
            for (auto r : dev->getAvailableSampleRates())
                if (r >= 44100.0 || (int) r == (int) dev->getCurrentSampleRate()) rates.add ((int) r);
            // Some drivers (ASIO4ALL, for one) offer every multiple of 16 from
            // 64 to 2048 — 129 entries, which is not a menu anyone can use on
            // stage. Offer the sizes players actually choose between, plus
            // whatever is in use and the smallest the device allows.
            {
                const auto available = dev->getAvailableBufferSizes();
                const int current = dev->getCurrentBufferSizeSamples();
                const int smallest = available.isEmpty() ? current : available[0];
                juce::Array<int> keep;
                for (auto b : available)
                    if (b == current || b == smallest || (b >= 32 && (b & (b - 1)) == 0))
                        keep.addIfNotAlreadyThere (b);
                keep.sort();
                for (auto b : keep) buffers.add (b);
            }
            o->setProperty ("rate", (int) dev->getCurrentSampleRate());
            o->setProperty ("buffer", dev->getCurrentBufferSizeSamples());
            o->setProperty ("inputLatency", dev->getInputLatencyInSamples());
            o->setProperty ("outputLatency", dev->getOutputLatencyInSamples());
            o->setProperty ("hasDriverPanel", dev->hasControlPanel());
            // What the Settings panel shows live: how hard the audio thread is
            // working, and how many times the driver ran dry this session
            // (-1 where the driver cannot say).
            o->setProperty ("cpu", dm.getCpuUsage());
            o->setProperty ("xruns", dev->getXRunCount());

            // The card's own channels, by the names the driver gives them.
            juce::Array<juce::var> ins, outs;
            for (auto& n : dev->getInputChannelNames()) ins.add (n);
            for (auto& n : dev->getOutputChannelNames()) outs.add (n);
            o->setProperty ("inputChannels", ins);
            o->setProperty ("outputChannels", outs);
            o->setProperty ("inCh", inCh_);
            o->setProperty ("inStereo", inStereo_);
            o->setProperty ("outCh", outCh_);
            o->setProperty ("outMono", outMono_);
            o->setProperty ("clickCh", clickCh_);
            // kept for the page's click-pair menu: pairs by number
            juce::Array<juce::var> pairs;
            const auto chNames = dev->getOutputChannelNames();
            for (int p = 0; p * 2 + 1 < chNames.size(); ++p)
                pairs.add (juce::String (p * 2 + 1) + "-" + juce::String (p * 2 + 2) + "  " + chNames[p * 2] + " / " + chNames[p * 2 + 1]);
            o->setProperty ("outputPairs", pairs);
            o->setProperty ("clickPair", clickCh_ >= 0 ? clickCh_ / 2 : 0);
            o->setProperty ("open", true);
        }
        else
        {
            o->setProperty ("open", false);
            o->setProperty ("error", dm.getCurrentAudioDevice() == nullptr ? juce::String ("No audio device is open") : juce::String());
        }
        o->setProperty ("rates", rates);
        // Every interface the computer has, on every driver, named as a DAW
        // names them: the card first, the driver after it.
        {
            juce::Array<juce::var> devs;
            for (auto* t : dm.getAvailableDeviceTypes())
            {
                const auto tn = t->getTypeName();
                const bool separateIo = tn != "ASIO";
                for (auto& n : t->getDeviceNames (false))
                {
                    auto* d = new juce::DynamicObject();
                    d->setProperty ("type", tn);
                    d->setProperty ("name", n);
                    d->setProperty ("label", n + "  —  " + shortDriver (tn));
                    d->setProperty ("wrapper", isWrapperDevice (n));
                    devs.add (juce::var (d));
                }
                if (separateIo && type != nullptr && t == type)
                {
                    juce::Array<juce::var> inDevs;
                    for (auto& n : t->getDeviceNames (true)) inDevs.add (n);
                    o->setProperty ("inputDevices", inDevs);
                }
            }
            // ASIO first (it is the one with the lowest latency), then the rest.
            DeviceOrder order;
            devs.sort (order, true);
            o->setProperty ("devices", devs);
            o->setProperty ("separateInput", type != nullptr && type->getTypeName() != "ASIO");
        }
        o->setProperty ("buffers", buffers);
        o->setProperty ("muteInput", pluginHolder != nullptr && (bool) pluginHolder->getMuteInputValue().getValue());
        return juce::var (o);
    }

    bool applyAudio (const juce::var& a)
    {
        auto& dm = getDeviceManager();
        const auto driver = a.getProperty ("driver", {}).toString();
        if (driver.isNotEmpty())
        {
            auto* cur = dm.getCurrentDeviceTypeObject();
            if (cur == nullptr || cur->getTypeName() != driver)
            {
                for (auto* t : dm.getAvailableDeviceTypes())
                    if (t->getTypeName() == driver) { t->scanForDevices(); dm.setCurrentAudioDeviceType (driver, true); break; }
            }
        }
        auto setup = dm.getAudioDeviceSetup();
        if (a.hasProperty ("output")) { setup.outputDeviceName = a.getProperty ("output", {}).toString(); setup.useDefaultOutputChannels = true; }
        if (a.hasProperty ("input"))  { setup.inputDeviceName  = a.getProperty ("input", {}).toString();  setup.useDefaultInputChannels = true; }
        if (a.hasProperty ("rate"))   setup.sampleRate = (double) a.getProperty ("rate", 0);
        if (a.hasProperty ("buffer")) setup.bufferSize = (int) a.getProperty ("buffer", 0);
        if (a.hasProperty ("device"))
        {
            // A card, with its driver: switch driver if needed, then the card.
            const auto t = a.getProperty ("type", {}).toString();
            if (t.isNotEmpty() && (dm.getCurrentDeviceTypeObject() == nullptr || dm.getCurrentDeviceTypeObject()->getTypeName() != t))
            {
                dm.setCurrentAudioDeviceType (t, true);
                setup = dm.getAudioDeviceSetup();
            }
            setup.outputDeviceName = a.getProperty ("device", {}).toString();
            if (t == "ASIO" || ! a.hasProperty ("inputDevice")) setup.inputDeviceName = t == "ASIO" ? setup.outputDeviceName : setup.inputDeviceName;
            // A new card starts from its first input in mono and its first pair.
            inCh_ = 0; inStereo_ = false; outCh_ = 0; outMono_ = false; clickCh_ = -1;
        }
        if (a.hasProperty ("inputDevice")) setup.inputDeviceName = a.getProperty ("inputDevice", {}).toString();
        if (a.hasProperty ("inCh"))    inCh_ = juce::jmax (0, (int) a.getProperty ("inCh", 0));
        if (a.hasProperty ("inStereo")) inStereo_ = (bool) a.getProperty ("inStereo", false);
        if (a.hasProperty ("outCh"))   outCh_ = juce::jmax (0, (int) a.getProperty ("outCh", 0));
        if (a.hasProperty ("outMono")) outMono_ = (bool) a.getProperty ("outMono", false);
        if (a.hasProperty ("clickPair")) { const int p = (int) a.getProperty ("clickPair", 0); clickCh_ = p > 0 ? p * 2 : -1; }
        if (a.hasProperty ("clickCh"))   clickCh_ = (int) a.getProperty ("clickCh", -1);
        // The channels that are opened are exactly the ones in use.
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setBit (inCh_);
        if (inStereo_) setup.inputChannels.setBit (inCh_ + 1);
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setBit (outCh_);
        if (! outMono_) setup.outputChannels.setBit (outCh_ + 1);
        if (clickCh_ >= 0 && clickCh_ != outCh_ && clickCh_ + 1 != outCh_ && (outMono_ || clickCh_ != outCh_ + 1))
        { setup.outputChannels.setBit (clickCh_); setup.outputChannels.setBit (clickCh_ + 1); }
        else clickCh_ = -1;
        const auto err = dm.setAudioDeviceSetup (setup, true);
        if (a.hasProperty ("muteInput") && pluginHolder != nullptr)
            pluginHolder->getMuteInputValue().setValue ((bool) a.getProperty ("muteInput", false));
        saveRouting();
        reapplyRouting();
        return err.isEmpty();
    }

    /// One tap: the best driver this machine has, at its smallest buffer.
    juce::String chooseLowestLatency() { return chooseLowLatencyDefaults(); }

    bool openDriverPanel()
    {
        if (auto* dev = getDeviceManager().getCurrentAudioDevice())
            if (dev->hasControlPanel())
                return dev->showControlPanel();
        return false;
    }

private:
    //------------------------------------------------------------ routing
    int inCh_ = 0, outCh_ = 0, clickCh_ = -1;
    bool inStereo_ = false, outMono_ = false;
    juce::String deviceSig_;

    static juce::String shortDriver (const juce::String& t)
    {
        if (t == "Windows Audio (Exclusive Mode)") return "WASAPI exclusive";
        if (t == "Windows Audio (Low Latency Mode)") return "WASAPI low latency";
        if (t == "Windows Audio") return "WASAPI shared";
        return t;
    }
    struct DeviceOrder
    {
        static int rank (const juce::var& v)
        {
            const auto t = v.getProperty ("type", {}).toString();
            return t == "ASIO" ? 0 : t == "CoreAudio" ? 0 : t.contains ("Exclusive") ? 1 : t.contains ("Low Latency") ? 2 : t == "Windows Audio" ? 3 : 4;
        }
        int compareElements (const juce::var& a, const juce::var& b) const
        {
            // real interfaces before wrappers and virtual devices
            const int w = (int) (bool) a.getProperty ("wrapper", false) - (int) (bool) b.getProperty ("wrapper", false);
            if (w != 0) return w;
            const int d = rank (a) - rank (b);
            return d != 0 ? d : a.getProperty ("name", {}).toString().compareNatural (b.getProperty ("name", {}).toString());
        }
    };
    void loadRouting()
    {
        if (settings_ == nullptr) return;
        inCh_ = settings_->getIntValue ("inCh", 0); inStereo_ = settings_->getBoolValue ("inStereo", false);
        outCh_ = settings_->getIntValue ("outCh", 0); outMono_ = settings_->getBoolValue ("outMono", false);
        clickCh_ = settings_->getIntValue ("clickCh", -1);
    }
    void saveRouting()
    {
        if (settings_ == nullptr) return;
        settings_->setValue ("inCh", inCh_); settings_->setValue ("inStereo", inStereo_);
        settings_->setValue ("outCh", outCh_); settings_->setValue ("outMono", outMono_);
        settings_->setValue ("clickCh", clickCh_);
    }
    /// The engine sees the opened channels packed in order; tell it where, in
    /// that packing, the main mix and the click's pair are.
    void reapplyRouting()
    {
        auto* dev = getDeviceManager().getCurrentAudioDevice();
        auto* p = pluginHolder != nullptr ? dynamic_cast<LowEndProcessor*> (pluginHolder->processor.get()) : nullptr;
        if (dev == nullptr || p == nullptr) return;
        const auto active = dev->getActiveOutputChannels();
        auto rankOf = [&active] (int ch) { int k = 0; for (int i = 0; i < ch; ++i) if (active[i]) ++k; return active[ch] ? k : -1; };
        const int main = rankOf (outCh_);
        const int click = clickCh_ >= 0 ? rankOf (clickCh_) : -1;
        p->setOutputRouting (juce::jmax (0, main), outMono_, click);
        // Where a Cover take lands: the round trip the driver reports.
        p->setDeviceLatency (dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples());
    }
    void notifyPage()
    {
        if (pluginHolder != nullptr && pluginHolder->processor != nullptr)
            if (auto* ed = dynamic_cast<LowEndEditor*> (pluginHolder->processor->getActiveEditor()))
                ed->audioDevicesChanged();
    }
    /// Background: an interface plugged in or out shows up without a restart.
    void rescanDevices()
    {
        juce::String sig;
        for (auto* t : getDeviceManager().getAvailableDeviceTypes())
        {
            // ASIO is never rescanned in the background. Its list comes from the
            // registry and does not change when an interface is plugged in, and
            // loading ASIO drivers while another program is also loading them
            // (a second Amanorsac app starting) deadlocked the app that held the
            // interface. The other types enumerate cheaply and show hot-plugs.
            if (t->getTypeName() == "ASIO") { sig << "ASIO;\n"; continue; }
            t->scanForDevices();
            sig << t->getTypeName() << ':' << t->getDeviceNames (false).joinIntoString ("|") << ';' << t->getDeviceNames (true).joinIntoString ("|") << '\n';
        }
        if (sig != deviceSig_) { const bool first = deviceSig_.isEmpty(); deviceSig_ = sig; if (! first) notifyPage(); }
    }
    struct Fn : juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } } watch_;
    struct Listener : juce::ChangeListener { std::function<void()> fn; void changeListenerCallback (juce::ChangeBroadcaster*) override { if (fn) fn(); } } deviceListener_;

    /// A bass player plugging into a laptop wants the lowest latency the machine
    /// can give without being asked. ASIO first, then the exclusive-mode WASAPI
    /// paths, at 128 samples.
    /// Takes the best driver the machine offers and the smallest buffer that
    /// driver will actually give — not a fixed 128, because an interface whose
    /// minimum is 64 should use 64, and one that cannot go below 256 should not
    /// be asked to. Returns what it settled on, for the player to read.
public:
    /// Wrappers and virtual drivers. They open, but they are not the interface
    /// the player plugged in (ASIO4ALL with nothing routed is digital silence),
    /// so they are only ever a last resort.
    static bool isWrapperDevice (const juce::String& n)
    {
        for (auto* w : { "ASIO4ALL", "FlexASIO", "SoundGrid", "Ableton", "Voicemeeter", "VB-Audio", "CABLE",
                         "Generic Low Latency", "Realtek ASIO", "ASIO2WASAPI", "JackRouter", "Virtual" })
            if (n.containsIgnoreCase (w)) return true;
        return false;
    }

private:
    /// How likely a device is the player's interface. An ASIO driver stays
    /// installed when its hardware is unplugged, so a driver whose name shares
    /// a word with a CONNECTED Windows device ("UMC" in "BEHRINGER UMC 404HD")
    /// ranks first; wrappers rank last.
    int deviceScore (const juce::String& name)
    {
        int score = 0;
        if (isWrapperDevice (name)) score -= 100;
        static const juce::StringArray generic { "ASIO", "Driver", "USB", "Audio", "Thunderbolt", "Device", "Steinberg", "Interface" };
        juce::StringArray words;
        words.addTokens (name, " -_()/", "");
        juce::StringArray connected;
        for (auto* t : getDeviceManager().getAvailableDeviceTypes())
            if (t->getTypeName() != "ASIO")
            {
                connected.addArray (t->getDeviceNames (false));
                connected.addArray (t->getDeviceNames (true));
            }
        for (auto& w : words)
        {
            if (w.length() < 3 || generic.contains (w, true)) continue;
            for (auto& c : connected)
                if (c.containsIgnoreCase (w)) { score += 50; break; }
        }
        return score;
    }

    /// Opens `device` of driver `type` and checks it really carries audio both
    /// ways. Returns false (leaving it closed for the next candidate) otherwise.
    bool tryOpen (const juce::String& type, const juce::String& device)
    {
        auto& dm = getDeviceManager();
        dm.setCurrentAudioDeviceType (type, true);
        auto setup = dm.getAudioDeviceSetup();
        setup.outputDeviceName = device;
        setup.inputDeviceName = device;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
        setup.bufferSize = 128;
        if (dm.setAudioDeviceSetup (setup, true).isNotEmpty()) return false;
        auto* dev = dm.getCurrentAudioDevice();
        return dev != nullptr && dev->getActiveInputChannels().countNumberOfSetBits() > 0
                              && dev->getActiveOutputChannels().countNumberOfSetBits() > 0;
    }

    juce::String chooseLowLatencyDefaults()
    {
        auto& dm = getDeviceManager();
        for (auto* t : dm.getAvailableDeviceTypes()) t->scanForDevices();

        // Every device of every driver, best first: the fastest driver, then the
        // device most likely to be the interface. Wrappers only after every
        // real device of every driver has been tried.
        struct Candidate { juce::String type, name; int score, rank; };
        std::vector<Candidate> all;
        const auto pref = driverPreference();
        for (auto* t : dm.getAvailableDeviceTypes())
        {
            const int rank = pref.indexOf (t->getTypeName());
            if (rank < 0) continue;
            for (auto& n : t->getDeviceNames (false))
                all.push_back ({ t->getTypeName(), n, deviceScore (n), rank });
        }
        std::stable_sort (all.begin(), all.end(), [] (const Candidate& a, const Candidate& b)
        {
            const bool wa = a.score < 0, wb = b.score < 0;
            if (wa != wb) return wb;                       // real devices before wrappers
            if (a.score >= 50 && b.score >= 50 && a.rank != b.rank) return a.rank < b.rank;
            if ((a.score >= 50) != (b.score >= 50)) return a.score >= 50;   // the interface itself first
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.score > b.score;
        });

        // If the interface itself is there but will not open, it is almost
        // always held by another program. A wrapper would open, and be silent,
        // so the player is told instead and the current device is put back.
        const auto previousType = dm.getCurrentAudioDeviceType();
        const auto previousSetup = dm.getAudioDeviceSetup();
        juce::String busyInterface;
        for (auto& c : all)
        {
            if (c.score < 0 && busyInterface.isNotEmpty())
            {
                dm.setCurrentAudioDeviceType (previousType, true);
                dm.setAudioDeviceSetup (previousSetup, true);
                return busyInterface + " is in use by another program. Close that program, then press Lowest safe latency again.";
            }
            if (! tryOpen (c.type, c.name))
            {
                if (c.score >= 50 && busyInterface.isEmpty()) busyInterface = c.name;
                continue;
            }
            auto* dev = dm.getCurrentAudioDevice();
            // Now that a device is open its real buffer sizes are known: take
            // the smallest it offers at or above 64.
            int best = dev->getCurrentBufferSizeSamples();
            for (auto b : dev->getAvailableBufferSizes())
                if (b >= 64 && b < best) best = b;
            if (best != dev->getCurrentBufferSizeSamples())
            {
                auto setup = dm.getAudioDeviceSetup();
                setup.bufferSize = best;
                dm.setAudioDeviceSetup (setup, true);
            }
            dev = dm.getCurrentAudioDevice();
            if (dev == nullptr) return {};
            const double sr = dev->getCurrentSampleRate();
            const int bs = dev->getCurrentBufferSizeSamples();
            return c.name + " · " + c.type + " · " + juce::String (bs) + " samples · "
                 + juce::String (sr > 0 ? 1000.0 * bs / sr : 0.0, 1) + " ms";
        }
        return {};
    }

    juce::PropertySet* settings_;
    juce::Rectangle<int> lastBounds_;
};

class LowEndApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Low End"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Low End";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.folderName = "Amanorsac Studio";
        // Window position and the chosen audio device are machine state, so the
        // file lives in %LOCALAPPDATA% / Application Support — not in Roaming,
        // which is where JUCE would put it. An earlier build's file is moved.
        const auto settingsFile = machineStateDir().getChildFile ("Low End.settings");
        migrateOnce (o.getDefaultFile(), settingsFile);
        settingsFile.getParentDirectory().createDirectory();
        props_ = std::make_unique<juce::PropertiesFile> (settingsFile, o);
        const bool first = ! props_->containsKey ("launched");
        props_->setValue ("launched", true);

        // The app's own settings panel drives the device through these. JUCE's
        // generic device dialog is never shown: a buyer must not meet a panel
        // that was not designed (Build Standard B53).
        auto& hooks = LowEndProcessor::audioHooks();
        hooks.describe        = [this] { return window_ != nullptr ? window_->describeAudio() : juce::var(); };
        hooks.apply           = [this] (const juce::var& a) { return window_ != nullptr && window_->applyAudio (a); };
        hooks.openDriverPanel = [this] { return window_ != nullptr && window_->openDriverPanel(); };
        hooks.chooseLowest    = [this] { return window_ != nullptr ? window_->chooseLowestLatency() : juce::String(); };

        window_ = std::make_unique<LowEndWindow> (getApplicationName(), props_.get(), first);
        window_->setVisible (true);
    }
    void shutdown() override
    {
        LowEndProcessor::audioHooks() = {};
        window_ = nullptr;
        if (props_) props_->saveIfNeeded();
    }
    void systemRequestedQuit() override { if (window_ != nullptr) window_->closeButtonPressed(); else quit(); }

private:
    std::unique_ptr<juce::PropertiesFile> props_;
    std::unique_ptr<LowEndWindow> window_;
};

} // namespace lowend

#if JUCE_IOS || JUCE_ANDROID
juce::JUCEApplicationBase* juce_CreateApplication() { return new lowend::LowEndApplication(); }
#else
START_JUCE_APPLICATION (lowend::LowEndApplication)
#endif

#endif
