#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <memory>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

static_assert (std::atomic<float>::is_always_lock_free && std::atomic<double>::is_always_lock_free
               && std::atomic<std::uint64_t>::is_always_lock_free,
               "Sculpt Live Clean needs lock-free scalar atomics on its supported 64-bit targets");

void SculptLiveCleanProcessor::AtomicLearnedState::store (const sculptliveclean::LearnedState& value,
                                                        std::uint64_t resetGeneration,
                                                        std::uint64_t learnGeneration,
                                                        std::uint64_t restoreGeneration) noexcept
{
    sequence.fetch_add (1, std::memory_order_acq_rel);
    std::atomic_thread_fence (std::memory_order_release);
    for (std::size_t i = 0; i < noisePower.size(); ++i)
        noisePower[i].store (value.noisePower[i], std::memory_order_relaxed);
    noiseReady.store (value.noiseReady, std::memory_order_relaxed);
    sampleRate.store (value.sampleRate, std::memory_order_relaxed);
    for (std::size_t i = 0; i < frequencies.size(); ++i)
        frequencies[i].store (value.feedbackFrequencies[i], std::memory_order_relaxed);
    notchCount.store (value.activeNotches, std::memory_order_relaxed);
    consumedResetGeneration.store (resetGeneration, std::memory_order_relaxed);
    consumedLearnGeneration.store (learnGeneration, std::memory_order_relaxed);
    consumedRestoreGeneration.store (restoreGeneration, std::memory_order_relaxed);
    sequence.fetch_add (1, std::memory_order_release);
}

bool SculptLiveCleanProcessor::AtomicLearnedState::load (
    sculptliveclean::LearnedState& value, std::uint64_t& version,
    std::uint64_t* resetGeneration, std::uint64_t* learnGeneration,
    std::uint64_t* restoreGeneration) const noexcept
{
    const auto before = sequence.load (std::memory_order_acquire);
    if ((before & 1U) != 0) return false;
    for (std::size_t i = 0; i < noisePower.size(); ++i)
        value.noisePower[i] = noisePower[i].load (std::memory_order_relaxed);
    value.noiseReady = noiseReady.load (std::memory_order_relaxed);
    value.sampleRate = sampleRate.load (std::memory_order_relaxed);
    for (std::size_t i = 0; i < frequencies.size(); ++i)
        value.feedbackFrequencies[i] = frequencies[i].load (std::memory_order_relaxed);
    value.activeNotches = notchCount.load (std::memory_order_relaxed);
    const auto savedReset = consumedResetGeneration.load (std::memory_order_relaxed);
    const auto savedLearn = consumedLearnGeneration.load (std::memory_order_relaxed);
    const auto savedRestore = consumedRestoreGeneration.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    const auto after = sequence.load (std::memory_order_relaxed);
    if (before != after) return false;
    version = after;
    if (resetGeneration != nullptr) *resetGeneration = savedReset;
    if (learnGeneration != nullptr) *learnGeneration = savedLearn;
    if (restoreGeneration != nullptr) *restoreGeneration = savedRestore;
    return true;
}

juce::AudioProcessorValueTreeState::ParameterLayout SculptLiveCleanProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout parameters;
    const auto percent = AudioParameterFloatAttributes().withLabel ("%");
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "denoise", 1 }, "Denoise", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 25.0f, percent));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "dereverb", 1 }, "Dereverb", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 15.0f, percent));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "defeedback", 1 }, "Defeedback", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-12.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    parameters.add (std::make_unique<AudioParameterBool> (ParameterID { "feedbackHold", 1 }, "Feedback Hold", true));
    parameters.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return parameters;
}

SculptLiveCleanProcessor::SculptLiveCleanProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SCULPT_LIVE_CLEAN_STATE", createLayout())
{
    denoiseParameter = apvts.getRawParameterValue ("denoise");
    dereverbParameter = apvts.getRawParameterValue ("dereverb");
    defeedbackParameter = apvts.getRawParameterValue ("defeedback");
    outputParameter = apvts.getRawParameterValue ("output");
    holdParameter = apvts.getRawParameterValue ("feedbackHold");
    bypassParameter = apvts.getRawParameterValue ("bypass");
    clearDisplay();
    setLatencySamples (0);
}

