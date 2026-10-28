/*
  ==============================================================================
    PluginEditor.h
  ==============================================================================
*/
#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "GWLookAndFeel.h"
#include "GWVisuals.h"

//==============================================================================
// ComboBox subclass that adds proper keyboard navigation:
// Enter/Space opens the popup; Up/Down cycle items without opening it.
class KeyboardComboBox : public juce::ComboBox
{
public:
    using juce::ComboBox::ComboBox;

    bool keyPressed(const juce::KeyPress& key) override
    {
        const int current = getSelectedId();   // 1-based, 0 = nothing selected
        const int total = getNumItems();

        if (key == juce::KeyPress::upKey || key == juce::KeyPress::leftKey)
        {
            const int next = juce::jmax(1, current - 1);
            if (next != current) setSelectedId(next, juce::sendNotification);
            return true;
        }
        if (key == juce::KeyPress::downKey || key == juce::KeyPress::rightKey)
        {
            const int next = juce::jmin(total, current + 1);
            if (next != current) setSelectedId(next, juce::sendNotification);
            return true;
        }
        if (key == juce::KeyPress::returnKey
            || key == juce::KeyPress(juce::KeyPress::spaceKey))
        {
            showPopup();
            return true;
        }
        return juce::ComboBox::keyPressed(key);
    }
};

//==============================================================================
// Everything is laid out at a fixed design size (kW × kH) inside this view;
// the editor scales it to whatever size the window is.
class CosmicMainView : public juce::Component, private juce::Timer
{
public:
    static constexpr int kW = 1280, kH = 800;

    explicit CosmicMainView(CosmicsynthAudioProcessor&);
    ~CosmicMainView() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void presetChanged(PresetId pid);
    void updateReadouts(PresetId pid);
    static juce::String presetDescription(PresetId pid);

    CosmicsynthAudioProcessor& proc;

    // header
    KeyboardComboBox presetBox;
    juce::TextButton prevButton{ "<" }, nextButton{ ">" };
    juce::TextButton loadButton{ "LOAD CSV" }, builtInButton{ "BUILT-IN" };
    juce::ToggleButton liveToggle{ "GW LIVE" };
    std::unique_ptr<juce::FileChooser> chooser;

    // gravitational-wave panel
    ReadoutColumn leftReadout, rightReadout;
    OrbitView orbit;
    StrainView strain;
    juce::String gwSubtitle, strainTitle;

    // spectrogram panel
    SpectrogramView spectrogram;
    std::vector<float> scopeScratch = std::vector<float>(8192, 0.0f);

    // modulation panel (only parameters that reach the audio)
    LabelledKnob kAmp{ "Amp Depth" }, kPitch{ "Pitch Depth" }, kFilter{ "Filter Depth" },
        kTimbre{ "Timbre Depth" }, kSmooth{ "Smoothing" }, kTilt{ "KL Tilt" }, kLens{ "Lens Mass" };
    using APVTS = CosmicsynthAudioProcessor::APVTS;
    std::vector<std::unique_ptr<APVTS::SliderAttachment>> sliderAttachments;
    std::unique_ptr<APVTS::ButtonAttachment> liveAttachment;

    juce::String description, dataName;
    int shownPreset = -1;
    double lastTickMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CosmicMainView)
};

//==============================================================================
class CosmicsynthAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    CosmicsynthAudioProcessorEditor(CosmicsynthAudioProcessor&);
    ~CosmicsynthAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    GWLookAndFeel lookAndFeel;      // declared first: outlives the view
    CosmicMainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CosmicsynthAudioProcessorEditor)
};