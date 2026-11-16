#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "LiveCleanEngine.h"

class SculptLiveCleanProcessor final : public juce::AudioProcessor
{
public:
    SculptLiveCleanProcessor();
    ~SculptLiveCleanProcessor() override = default;

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Sculpt Live Clean"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    void requestNoiseLearn() noexcept { learnRequestGeneration.fetch_add (1, std::memory_order_relaxed); }
    void requestFeedbackReset() noexcept { resetRequestGeneration.fetch_add (1, std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    struct DisplayState
    {
        std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
        std::atomic<float> denoiseReductionDb { 0.0f }, dereverbReductionDb { 0.0f };
        std::atomic<float> sourceReductionDb { 0.0f };
        std::atomic<float> noiseLearnProgress { 0.0f };
        std::atomic<bool> noiseLearning { false }, noiseProfileReady { false };
        std::atomic<int> activeNotches { 0 };
        std::array<std::atomic<float>, 6> feedbackFrequencies {};
    } display;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    sculptliveclean::Parameters readParameters (bool hostBypass) const noexcept;
    void process (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool hostBypass);
    void publishMeters() noexcept;
    void clearDisplay() noexcept;
    bool applyPendingLearnedState() noexcept;
    void publishLearnedState() noexcept;
    void consumeCommands() noexcept;

    // Every field is atomic: there are no concurrent plain-POD reads/writes.
    // A writer is serialized outside the audio callback; the audio reader
    // takes one bounded snapshot attempt and retries on the next block.
    struct AtomicLearnedState
    {
        AtomicLearnedState() noexcept
        {
            for (auto& value : noisePower) value.store (0.0f, std::memory_order_relaxed);
            for (auto& value : frequencies) value.store (0.0f, std::memory_order_relaxed);
        }
        void store (const sculptliveclean::LearnedState&, std::uint64_t resetGeneration = 0,
                    std::uint64_t learnGeneration = 0, std::uint64_t restoreGeneration = 0) noexcept;
        bool load (sculptliveclean::LearnedState&, std::uint64_t& version,
                   std::uint64_t* resetGeneration = nullptr, std::uint64_t* learnGeneration = nullptr,
                   std::uint64_t* restoreGeneration = nullptr) const noexcept;
        std::atomic<std::uint64_t> sequence { 0 };
        std::array<std::atomic<float>, 12> noisePower {};
        std::atomic<bool> noiseReady { false };
        std::atomic<double> sampleRate { 48000.0 };
        std::array<std::atomic<float>, 6> frequencies {};
        std::atomic<int> notchCount { 0 };
        std::atomic<std::uint64_t> consumedResetGeneration { 0 }, consumedLearnGeneration { 0 };
        std::atomic<std::uint64_t> consumedRestoreGeneration { 0 };
    };
    std::array<AtomicLearnedState, 2> publishedLearnedStates {};
    std::atomic<unsigned> completedPublishedSlot { 0 };
    AtomicLearnedState pendingLearnedState;

    std::uint64_t appliedRestoreVersion = 0; // Audio/lifecycle thread only.
    std::mutex stateIoMutex; // State XML callbacks only; never used by process().
    sculptliveclean::LearnedState lastSerializedLearnedState;
    std::uint64_t lastSerializedResetGeneration = 0, lastSerializedLearnGeneration = 0;
    std::uint64_t lastSerializedRestoreGeneration = 0;

    sculptliveclean::LiveCleanEngine engine;
    std::atomic<float>* denoiseParameter = nullptr;
    std::atomic<float>* dereverbParameter = nullptr;
    std::atomic<float>* defeedbackParameter = nullptr;
    std::atomic<float>* outputParameter = nullptr;
    std::atomic<float>* holdParameter = nullptr;
    std::atomic<float>* bypassParameter = nullptr;
    std::atomic<float>* sourceThresholdParameter = nullptr;
    std::atomic<float>* sourceDepthParameter = nullptr;
    std::atomic<float>* sourceEnabledParameter = nullptr;
    std::atomic<std::uint64_t> learnRequestGeneration { 0 }, resetRequestGeneration { 0 };
    std::uint64_t consumedLearnGeneration = 0, consumedResetGeneration = 0; // Audio/lifecycle thread only.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptLiveCleanProcessor)
};
