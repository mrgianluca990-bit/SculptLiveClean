#pragma once

#include "FeedbackSuppressor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace sculptliveclean {

struct Parameters {
    float denoise = 0.25f;
    float dereverb = 0.15f;
    float defeedback = 0.5f;
    float outputGain = 1.0f;
    bool feedbackHold = true;
    bool bypass = false;
};

struct Meters {
    float inputPeak = 0.0f;
    float outputPeak = 0.0f;
    float denoiseReductionDb = 0.0f;
    float dereverbReductionDb = 0.0f;
    bool noiseLearning = false;
    bool noiseProfileReady = false;
    float noiseLearnProgress = 0.0f;
    int activeNotches = 0;
    std::array<float, 6> feedbackFrequencies{};
};

// A host may copy this plain state to its project file. Restore runs on the
// audio thread through the wrapper's command handoff, never concurrently.
struct LearnedState {
    std::array<float, 12> noisePower{};
    bool noiseReady = false;
    double sampleRate = 48000.0;
    std::array<float, 6> feedbackFrequencies{};
    int activeNotches = 0;
};

// Causal, light live cleanup, with no audio look-ahead or block delay.
// The residual IIR bank sums to the original sample at unity. The dereverb
// section suppresses decaying late energy; it cannot reconstruct a direct
// signal hidden underneath simultaneous reverberation.
// All methods are audio-thread methods except prepare() before playback.
// The host wrapper publishes its own atomic meter snapshots to the editor.
class LiveCleanEngine {
public:
    static constexpr std::size_t bandCount = 12;

    LiveCleanEngine() { prepare(48000.0, 512, 2); }

    void prepare(double sampleRate, int maximumBlockSize, int channels) {
        (void)maximumBlockSize;
        rate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
            ? sampleRate : 48000.0;
        channelCount_ = std::clamp(channels, 1, 2);
        constexpr std::array<double, bandCount - 1> cutoffs{
            90.0, 160.0, 280.0, 480.0, 780.0, 1250.0,
            2000.0, 3150.0, 4900.0, 7400.0, 11000.0};
        for (std::size_t i = 0; i < cutoffs.size(); ++i)
            lowpassStep_[i] = coefficientHz(std::min(cutoffs[i], rate_ * 0.42));
        meanStep_ = coefficientSeconds(0.025);
        fastAttack_ = coefficientSeconds(0.0015);
        fastRelease_ = coefficientSeconds(0.012);
        slowAttack_ = coefficientSeconds(0.025);
        slowRelease_ = coefficientSeconds(0.18);
        gainOpenStep_ = static_cast<float>(coefficientSeconds(0.0015));
        gainCloseStep_ = static_cast<float>(coefficientSeconds(0.035));
        parameterStep_ = static_cast<float>(coefficientSeconds(0.015));
        bypassStep_ = static_cast<float>(1.0 / (0.005 * rate_));
        guardSamples_ = std::max(1, static_cast<int>(rate_ * 0.025));
        learnTotal_ = std::max(1, static_cast<int>(std::lround(rate_ * 2.0)));
        feedback_.prepare(rate_, channelCount_);
        reset();
    }

    void reset() {
        lowpass_ = {};
        meanPower_ = fastPower_ = slowPower_ = {};
        noisePower_ = noiseAccum_ = {};
        onsetGuard_ = {};
        noiseGain_.fill(1.0f); tailGain_.fill(1.0f);
        noiseTarget_.fill(1.0f); tailTarget_.fill(1.0f);
        current_ = target_;
        wet_ = target_.bypass ? 0.0f : 1.0f;
        learned_ = false; learnRemaining_ = learnCount_ = 0;
        controlCounter_ = 0;
        meters_ = {};
        feedback_.reset();
        feedback_.setAmount(target_.defeedback);
        feedback_.setHold(target_.feedbackHold);
    }

    void setParameters(const Parameters& parameters) {
        setFinite(target_.denoise, parameters.denoise, 0.0f, 1.0f);
        setFinite(target_.dereverb, parameters.dereverb, 0.0f, 1.0f);
        setFinite(target_.defeedback, parameters.defeedback, 0.0f, 1.0f);
        setFinite(target_.outputGain, parameters.outputGain, 0.0f, 4.0f);
        target_.feedbackHold = parameters.feedbackHold;
        target_.bypass = parameters.bypass;
        feedback_.setAmount(target_.defeedback);
        feedback_.setHold(target_.feedbackHold);
    }

