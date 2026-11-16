#pragma once

#include "PluginProcessor.h"
#include <array>
#include <memory>

struct SculptLiveCleanLookAndFeel final : juce::LookAndFeel_V4
{
    SculptLiveCleanLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float position, float startAngle, float endAngle, juce::Slider&) override;
};

class SculptLiveCleanEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SculptLiveCleanEditor (SculptLiveCleanProcessor&);
    ~SculptLiveCleanEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawMeter (juce::Graphics&, juce::Rectangle<float>, float level, const juce::String& label);
    static juce::String frequencyText (float hz);

    SculptLiveCleanProcessor& processor;
    SculptLiveCleanLookAndFeel look;
    std::array<juce::Slider, 3> amounts;
    juce::Slider output;
    juce::TextButton learn { "Apprendi rumore" }, hold { "HOLD" }, reset { "Reset" }, bypass { "BYPASS" };
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 3> amountAttachments;
    juce::AudioProcessorValueTreeState::SliderAttachment outputAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment holdAttachment, bypassAttachment;
    juce::TooltipWindow tooltip { this, 650 };
    float inputLevel = 0.0f, outputLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptLiveCleanEditor)
};