bool SculptLiveCleanProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto input = layout.getMainInputChannelSet();
    return (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo())
        && layout.getMainOutputChannelSet() == input;
}

sculptliveclean::Parameters SculptLiveCleanProcessor::readParameters (bool hostBypass) const noexcept
{
    sculptliveclean::Parameters result;
    result.denoise = denoiseParameter->load (std::memory_order_relaxed) * 0.01f;
    result.dereverb = dereverbParameter->load (std::memory_order_relaxed) * 0.01f;
    result.defeedback = defeedbackParameter->load (std::memory_order_relaxed) * 0.01f;
    result.outputGain = juce::Decibels::decibelsToGain (outputParameter->load (std::memory_order_relaxed));
    result.feedbackHold = holdParameter->load (std::memory_order_relaxed) >= 0.5f;
    result.bypass = hostBypass || bypassParameter->load (std::memory_order_relaxed) >= 0.5f;
    return result;
}

void SculptLiveCleanProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    const auto previous = engine.getLearnedState();
    engine.setParameters (readParameters (false));
    engine.prepare (sampleRate, std::max (1, maximumBlockSize), getTotalNumOutputChannels());
    if (! applyPendingLearnedState()) engine.restoreLearnedState (previous);
    consumeCommands();
    setLatencySamples (engine.getLatencySamples());
    clearDisplay();
    publishMeters();
    publishLearnedState();
}

void SculptLiveCleanProcessor::releaseResources()
{
    const auto previous = engine.getLearnedState();
    engine.reset();
    engine.restoreLearnedState (previous);
    clearDisplay();
    publishMeters();
    publishLearnedState();
}

bool SculptLiveCleanProcessor::applyPendingLearnedState() noexcept
{
    const auto candidateVersion = pendingLearnedState.sequence.load (std::memory_order_acquire);
    if (candidateVersion == 0 || candidateVersion == appliedRestoreVersion || (candidateVersion & 1U) != 0)
        return false;
    sculptliveclean::LearnedState incoming;
    std::uint64_t version = 0;
    std::uint64_t resetAtRestore = 0, learnAtRestore = 0;
    if (! pendingLearnedState.load (incoming, version, &resetAtRestore, &learnAtRestore)
        || version == 0 || version == appliedRestoreVersion)
        return false;
    engine.restoreLearnedState (incoming);
    // A later preset load supersedes earlier pending UI commands. Requests
    // made after that load still differ and are consumed immediately below.
    consumedResetGeneration = resetAtRestore;
    consumedLearnGeneration = learnAtRestore;
    appliedRestoreVersion = version;
    return true;
}

void SculptLiveCleanProcessor::publishLearnedState() noexcept
{
    // Command and restore acknowledgements travel inside the same snapshot.
    // A save during processing cannot pair old filters with a newer ack.
    // Publish into the inactive slot so a completed snapshot remains readable
    // even if this audio thread is preempted part-way through publication.
    const auto next = completedPublishedSlot.load (std::memory_order_relaxed) ^ 1U;
    publishedLearnedStates[next].store (engine.getLearnedState(), consumedResetGeneration,
                                       consumedLearnGeneration, appliedRestoreVersion);
    completedPublishedSlot.store (next, std::memory_order_release);
}

void SculptLiveCleanProcessor::consumeCommands() noexcept
{
    const auto learn = learnRequestGeneration.load (std::memory_order_relaxed);
    if (learn != consumedLearnGeneration)
    {
        engine.requestNoiseLearn();
        consumedLearnGeneration = learn;
    }
    const auto reset = resetRequestGeneration.load (std::memory_order_relaxed);
    if (reset != consumedResetGeneration)
    {
        engine.requestFeedbackReset();
        consumedResetGeneration = reset;
    }
}