    // Capture two seconds of noise-only input. The stored profile is then
    // frozen; music or speech is never silently learned in the background.
    // Relearning disables denoising smoothly during the capture.
    void requestNoiseLearn() {
        noiseAccum_ = {};
        learnRemaining_ = learnTotal_;
        learnCount_ = 0;
        learned_ = false;
        meters_.noiseLearning = true;
        meters_.noiseProfileReady = false;
        meters_.noiseLearnProgress = 0.0f;
    }

    void requestFeedbackReset() { feedback_.requestClear(); }
    int getLatencySamples() const { return 0; }
    const Meters& getMeters() const { return meters_; }

    LearnedState getLearnedState() const {
        LearnedState state;
        state.sampleRate = rate_;
        state.noiseReady = learned_;
        for (std::size_t i = 0; i < bandCount; ++i)
            state.noisePower[i] = checkedFloat(noisePower_[i]);
        state.feedbackFrequencies = feedback_.savedFrequencies();
        state.activeNotches = feedback_.savedCount();
        return state;
    }

    void restoreLearnedState(const LearnedState& state) {
        bool validNoise = state.noiseReady && std::isfinite(state.sampleRate)
            && std::abs(state.sampleRate - rate_) <= 0.5;
        for (float power : state.noisePower)
            validNoise = validNoise && std::isfinite(power) && power >= 0.0f;
        learned_ = validNoise;
        noisePower_ = noiseAccum_ = {};
        learnRemaining_ = learnCount_ = 0;
        if (learned_)
            for (std::size_t i = 0; i < bandCount; ++i)
                noisePower_[i] = std::max(1.0e-18, static_cast<double>(state.noisePower[i]));
        feedback_.restoreFrequencies(state.feedbackFrequencies, state.activeNotches);
        controlCounter_ = 0;
        updateMeters();
    }

