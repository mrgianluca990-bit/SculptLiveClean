#include "PluginProcessor.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
void near(float got, float expected, const char* text) {
    require(std::abs(got - expected) < 0.001f, text);
}
float value(SculptLiveCleanProcessor& p, const char* id) {
    auto* raw = p.apvts.getRawParameterValue(id);
    require(raw != nullptr, "Missing APVTS parameter");
    return raw->load();
}
void set(SculptLiveCleanProcessor& p, const char* id, float raw) {
    auto* parameter = p.apvts.getParameter(id);
    require(parameter != nullptr, "Missing parameter for write");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(raw));
}
juce::ValueTree snapshot(SculptLiveCleanProcessor& p) {
    juce::MemoryBlock binary;
    p.getStateInformation(binary);
    auto xml = juce::AudioProcessor::getXmlFromBinary(binary.getData(), static_cast<int>(binary.getSize()));
    require(xml != nullptr, "Invalid state XML");
    return juce::ValueTree::fromXml(*xml);
}
void restore(SculptLiveCleanProcessor& p, const juce::ValueTree& tree) {
    auto xml = tree.createXml();
    require(xml != nullptr, "Cannot encode state XML");
    juce::MemoryBlock binary;
    juce::AudioProcessor::copyXmlToBinary(*xml, binary);
    p.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
}
void addLearned(juce::ValueTree& tree) {
    tree.removeChild(tree.getChildWithName("LEARNED"), nullptr);
    juce::ValueTree learned("LEARNED");
    learned.setProperty("sampleRate", 48000.0, nullptr);
    learned.setProperty("noiseReady", true, nullptr);
    learned.setProperty("notchCount", 2, nullptr);
    for (int i = 0; i < 12; ++i)
        learned.setProperty("noise" + juce::String(i), 1.0e-7 * (i + 1), nullptr);
    for (int i = 0; i < 6; ++i)
        learned.setProperty("frequency" + juce::String(i), i == 0 ? 997.0 : (i == 1 ? 1901.0 : 0.0), nullptr);
    tree.addChild(learned, -1, nullptr);
}
void verifyLearned(juce::ValueTree tree) {
    const auto learned = tree.getChildWithName("LEARNED");
    require(learned.isValid() && static_cast<bool>(learned["noiseReady"]), "Lost learned noise readiness");
    require(static_cast<int>(learned["notchCount"]) == 2, "Lost saved notch count");
    near(static_cast<float>(learned["frequency0"]), 997.0f, "Lost notch frequency 0");
    near(static_cast<float>(learned["frequency1"]), 1901.0f, "Lost notch frequency 1");
    for (int i = 0; i < 12; ++i) {
        const float expected = 1.0e-7f * static_cast<float>(i + 1);
        require(std::abs(static_cast<float>(learned["noise" + juce::String(i)]) - expected) < 1.0e-11f,
                "Changed learned noise power");
    }
}
void defaultsAndOrder() {
    SculptLiveCleanProcessor p;
    near(value(p, "sourceEnabled"), 0.0f, "Extractor must default off");
    near(value(p, "sourceThreshold"), -32.0f, "Threshold default changed");
    near(value(p, "sourceDepth"), 50.0f, "Depth default changed");
    const std::array<const char*, 9> expected { "denoise", "dereverb", "defeedback", "output",
        "feedbackHold", "bypass", "sourceThreshold", "sourceDepth", "sourceEnabled" };
    require(p.getParameters().size() == static_cast<int>(expected.size()), "Unexpected parameter count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto* ranged = dynamic_cast<const juce::AudioProcessorParameterWithID*>(p.getParameters()[static_cast<int>(i)]);
        require(ranged != nullptr && ranged->paramID == expected[i], "Existing automation parameter order changed");
    }
}
void recallAndLegacy() {
    SculptLiveCleanProcessor a, b;
    set(a, "sourceThreshold", -27.3f);
    set(a, "sourceDepth", 67.4f);
    set(a, "sourceEnabled", 1.0f);
    set(a, "denoise", 39.1f);
    auto state = snapshot(a);
    addLearned(state);
    restore(b, state);
    near(value(b, "sourceThreshold"), -27.3f, "Threshold recall failed");
    near(value(b, "sourceDepth"), 67.4f, "Depth recall failed");
    near(value(b, "sourceEnabled"), 1.0f, "Enable recall failed");
    verifyLearned(snapshot(b)); // Pending restore, even before prepare/process.
    b.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    audio.clear();
    b.processBlock(audio, midi);
    verifyLearned(snapshot(b));
    // A legacy preset is deliberately loaded into an already-enabled instance.
    auto legacy = state.createCopy();
    for (const char* id : { "sourceThreshold", "sourceDepth", "sourceEnabled" })
        legacy.removeChild(legacy.getChildWithProperty("id", id), nullptr);
    legacy.setProperty("schemaVersion", 1, nullptr);
    restore(b, legacy);
    near(value(b, "sourceEnabled"), 0.0f, "Legacy recall retained enabled extractor");
    near(value(b, "sourceThreshold"), -32.0f, "Legacy recall retained threshold");
    near(value(b, "sourceDepth"), 50.0f, "Legacy recall retained depth");
    near(value(b, "denoise"), 39.1f, "Legacy recall changed preexisting parameter");
    verifyLearned(snapshot(b));
    const auto roundtrip = snapshot(b);
    require(static_cast<int>(roundtrip["schemaVersion"]) == 2, "Schema migration not saved");
    for (const char* id : { "sourceThreshold", "sourceDepth", "sourceEnabled" })
        require(roundtrip.getChildWithProperty("id", id).isValid(), "Migrated source parameter missing");
}
void hostBypassAndMeter() {
    SculptLiveCleanProcessor p;
    for (const char* id : { "denoise", "dereverb", "defeedback" }) set(p, id, 0.0f);
    set(p, "sourceEnabled", 1.0f);
    set(p, "sourceThreshold", -20.0f);
    set(p, "sourceDepth", 100.0f);
    set(p, "output", 6.0f);
    p.prepareToPlay(48000.0, 128);
    require(p.getLatencySamples() == 0, "Source Extractor added buffering latency");
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    const auto fill = [&] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 128; ++i) audio.setSample(c, i, c == 0 ? 0.001f : -0.0005f);
    };
    for (int block = 0; block < 375; ++block) { fill(); p.processBlock(audio, midi); }
    require(p.display.sourceReductionDb.load() > 10.0f, "Wrapper did not expose source gain reduction");
    for (int block = 0; block < 8; ++block) { fill(); p.processBlockBypassed(audio, midi); }
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 128; ++i)
            require(audio.getSample(c, i) == (c == 0 ? 0.001f : -0.0005f), "Host bypass was not exact raw audio");
    near(value(p, "bypass"), 0.0f, "Host bypass modified saved bypass parameter");
    near(value(p, "sourceEnabled"), 1.0f, "Host bypass modified source enable parameter");
}
}
int main() {
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    try {
        defaultsAndOrder();
        std::cout << "PASS: defaults and existing parameter order\n";
        recallAndLegacy();
        std::cout << "PASS: recall, legacy migration and learned state preservation\n";
        hostBypassAndMeter();
        std::cout << "PASS: source meter, zero buffering latency and host bypass\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
