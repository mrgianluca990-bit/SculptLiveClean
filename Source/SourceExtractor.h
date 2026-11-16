#pragma once

#include <algorithm>
#include <cmath>

namespace sculptliveclean {

// Source Enhancer from the user's Sculpt CH Strip, ported to a small standalone
// processor. Reference: mrgianluca990-bit/SculptDAW, commit
// 081a7b851b9ebb41fa8c53d7f2335f0abe007b36,
// crates/sculpt-strip/src/processor.rs (gate section and compress_gr), and
// crates/sculpt-strip/src/util.rs (time_coef).
// Original Sculpt processing; soft-knee gain computer adapted from SoundCraft,
// ArtCraft Team, MIT/Apache-2.0. See the accompanying third-party notice.
//
// This is a linked downward expander for quieter spill between phrases. It does
// not separate simultaneously overlapping sources. The original gain computer
// uses slope 1 - 1/4 = 0.75 below a 6 dB knee (not a conventional 3x expansion
// slope), with 5 ms detector decay, 60 ms hold, 2 ms opening and 180 ms closing.
// The four fixed time coefficients are calculated only in prepare().
class SourceExtractor {
public:
    SourceExtractor() noexcept { prepare(48000.0); }

    void prepare(double sampleRate) noexcept {
        rate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
            ? sampleRate : 48000.0;
        parameterCoefficient_ = timeCoefficient(0.020);
        envelopeCoefficient_ = timeCoefficient(0.005);
        openCoefficient_ = timeCoefficient(0.002);
        closeCoefficient_ = timeCoefficient(0.180);
        holdSamples_ = std::max(1, static_cast<int>(0.060 * rate_));
        reset();
    }

    void reset() noexcept {
        threshold_ = targetThreshold_;
        depth_ = targetDepth_;
        enabledMix_ = enabled_ ? 1.0 : 0.0;
        envelope_ = reduction_ = appliedReduction_ = 0.0;
        holdRemaining_ = 0;
    }

    void setParameters(float thresholdDb, float depthNormalized, bool enabled) noexcept {
        if (std::isfinite(thresholdDb)) targetThreshold_ = std::clamp<double>(thresholdDb, -60.0, 0.0);
        if (std::isfinite(depthNormalized)) targetDepth_ = std::clamp<double>(depthNormalized, 0.0, 1.0);
        enabled_ = enabled;
    }

    // The detector reads the original input. Applying the resulting gain after
    // denoise/feedback avoids moving the threshold when those controls change.
    // One shared envelope/gain preserves stereo position, including antiphase.
    void processSample(float& left, float& right, float detectorLeft, float detectorRight,
                       int channels) noexcept {
        left = finiteSample(left);
        right = finiteSample(right);
        smooth(threshold_, targetThreshold_);
        smooth(depth_, targetDepth_);
        smooth(enabledMix_, enabled_ ? 1.0 : 0.0);

        // Sleep once either transition has reached neutral. Resetting dormant
        // history also prevents an old held attenuation when enabled again.
        if (enabledMix_ == 0.0 || depth_ == 0.0) {
            envelope_ = reduction_ = appliedReduction_ = 0.0;
            holdRemaining_ = 0;
            return;
        }

        const double peak = std::max(std::abs(static_cast<double>(finiteSample(detectorLeft))),
            channels > 1 ? std::abs(static_cast<double>(finiteSample(detectorRight))) : 0.0);
        envelope_ = std::max(peak, envelope_ * envelopeCoefficient_);
        if (envelope_ < 1.0e-30) envelope_ = 0.0;
        const double levelDb = 20.0 * std::log10(std::max(1.0e-12, envelope_));
        const double below = threshold_ - levelDb;
        double targetReduction = 0.0;
        if (std::abs(below) <= 3.0) {
            const double kneePosition = below + 3.0;
            targetReduction = 0.75 * kneePosition * kneePosition / 12.0;
        } else if (below > 0.0) {
            targetReduction = 0.75 * below;
        }
        targetReduction = std::min(targetReduction, depth_ * 48.0);
        if (levelDb > threshold_ + 2.0) holdRemaining_ = holdSamples_;
        if (targetReduction > reduction_ && holdRemaining_ > 0) {
            --holdRemaining_;
            targetReduction = reduction_;
        }
        const double coefficient = targetReduction < reduction_ ? openCoefficient_ : closeCoefficient_;
        reduction_ = targetReduction + (reduction_ - targetReduction) * coefficient;
        if (reduction_ < 1.0e-30) reduction_ = 0.0;
        appliedReduction_ = reduction_ * enabledMix_;
        const double gain = std::pow(10.0, -appliedReduction_ / 20.0);
        left = static_cast<float>(static_cast<double>(left) * gain);
        right = static_cast<float>(static_cast<double>(right) * gain);
    }

    float getReductionDb() const noexcept { return static_cast<float>(appliedReduction_); }

private:
    static float finiteSample(float value) noexcept { return std::isfinite(value) ? value : 0.0f; }
    double timeCoefficient(double seconds) const noexcept { return std::exp(-1.0 / (seconds * rate_)); }
    void smooth(double& current, double target) const noexcept {
        current = target + (current - target) * parameterCoefficient_;
        if (std::abs(current - target) < 1.0e-9 * (1.0 + std::abs(target))) current = target;
    }

    double rate_ = 48000.0;
    double targetThreshold_ = -32.0, targetDepth_ = 0.5;
    double threshold_ = -32.0, depth_ = 0.5, enabledMix_ = 0.0;
    double envelope_ = 0.0, reduction_ = 0.0, appliedReduction_ = 0.0;
    double parameterCoefficient_ = 0.0, envelopeCoefficient_ = 0.0;
    double openCoefficient_ = 0.0, closeCoefficient_ = 0.0;
    int holdSamples_ = 2880, holdRemaining_ = 0;
    bool enabled_ = false;
};

} // namespace sculptliveclean
