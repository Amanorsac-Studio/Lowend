// LowEndEditor.h — WebView host and native bridge.
//
// Same shape as Sanctuary's editor, and for the same reason: the UI is the HTML
// in `resources/ui/index.html`, hosted in a `juce::WebBrowserComponent`, so what
// is designed is what ships. Two events go out (`state`, the whole rig, on every
// change; `tick`, meters and transport, at 30 Hz) and one function comes back
// (`cmd(name, args)`), which routes into the processor's path space.
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "LowEndProcessor.h"

namespace lowend {

class LowEndEditor final : public juce::AudioProcessorEditor,
                           private juce::ChangeListener,
                           private juce::Timer
{
public:
    explicit LowEndEditor (LowEndProcessor&);
    ~LowEndEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    /// The WebView's native child window does not get OS keyboard focus through
    /// JUCE's focus system on its own; this is re-called whenever the window
    /// could have lost it. (Same fix as Sanctuary — see commit aee3727.)
    void focusWebView();


private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    std::optional<juce::WebBrowserComponent::Resource> provideResource (const juce::String& url);
    juce::var runCommand (const juce::String& name, const juce::var& args);
    juce::var buildState() const;
    juce::var buildTick() const;
    void emit (const juce::String& event, const juce::var& payload);

    /// Learn tab → YouTube. A second, ordinary browser view laid over a
    /// placeholder in the page. It is a real browser (search, sign-in, the full
    /// site), which an <iframe> could not be: youtube.com refuses to be framed.
    /// The Learn tab's own transport drives its <video> element through
    /// evaluateJavascript — speed and A–B looping — the same way a browser
    /// extension would. Its audio is never captured or saved.
    juce::var ytCommand (const juce::var& args);
    void ytPoll();

    /// Learn tab → stem separation. The page hands the decoded song over in
    /// base64 chunks, asks for a job, then polls "status" while it runs; the
    /// finished stems are served back as WAV files from provideResource. All of
    /// it is forwarded to the processor's StemSeparator, which owns the work.
    juce::var stemCommand (const juce::var& args);

    LowEndProcessor& proc_;
    std::unique_ptr<juce::WebBrowserComponent> web_;
    std::unique_ptr<juce::WebBrowserComponent> yt_;
    bool ytVisible_ = false;
    int ytPollTicks_ = 0;
    bool stateDirty_ = true;
    int ticksSinceState_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LowEndEditor)
};

} // namespace lowend