    void process(float* const* channels, int channelCount, int count) {
        meters_.inputPeak = meters_.outputPeak = 0.0f;
        if (channels == nullptr || count <= 0 || channelCount < 1 || channels[0] == nullptr) {
            updateMeters();
            return;
        }
        const int activeChannels = channelCount > 1 && channels[1] != nullptr ? 2 : 1;
        for (int frame = 0; frame < count; ++frame) {
            const float rawL = finiteAudio(channels[0][frame]);
            const float rawR = activeChannels == 2 ? finiteAudio(channels[1][frame]) : rawL;
            meters_.inputPeak = std::max(meters_.inputPeak, std::max(std::abs(rawL), std::abs(rawR)));
            smooth(current_.denoise, target_.denoise, parameterStep_);
            smooth(current_.dereverb, target_.dereverb, parameterStep_);
            smooth(current_.outputGain, target_.outputGain, parameterStep_);
            const float wetTarget = target_.bypass ? 0.0f : 1.0f;
            wet_ += std::clamp(wetTarget - wet_, -bypassStep_, bypassStep_);

            std::array<double, bandCount> left{}, right{};
            split(rawL, 0, left);
            if (activeChannels == 2) split(rawR, 1, right);
            else right = left;
            for (std::size_t band = 0; band < bandCount; ++band) {
                const double power = 0.5 * (left[band] * left[band] + right[band] * right[band]);
                meanPower_[band] += meanStep_ * (power - meanPower_[band]);
                fastPower_[band] += (power > fastPower_[band] ? fastAttack_ : fastRelease_)
                    * (power - fastPower_[band]);
                slowPower_[band] += (power > slowPower_[band] ? slowAttack_ : slowRelease_)
                    * (power - slowPower_[band]);
                if (learnRemaining_ > 0) noiseAccum_[band] += power;
                if (onsetGuard_[band] > 0) --onsetGuard_[band];
            }
            if (learnRemaining_ > 0) {
                ++learnCount_;
                if (--learnRemaining_ == 0) {
                    for (std::size_t band = 0; band < bandCount; ++band)
                        noisePower_[band] = std::max(1.0e-18, noiseAccum_[band] / learnCount_);
                    learned_ = true;
                }
            }
            if (controlCounter_ == 0) {
                updateTargets();
                controlCounter_ = controlPeriod_;
            }
            --controlCounter_;

            // Residual form preserves exact input bits when both controls are
            // zero, avoiding a summation-roundoff change in neutral operation.
            double cleanL = rawL, cleanR = rawR;
            for (std::size_t band = 0; band < bandCount; ++band) {
                smooth(noiseGain_[band], noiseTarget_[band],
                    noiseTarget_[band] > noiseGain_[band] ? gainOpenStep_ : gainCloseStep_);
                smooth(tailGain_[band], tailTarget_[band],
                    tailTarget_[band] > tailGain_[band] ? gainOpenStep_ : gainCloseStep_);
                const double change = static_cast<double>(noiseGain_[band]) * tailGain_[band] - 1.0;
                cleanL += change * left[band];
                cleanR += change * right[band];
            }
            if (current_.denoise == 0.0f && current_.dereverb == 0.0f) {
                cleanL = rawL; cleanR = rawR;
            }
            float processedL = checkedFloat(cleanL), processedR = checkedFloat(cleanR);
            feedback_.processSample(processedL, processedR, activeChannels);
            processedL = checkedFloat(static_cast<double>(processedL) * current_.outputGain);
            processedR = checkedFloat(static_cast<double>(processedR) * current_.outputGain);
            const float outL = wet_ == 0.0f ? rawL : (wet_ == 1.0f ? processedL
                : checkedFloat(static_cast<double>(rawL) + wet_ * (static_cast<double>(processedL) - rawL)));
            const float outR = wet_ == 0.0f ? rawR : (wet_ == 1.0f ? processedR
                : checkedFloat(static_cast<double>(rawR) + wet_ * (static_cast<double>(processedR) - rawR)));
            channels[0][frame] = outL;
            if (activeChannels == 2) channels[1][frame] = outR;
            meters_.outputPeak = std::max(meters_.outputPeak, std::max(std::abs(outL), std::abs(outR)));
        }
        updateMeters();
    }

private:
    static constexpr int controlPeriod_ = 16;
    static constexpr double pi_ = 3.14159265358979323846;
    static void setFinite(float& destination, float source, float low, float high) {
        if (std::isfinite(source)) destination = std::clamp(source, low, high);
    }
    static float finiteAudio(float value) { return std::isfinite(value) ? value : 0.0f; }
    static float checkedFloat(double value) {
        if (!std::isfinite(value)) return 0.0f;
        constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
        return static_cast<float>(std::clamp(value, -maximum, maximum));
    }
    static void smooth(float& current, float target, float step) {
        const float delta = target - current;
        const float next = current + step * delta;
        // At high sample rates a float ramp can stall before its endpoint.
        // Snap only at the numerical floor, so returning to unity is exact.
        current = std::abs(delta) < 1.0e-7f || next == current ? target : next;
    }
    double coefficientSeconds(double seconds) const { return 1.0 - std::exp(-1.0 / (rate_ * seconds)); }
    double coefficientHz(double frequency) const { return 1.0 - std::exp(-2.0 * pi_ * frequency / rate_); }
    void split(float input, int channel, std::array<double, bandCount>& result) {
        double previous = 0.0;
        auto& state = lowpass_[static_cast<std::size_t>(channel)];
        for (std::size_t band = 0; band + 1 < bandCount; ++band) {
            state[band] += lowpassStep_[band] * (static_cast<double>(input) - state[band]);
            if (std::abs(state[band]) < 1.0e-30) state[band] = 0.0;
            result[band] = state[band] - previous;
            previous = state[band];
        }
        result[bandCount - 1] = static_cast<double>(input) - previous;
    }
    void updateTargets() {
        const float noiseFloor = std::exp(-1.3815510558f * current_.denoise); // Up to -12 dB.
        const float tailFloor = std::exp(-1.0361632918f * current_.dereverb); // Up to -9 dB.
        for (std::size_t band = 0; band < bandCount; ++band) {
            if (meanPower_[band] < 1.0e-30) meanPower_[band] = 0.0;
            if (fastPower_[band] < 1.0e-30) fastPower_[band] = 0.0;
            if (slowPower_[band] < 1.0e-30) slowPower_[band] = 0.0;
            const double mean = meanPower_[band];
            const double fast = fastPower_[band];
            const double slow = slowPower_[band];
            const double reference = learned_ ? noisePower_[band] : 1.0e-12;
            if (fast > 1.35 * slow && fast > 3.5 * reference && fast > 1.0e-12)
                onsetGuard_[band] = guardSamples_;

            noiseTarget_[band] = 1.0f;
            if (learned_ && current_.denoise > 0.0f && mean > 1.0e-24) {
                const double directFraction = std::clamp(1.0 - 1.25 * noisePower_[band] / mean, 0.0, 1.0);
                const float wiener = static_cast<float>(std::sqrt(directFraction));
                noiseTarget_[band] = noiseFloor + (1.0f - noiseFloor) * wiener;
                if (onsetGuard_[band] > 0 && fast > 6.0 * reference)
                    noiseTarget_[band] = std::max(noiseTarget_[band], 0.97f);
            }

            tailTarget_[band] = 1.0f;
            if (current_.dereverb > 0.0f && slow > 1.0e-18 && onsetGuard_[band] == 0) {
                // Stationary energy keeps a unity target. Only a substantial
                // fast/slow deficit is treated as a late decay, with a soft knee.
                const double deficit = std::clamp((0.78 - fast / slow) / 0.60, 0.0, 1.0);
                const double confidence = deficit * deficit * (3.0 - 2.0 * deficit);
                tailTarget_[band] = 1.0f - (1.0f - tailFloor) * static_cast<float>(confidence);
            }
        }
    }
    void updateMeters() {
        meters_.noiseLearning = learnRemaining_ > 0;
        meters_.noiseProfileReady = learned_;
        meters_.noiseLearnProgress = learned_ ? 1.0f
            : (learnCount_ > 0 ? std::min(1.0f, static_cast<float>(learnCount_) / learnTotal_) : 0.0f);
        double total = 0.0, afterNoise = 0.0, afterTail = 0.0;
        for (std::size_t band = 0; band < bandCount; ++band) {
            const double power = meanPower_[band];
            total += power;
            const double denoised = power * noiseGain_[band] * noiseGain_[band];
            afterNoise += denoised;
            afterTail += denoised * tailGain_[band] * tailGain_[band];
        }
        meters_.denoiseReductionDb = total > 1.0e-20
            ? wet_ * static_cast<float>(std::max(0.0, -10.0 * std::log10(std::max(1.0e-12, afterNoise / total)))) : 0.0f;
        meters_.dereverbReductionDb = afterNoise > 1.0e-20
            ? wet_ * static_cast<float>(std::max(0.0, -10.0 * std::log10(std::max(1.0e-12, afterTail / afterNoise)))) : 0.0f;
        meters_.activeNotches = feedback_.activeCount();
        meters_.feedbackFrequencies = feedback_.frequencies();
    }

    FeedbackSuppressor feedback_;
    Parameters target_{}, current_{};
    Meters meters_{};
    double rate_ = 48000.0;
    int channelCount_ = 2, controlCounter_ = 0, guardSamples_ = 1200;
    int learnTotal_ = 96000, learnRemaining_ = 0, learnCount_ = 0;
    bool learned_ = false;
    float wet_ = 1.0f, bypassStep_ = 1.0f / 240.0f;
    float parameterStep_ = 0.001f, gainOpenStep_ = 0.01f, gainCloseStep_ = 0.001f;
    double meanStep_ = 0.001, fastAttack_ = 0.01, fastRelease_ = 0.001;
    double slowAttack_ = 0.001, slowRelease_ = 0.0001;
    std::array<double, bandCount - 1> lowpassStep_{};
    std::array<std::array<double, bandCount - 1>, 2> lowpass_{};
    std::array<double, bandCount> meanPower_{}, fastPower_{}, slowPower_{}, noisePower_{}, noiseAccum_{};
    std::array<float, bandCount> noiseGain_{}, tailGain_{}, noiseTarget_{}, tailTarget_{};
    std::array<int, bandCount> onsetGuard_{};
};

} // namespace sculptliveclean
