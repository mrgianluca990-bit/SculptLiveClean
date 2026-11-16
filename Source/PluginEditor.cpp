#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace
{
const juce::Colour cream { 0xfff2ead2 }, warm { 0xffe8935a }, amber { 0xfff0c060 }, cool { 0xff5fc8c0 };
const std::array<juce::Colour, 3> accents { warm, amber, cool };
constexpr float startAngle = juce::MathConstants<float>::pi * 1.25f;
constexpr float endAngle = juce::MathConstants<float>::pi * 2.75f;
}

SculptLiveCleanLookAndFeel::SculptLiveCleanLookAndFeel()
{
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff292929));
    setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff474236));
    setColour (juce::TextButton::textColourOffId, cream);
    setColour (juce::TextButton::textColourOnId, cream);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff282828));
    setColour (juce::TooltipWindow::textColourId, cream);
    setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff555555));
}

void SculptLiveCleanLookAndFeel::drawRotarySlider (juce::Graphics& graphics, int x, int y,
    int width, int height, float position, float a0, float a1, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                        static_cast<float> (width), static_cast<float> (height)).reduced (5.0f);
    const auto centre = bounds.getCentre();
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float knobRadius = radius * 0.80f;
    const auto colour = slider.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, a1, true);
    graphics.setColour (colour.withAlpha (0.25f));
    graphics.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (position > 0.0f)
    {
        juce::Path active;
        active.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, a0 + position * (a1 - a0), true);
        graphics.setColour (colour);
        graphics.strokePath (active, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    graphics.setColour (juce::Colour (0xff151515));
    graphics.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
    graphics.setColour (juce::Colour (0xff373737));
    graphics.drawEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.5f);

    juce::Path tick;
    tick.addRoundedRectangle (-1.5f, -knobRadius * 0.90f, 3.0f, knobRadius * 0.23f, 1.0f);
    graphics.setColour (cream);
    graphics.fillPath (tick, juce::AffineTransform::rotation (a0 + position * (a1 - a0)).translated (centre.x, centre.y));

    const bool isOutput = slider.getName() == "Output";
    const auto text = isOutput ? juce::String (slider.getValue(), 1) : juce::String (static_cast<int> (std::lround (slider.getValue())));
    graphics.setFont (juce::Font (juce::FontOptions (isOutput ? 13.0f : 25.0f)));
    graphics.setColour (cream);
    graphics.drawText (text, bounds, juce::Justification::centred);
}

SculptLiveCleanEditor::SculptLiveCleanEditor (SculptLiveCleanProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      outputAttachment (p.apvts, "output", output),
      holdAttachment (p.apvts, "feedbackHold", hold),
      bypassAttachment (p.apvts, "bypass", bypass)
{
    setLookAndFeel (&look);
    const std::array<const char*, 3> ids { "denoise", "dereverb", "defeedback" };
    const std::array<const char*, 3> names { "Denoise", "Dereverb", "Defeedback" };
    const std::array<const char*, 3> hints {
        "Riduzione del rumore. Prima apprendi 2 secondi di solo rumore. Doppio clic: 0%.",
        "Riduzione prudente delle code ambientali. Protegge gli attacchi; aumenta gradualmente. Doppio clic: 0%.",
        "Profondità dei filtri antifeedback. AUTO rileva nuovi picchi; HOLD conserva i filtri trovati. Doppio clic: 0%."
    };
    for (std::size_t i = 0; i < amounts.size(); ++i)
    {
        auto& slider = amounts[i];
        slider.setName (names[i]);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (startAngle, endAngle, true);
        slider.setColour (juce::Slider::rotarySliderFillColourId, accents[i]);
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setTextValueSuffix (" %");
        slider.setTooltip (hints[i]);
        addAndMakeVisible (slider);
        amountAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, ids[i], slider);
    }
    output.setName ("Output");
    output.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    output.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    output.setRotaryParameters (startAngle, endAngle, true);
    output.setColour (juce::Slider::rotarySliderFillColourId, cream);
    output.setDoubleClickReturnValue (true, 0.0);
    output.setTextValueSuffix (" dB");
    output.setTooltip ("Volume di uscita. Doppio clic: 0 dB.");
    output.setPopupDisplayEnabled (true, false, this);
    addAndMakeVisible (output);

    hold.setClickingTogglesState (true);
    bypass.setClickingTogglesState (true);
    hold.setTooltip ("AUTO: aggiunge filtri sui picchi persistenti. HOLD: conserva i filtri trovati, senza aggiungerne altri.");
    bypass.setTooltip ("Ascolta il segnale originale. Gli stati audio restano aggiornati.");
    learn.setTooltip ("Premi durante 2 secondi di solo rumore, senza voce o musica. Il profilo resta fisso fino al prossimo apprendimento.");
    reset.setTooltip ("Rimuove gradualmente tutti i filtri antifeedback. HOLD evita che vengano ricreati subito.");
    learn.onClick = [this] { processor.requestNoiseLearn(); };
    reset.onClick = [this] { processor.requestFeedbackReset(); };
    for (auto* button : { &learn, &hold, &reset, &bypass }) addAndMakeVisible (*button);

    setOpaque (true);
    setSize (800, 460);
    startTimerHz (25);
    timerCallback();
}