void SculptLiveCleanProcessor::clearDisplay() noexcept
{
    display.inputPeak.store (0.0f, std::memory_order_relaxed);
    display.outputPeak.store (0.0f, std::memory_order_relaxed);
    display.denoiseReductionDb.store (0.0f, std::memory_order_relaxed);
    display.dereverbReductionDb.store (0.0f, std::memory_order_relaxed);
    display.noiseLearnProgress.store (0.0f, std::memory_order_relaxed);
    display.noiseLearning.store (false, std::memory_order_relaxed);
    display.noiseProfileReady.store (false, std::memory_order_relaxed);
    display.activeNotches.store (0, std::memory_order_relaxed);
    for (auto& frequency : display.feedbackFrequencies) frequency.store (0.0f, std::memory_order_relaxed);
}

void SculptLiveCleanProcessor::publishMeters() noexcept
{
    const auto meters = engine.getMeters();
    display.inputPeak.store (meters.inputPeak, std::memory_order_relaxed);
    display.outputPeak.store (meters.outputPeak, std::memory_order_relaxed);
    display.denoiseReductionDb.store (meters.denoiseReductionDb, std::memory_order_relaxed);
    display.dereverbReductionDb.store (meters.dereverbReductionDb, std::memory_order_relaxed);
    display.noiseLearnProgress.store (meters.noiseLearnProgress, std::memory_order_relaxed);
    display.noiseLearning.store (meters.noiseLearning, std::memory_order_relaxed);
    display.noiseProfileReady.store (meters.noiseProfileReady, std::memory_order_relaxed);
    display.activeNotches.store (meters.activeNotches, std::memory_order_relaxed);
    for (std::size_t i = 0; i < display.feedbackFrequencies.size(); ++i)
        display.feedbackFrequencies[i].store (meters.feedbackFrequencies[i], std::memory_order_relaxed);
}

void SculptLiveCleanProcessor::process (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi, bool hostBypass)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    for (int channel = getTotalNumInputChannels(); channel < audio.getNumChannels(); ++channel)
        audio.clear (channel, 0, audio.getNumSamples());

    const int channels = std::min ({2, audio.getNumChannels(), getTotalNumOutputChannels()});
    if (channels <= 0)
        return;

    engine.setParameters (readParameters (hostBypass));
    (void) applyPendingLearnedState();
    // UI commands are consumed here; the editor never touches mutable DSP state.
    consumeCommands();
    engine.process (audio.getArrayOfWritePointers(), channels, audio.getNumSamples());
    publishMeters();
    publishLearnedState();
}

void SculptLiveCleanProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    process (audio, midi, false);
}

void SculptLiveCleanProcessor::processBlockBypassed (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    // Keep analysis and filters warm; the core fades to the raw, undelayed input.
    process (audio, midi, true);
}

juce::AudioProcessorParameter* SculptLiveCleanProcessor::getBypassParameter() const
{
    return apvts.getParameter ("bypass");
}

juce::AudioProcessorEditor* SculptLiveCleanProcessor::createEditor()
{
    return new SculptLiveCleanEditor (*this);
}

