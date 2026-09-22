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
    }
    ~LowEndWindow() override { saveBounds(); }

    void closeButtonPressed() override { saveBounds(); juce::JUCEApplicationBase::quit(); }

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
            o->setProperty ("open", true);
        }
        else
        {
            o->setProperty ("open", false);
            o->setProperty ("error", dm.getCurrentAudioDevice() == nullptr ? juce::String ("No audio device is open") : juce::String());
        }
        o->setProperty ("rates", rates);
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
        const auto err = dm.setAudioDeviceSetup (setup, true);
        if (a.hasProperty ("muteInput") && pluginHolder != nullptr)
            pluginHolder->getMuteInputValue().setValue ((bool) a.getProperty ("muteInput", false));
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
    /// A bass player plugging into a laptop wants the lowest latency the machine
    /// can give without being asked. ASIO first, then the exclusive-mode WASAPI
    /// paths, at 128 samples.
    /// Takes the best driver the machine offers and the smallest buffer that
    /// driver will actually give — not a fixed 128, because an interface whose
    /// minimum is 64 should use 64, and one that cannot go below 256 should not
    /// be asked to. Returns what it settled on, for the player to read.
    juce::String chooseLowLatencyDefaults()
    {
        auto& dm = getDeviceManager();
        for (auto& name : driverPreference())
            for (auto* t : dm.getAvailableDeviceTypes())
                if (t->getTypeName() == name)
                {
                    t->scanForDevices();
                    if (t->getDeviceNames (false).isEmpty()) continue;
                    dm.setCurrentAudioDeviceType (name, true);

                    auto setup = dm.getAudioDeviceSetup();
                    setup.bufferSize = 128;
                    dm.setAudioDeviceSetup (setup, true);
                    if (auto* dev = dm.getCurrentAudioDevice())
                    {
                        // Now that a device is open its real buffer sizes are
                        // known: take the smallest it offers at or above 64.
                        int best = dev->getCurrentBufferSizeSamples();
                        for (auto b : dev->getAvailableBufferSizes())
                            if (b >= 64 && b < best) best = b;
                        if (best != dev->getCurrentBufferSizeSamples())
                        {
                            setup = dm.getAudioDeviceSetup();
                            setup.bufferSize = best;
                            dm.setAudioDeviceSetup (setup, true);
                        }
                        dev = dm.getCurrentAudioDevice();
                        if (dev == nullptr) return {};
                        const double sr = dev->getCurrentSampleRate();
                        const int bs = dev->getCurrentBufferSizeSamples();
                        return name + " · " + juce::String (bs) + " samples · "
                             + juce::String (sr > 0 ? 1000.0 * bs / sr : 0.0, 1) + " ms";
                    }
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
    void systemRequestedQuit() override { quit(); }

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