SculptLiveCleanEditor::~SculptLiveCleanEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SculptLiveCleanEditor::resized()
{
    for (std::size_t i = 0; i < amounts.size(); ++i)
        amounts[i].setBounds (62 + static_cast<int> (i) * 260, 100, 156, 156);
    learn.setBounds (55, 280, 170, 28);
    hold.setBounds (555, 280, 110, 28);
    reset.setBounds (675, 280, 90, 28);
    bypass.setBounds (678, 15, 102, 25);
    output.setBounds (666, 377, 68, 68);
}

void SculptLiveCleanEditor::timerCallback()
{
    inputLevel = std::max (processor.display.inputPeak.load (std::memory_order_relaxed), inputLevel * 0.83f);
    outputLevel = std::max (processor.display.outputPeak.load (std::memory_order_relaxed), outputLevel * 0.83f);
    const bool learning = processor.display.noiseLearning.load (std::memory_order_relaxed);
    learn.setEnabled (! learning);
    learn.setButtonText (learning ? "Apprendimento…" : "Apprendi rumore");
    hold.setButtonText (hold.getToggleState() ? "HOLD" : "AUTO");
    hold.setColour (juce::TextButton::textColourOffId, amber);
    bypass.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff784329));
    repaint();
}

juce::String SculptLiveCleanEditor::frequencyText (float hz)
{
    if (! std::isfinite (hz) || hz <= 0.0f) return "—";
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " k" : juce::String (static_cast<int> (std::lround (hz))) + " Hz";
}

void SculptLiveCleanEditor::drawMeter (juce::Graphics& graphics, juce::Rectangle<float> bounds,
                                      float level, const juce::String& label)
{
    const float db = juce::Decibels::gainToDecibels (std::max (0.0f, level), -72.0f);
    const float position = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    graphics.setColour (cream.withAlpha (0.75f));
    graphics.setFont (juce::Font (juce::FontOptions (11.0f)));
    graphics.drawText (label, bounds.withX (20.0f).withWidth (30.0f).expanded (0.0f, 4.0f), juce::Justification::centredLeft);
    graphics.setColour (juce::Colour (0xff272727));
    graphics.fillRoundedRectangle (bounds, 3.0f);
    graphics.setColour (db > 0.0f ? warm : cool);
    graphics.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * position), 3.0f);
    graphics.setColour (cream.withAlpha (0.75f));
    graphics.drawText (db <= -60.0f ? "< −60" : juce::String (db, 1),
                       bounds.withX (555.0f).withWidth (55.0f).expanded (0.0f, 4.0f), juce::Justification::centredRight);
}