void SculptLiveCleanProcessor::getStateInformation (juce::MemoryBlock& data)
{
    const std::lock_guard<std::mutex> stateGuard (stateIoMutex);
    sculptliveclean::LearnedState captured;
    std::uint64_t version = 0;
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const auto slot = completedPublishedSlot.load (std::memory_order_acquire);
        if (publishedLearnedStates[slot].load (captured, version,
                &lastSerializedResetGeneration, &lastSerializedLearnGeneration,
                &lastSerializedRestoreGeneration))
        {
            lastSerializedLearnedState = captured;
            break;
        }
    }

    // Preserve a state just loaded by the host even before its first block.
    std::uint64_t resetAtRestore = 0, learnAtRestore = 0;
    if (pendingLearnedState.load (captured, version, &resetAtRestore, &learnAtRestore) && version != 0
        && version != lastSerializedRestoreGeneration)
    {
        lastSerializedLearnedState = captured;
        lastSerializedResetGeneration = resetAtRestore;
        lastSerializedLearnGeneration = learnAtRestore;
    }

    // A UI command must be saved even while the host is stopped, or after
    // process() consumed it but before the resulting state was published.
    // The acknowledgement belongs to the same seqlock snapshot as its data.
    if (resetRequestGeneration.load (std::memory_order_relaxed) != lastSerializedResetGeneration)
    {
        lastSerializedLearnedState.activeNotches = 0;
        lastSerializedLearnedState.feedbackFrequencies.fill (0.0f);
    }
    if (learnRequestGeneration.load (std::memory_order_relaxed) != lastSerializedLearnGeneration)
    {
        lastSerializedLearnedState.noiseReady = false;
        lastSerializedLearnedState.noisePower.fill (0.0f);
    }

    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", 1, nullptr);
    juce::ValueTree learned ("LEARNED");
    learned.setProperty ("sampleRate", lastSerializedLearnedState.sampleRate, nullptr);
    learned.setProperty ("noiseReady", lastSerializedLearnedState.noiseReady, nullptr);
    learned.setProperty ("notchCount", lastSerializedLearnedState.activeNotches, nullptr);
    for (std::size_t i = 0; i < lastSerializedLearnedState.noisePower.size(); ++i)
        learned.setProperty ("noise" + juce::String (static_cast<int> (i)),
                             static_cast<double> (lastSerializedLearnedState.noisePower[i]), nullptr);
    for (std::size_t i = 0; i < lastSerializedLearnedState.feedbackFrequencies.size(); ++i)
        learned.setProperty ("frequency" + juce::String (static_cast<int> (i)),
                             static_cast<double> (lastSerializedLearnedState.feedbackFrequencies[i]), nullptr);
    state.addChild (learned, -1, nullptr);
    if (const auto xml = state.createXml()) copyXmlToBinary (*xml, data);
}

void SculptLiveCleanProcessor::setStateInformation (const void* data, int size)
{
    const std::lock_guard<std::mutex> stateGuard (stateIoMutex);
    if (const auto xml = getXmlFromBinary (data, size))
    {
        if (! xml->hasTagName (apvts.state.getType())) return;
        auto state = juce::ValueTree::fromXml (*xml);
        if (! state.isValid()) return;
        sculptliveclean::LearnedState restored;
        if (const auto learned = state.getChildWithName ("LEARNED"); learned.isValid())
        {
            restored.sampleRate = static_cast<double> (learned["sampleRate"]);
            restored.noiseReady = static_cast<bool> (learned["noiseReady"]);
            restored.activeNotches = juce::jlimit (0, 6, static_cast<int> (learned["notchCount"]));
            for (std::size_t i = 0; i < restored.noisePower.size(); ++i)
            {
                const juce::Identifier key ("noise" + juce::String (static_cast<int> (i)));
                const float power = static_cast<float> (learned[key]);
                if (! learned.hasProperty (key) || ! std::isfinite (power) || power < 0.0f)
                    restored.noiseReady = false;
                restored.noisePower[i] = std::isfinite (power) && power >= 0.0f ? power : 0.0f;
            }
            if (! std::isfinite (restored.sampleRate) || restored.sampleRate < 8000.0 || restored.sampleRate > 384000.0)
            {
                restored.noiseReady = false;
                restored.sampleRate = 48000.0;
            }
            for (std::size_t i = 0; i < restored.feedbackFrequencies.size(); ++i)
            {
                const float hz = static_cast<float> (learned["frequency" + juce::String (static_cast<int> (i))]);
                restored.feedbackFrequencies[i] = std::isfinite (hz) && hz > 0.0f && hz < 192000.0f ? hz : 0.0f;
            }
            state.removeChild (learned, nullptr);
        }
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            auto child = state.getChild (i);
            if (auto* parameter = apvts.getParameter (child["id"].toString()))
            {
                const float value = static_cast<float> (child["value"]);
                if (! std::isfinite (value)) return;
                const auto& range = parameter->getNormalisableRange();
                child.setProperty ("value", range.snapToLegalValue (juce::jlimit (range.start, range.end, value)), nullptr);
            }
        }
        apvts.replaceState (state);
        lastSerializedLearnedState = restored;
        pendingLearnedState.store (restored,
            resetRequestGeneration.load (std::memory_order_relaxed),
            learnRequestGeneration.load (std::memory_order_relaxed));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SculptLiveCleanProcessor();
}