void SculptLiveCleanEditor::paint (juce::Graphics& graphics)
{
    graphics.fillAll (juce::Colour (0xff0d0d0d));
    graphics.setColour (cream);
    graphics.setFont (juce::Font (juce::FontOptions (15.0f)));
    graphics.drawText ("S C U L P T   L I V E   C L E A N", 160, 9, 480, 28, juce::Justification::centred);
    graphics.setColour (cream.withAlpha (0.40f));
    graphics.setFont (juce::Font (juce::FontOptions (10.0f)));
    graphics.drawText ("PULIZIA PER IL LIVE", 260, 38, 280, 18, juce::Justification::centred);

    const std::array<const char*, 3> labels { "DENOISE", "DEREVERB", "DEFEEDBACK" };
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        const int x = 20 + static_cast<int> (i) * 260;
        graphics.setColour (juce::Colour (0xff191919));
        graphics.fillRoundedRectangle (static_cast<float> (x), 64.0f, 240.0f, 296.0f, 8.0f);
        graphics.setColour (accents[i]);
        graphics.setFont (juce::Font (juce::FontOptions (13.0f)));
        graphics.drawText (labels[i], x, 76, 240, 20, juce::Justification::centred);
    }
    graphics.setFont (juce::Font (juce::FontOptions (11.0f)));
    graphics.setColour (cream.withAlpha (0.7f));
    const float noiseGr = processor.display.denoiseReductionDb.load (std::memory_order_relaxed);
    const float reverbGr = processor.display.dereverbReductionDb.load (std::memory_order_relaxed);
    graphics.drawText ("RIDUZIONE  " + juce::String (noiseGr, 1) + " dB", 30, 253, 220, 20, juce::Justification::centred);
    graphics.drawText ("RIDUZIONE  " + juce::String (reverbGr, 1) + " dB", 290, 253, 220, 20, juce::Justification::centred);
    graphics.drawText (juce::String (processor.display.activeNotches.load (std::memory_order_relaxed)) + " / 6 FILTRI",
                       550, 253, 220, 20, juce::Justification::centred);

    const bool learning = processor.display.noiseLearning.load (std::memory_order_relaxed);
    const bool ready = processor.display.noiseProfileReady.load (std::memory_order_relaxed);
    graphics.setColour (ready ? cool.withAlpha (0.8f) : cream.withAlpha (0.6f));
    graphics.drawText (learning ? "Apprendimento in corso" : (ready ? "Profilo pronto" : "Profilo da apprendere"),
                       30, 312, 220, 17, juce::Justification::centred);
    graphics.setColour (cream.withAlpha (0.45f));
    graphics.drawText ("2 s di solo rumore", 30, 335, 220, 17, juce::Justification::centred);
    if (learning)
    {
        const float progress = juce::jlimit (0.0f, 1.0f, processor.display.noiseLearnProgress.load (std::memory_order_relaxed));
        graphics.setColour (warm);
        graphics.fillRoundedRectangle (60.0f, 331.0f, 160.0f * progress, 2.0f, 1.0f);
    }
    graphics.setColour (cream.withAlpha (0.55f));
    graphics.drawFittedText ("Riduce le code ambientali\ntra gli attacchi.", 300, 287, 200, 43, juce::Justification::centred, 2);
    graphics.setColour (cream.withAlpha (0.35f));
    graphics.drawText ("Aumenta gradualmente", 300, 335, 200, 17, juce::Justification::centred);

    for (std::size_t i = 0; i < processor.display.feedbackFrequencies.size(); ++i)
    {
        const float hz = processor.display.feedbackFrequencies[i].load (std::memory_order_relaxed);
        const int x = 551 + static_cast<int> (i % 3) * 73;
        const int y = 314 + static_cast<int> (i / 3) * 21;
        graphics.setColour (juce::Colour (0xff242424));
        graphics.fillRoundedRectangle (static_cast<float> (x), static_cast<float> (y), 70.0f, 18.0f, 3.0f);
        graphics.setColour (hz > 0.0f ? cool : cream.withAlpha (0.25f));
        graphics.drawText (frequencyText (hz), x, y, 70, 18, juce::Justification::centred);
    }

    drawMeter (graphics, { 55.0f, 390.0f, 480.0f, 10.0f }, inputLevel, "IN");
    drawMeter (graphics, { 55.0f, 417.0f, 480.0f, 10.0f }, outputLevel, "OUT");
    graphics.setColour (cream.withAlpha (0.65f));
    graphics.setFont (juce::Font (juce::FontOptions (10.0f)));
    graphics.drawText ("OUTPUT dB", 645, 443, 110, 14, juce::Justification::centred);
}
