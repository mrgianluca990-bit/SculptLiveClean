#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sculptliveclean {

// Detection-only FFT. The audio itself is never buffered or transformed.
// Automatic detection is intentionally conservative: a sustained musical sine
// can still be indistinguishable from feedback without an external reference.
// Learn during soundcheck, then Hold the six cut-only filters during the show.
class FeedbackSuppressor {
public:
    FeedbackSuppressor() noexcept
        : instancePhase_(instanceCounter_.fetch_add(137U, std::memory_order_relaxed)
                         % static_cast<unsigned>(hopSize_)) {}

    void prepare(double sampleRate, int channels) {
        sampleRate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0
            && sampleRate <= 384000.0 ? sampleRate : 48000.0;
        channels_ = std::clamp(channels, 1, 2);
        decimation_ = 1;
        while (sampleRate_ / static_cast<double>(decimation_) > 52000.0) decimation_ *= 2;
        analysisRate_ = sampleRate_ / static_cast<double>(decimation_);
        fadeStep_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.025 * sampleRate_)));
        requiredFrames_ = std::max(2, static_cast<int>(std::ceil(0.28 * analysisRate_ / hopSize_)));
        minimumBin_ = std::max(2, static_cast<int>(std::floor(120.0 * fftSize_ / analysisRate_)));
        maximumBin_ = std::min(static_cast<int>(fftSize_ / 2) - 12,
            static_cast<int>(std::floor(std::min(12000.0, analysisRate_ * 0.42) * fftSize_ / analysisRate_)));
        windowSum_ = 0.0f;
        for (std::size_t i = 0; i < fftSize_; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi_ * i / (fftSize_ - 1)));
            windowSum_ += window_[i];
            unsigned value = static_cast<unsigned>(i), reverse = 0;
            for (int bit = 0; bit < fftOrder_; ++bit) { reverse = (reverse << 1) | (value & 1U); value >>= 1; }
            reversed_[i] = static_cast<std::uint16_t>(reverse);
        }
        for (std::size_t i = 0; i < fftSize_ / 2; ++i) {
            const double angle = -2.0 * pi_ * static_cast<double>(i) / fftSize_;
            twiddleReal_[i] = static_cast<float>(std::cos(angle));
            twiddleImag_[i] = static_cast<float>(std::sin(angle));
        }
        if (decimation_ > 1) {
            makeLowpass(antiAliasCoefficients_[0], analysisRate_ * 0.25, 0.541196100146197);
            makeLowpass(antiAliasCoefficients_[1], analysisRate_ * 0.25, 1.306562964876377);
        }
        reset();
    }

    void reset() noexcept {
        for (auto& slot : slots_) slot = Slot{};
        for (auto& channel : antiAliasState_) for (auto& state : channel) state = FilterState{};
        historyLeft_.fill(0.0f); historyRight_.fill(0.0f);
        writePosition_ = filled_ = 0;
        hopCounter_ = static_cast<int>(instancePhase_);
        decimationCounter_ = 0;
        candidateFrames_ = 0; candidateFrequency_ = 0.0;
        previousBin_ = -1; previousChannel_ = 0; previousPhase_ = 0.0;
        analysisFrames_ = 0;
        restorePending_ = false; pendingRestoreCount_ = 0;
    }

    void setAmount(float amount) noexcept {
        if (!std::isfinite(amount)) return;
        amount = std::clamp(amount, 0.0f, 1.0f);
        if (amount == amount_) return;
        amount_ = amount;
        depthTarget_ = amount == 0.0f ? 0.0f
            : 1.0f - std::pow(10.0f, -12.0f * amount / 20.0f);
        if (amount == 0.0f) { candidateFrames_ = 0; previousBin_ = -1; }
    }

    void setHold(bool hold) noexcept {
        if (hold != hold_) { candidateFrames_ = 0; previousBin_ = -1; }
        hold_ = hold;
    }

    void requestClear() noexcept {
        for (auto& slot : slots_) if (slot.active) slot.clearing = true;
        candidateFrames_ = 0; candidateFrequency_ = 0.0; previousBin_ = -1;
        restorePending_ = false; pendingRestoreCount_ = 0;
    }

    // Audio-thread handoff for a DAW state restore. Existing filters fade out
    // before replacement; restored filters start at dry and fade in. No locks.
    void restoreFrequencies(const std::array<float, 6>& frequencies, int count) noexcept {
        requestClear();
        const int requested = std::clamp(count, 0, 6);
        for (float value : frequencies) {
            if (pendingRestoreCount_ >= requested) break;
            if (!std::isfinite(value) || value < 120.0f
                || value > static_cast<float>(std::min(12000.0, analysisRate_ * 0.42))) continue;
            bool duplicate = false;
            for (int i = 0; i < pendingRestoreCount_; ++i)
                duplicate = duplicate || std::abs(value - pendingRestore_[static_cast<std::size_t>(i)]) < 1.0f;
            if (!duplicate) pendingRestore_[static_cast<std::size_t>(pendingRestoreCount_++)] = value;
        }
        restorePending_ = pendingRestoreCount_ > 0;
        if (restorePending_ && activeCount() == 0) applyPendingRestore();
    }

    void processSample(float& left, float& right, int channels) noexcept {
        const int count = std::clamp(channels, 1, channels_);
        const float inputLeft = std::isfinite(left) ? left : 0.0f;
        const float inputRight = count == 2 && std::isfinite(right) ? right : inputLeft;
        if (restorePending_ && activeCount() == 0) applyPendingRestore();
        feedAnalysis(inputLeft, inputRight, count);
        left = inputLeft;
        if (count == 2) right = inputRight;
        for (auto& slot : slots_) {
            if (!slot.active) continue;
            const float target = slot.clearing ? 0.0f : depthTarget_;
            const float difference = target - slot.mix;
            slot.mix = std::abs(difference) < 1.0e-7f ? target : slot.mix + fadeStep_ * difference;
            if (slot.clearing && slot.mix == 0.0f) { slot = Slot{}; continue; }
            const float filteredLeft = filter(slot.coefficients, slot.state[0], left);
            if (slot.mix != 0.0f) left += slot.mix * (filteredLeft - left);
            if (count == 2) {
                const float filteredRight = filter(slot.coefficients, slot.state[1], right);
                if (slot.mix != 0.0f) right += slot.mix * (filteredRight - right);
            }
        }
    }

    int activeCount() const noexcept {
        int count = 0;
        for (const auto& slot : slots_) count += slot.active ? 1 : 0;
        return count;
    }

    std::array<float, 6> frequencies() const noexcept {
        std::array<float, 6> result{};
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].active) result[i] = static_cast<float>(slots_[i].frequency);
        return result;
    }

    int savedCount() const noexcept {
        if (restorePending_) return pendingRestoreCount_;
        int count = 0;
        for (const auto& slot : slots_) count += slot.active && !slot.clearing ? 1 : 0;
        return count;
    }

    std::array<float, 6> savedFrequencies() const noexcept {
        std::array<float, 6> result{};
        int count = 0;
        if (restorePending_) {
            for (int i = 0; i < pendingRestoreCount_; ++i)
                result[static_cast<std::size_t>(i)] = pendingRestore_[static_cast<std::size_t>(i)];
        } else {
            for (const auto& slot : slots_) if (slot.active && !slot.clearing)
                result[static_cast<std::size_t>(count++)] = static_cast<float>(slot.frequency);
        }
        return result;
    }

    bool isHold() const noexcept { return hold_; }
    std::uint64_t analysisFrames() const noexcept { return analysisFrames_; }

private:
    static constexpr double pi_ = 3.1415926535897932384626433832795;
    static constexpr int fftOrder_ = 11;
    static constexpr std::size_t fftSize_ = std::size_t{1} << fftOrder_;
    static constexpr int hopSize_ = 512;
    static constexpr double notchQ_ = 48.0;
    struct Coefficients { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
    struct FilterState { float z1 = 0.0f, z2 = 0.0f; };
    struct Slot {
        bool active = false, clearing = false;
        double frequency = 0.0;
        float mix = 0.0f;
        Coefficients coefficients{};
        std::array<FilterState, 2> state{};
    };

    static float filter(const Coefficients& c, FilterState& s, float input) noexcept {
        const float output = c.b0 * input + s.z1;
        s.z1 = c.b1 * input - c.a1 * output + s.z2;
        s.z2 = c.b2 * input - c.a2 * output;
        if (std::abs(s.z1) < 1.0e-20f) s.z1 = 0.0f;
        if (std::abs(s.z2) < 1.0e-20f) s.z2 = 0.0f;
        return output;
    }

    void makeLowpass(Coefficients& c, double frequency, double q) const noexcept {
        const double w = 2.0 * pi_ * frequency / sampleRate_;
        const double cosine = std::cos(w), alpha = std::sin(w) / (2.0 * q);
        const double scale = 1.0 / (1.0 + alpha);
        c.b0 = static_cast<float>(0.5 * (1.0 - cosine) * scale);
        c.b1 = static_cast<float>((1.0 - cosine) * scale); c.b2 = c.b0;
        c.a1 = static_cast<float>(-2.0 * cosine * scale);
        c.a2 = static_cast<float>((1.0 - alpha) * scale);
    }

    void install(double frequency) noexcept {
        for (auto& slot : slots_) if (!slot.active) {
            slot = Slot{};
            slot.frequency = std::clamp(frequency, 120.0, std::min(12000.0, analysisRate_ * 0.42));
            const double omega = 2.0 * pi_ * slot.frequency / sampleRate_;
            const double alpha = std::sin(omega) / (2.0 * notchQ_);
            const double scale = 1.0 / (1.0 + alpha);
            // RBJ notch coefficients; a dry/notch blend sets finite cut depth.
            // Reference: https://www.w3.org/TR/audio-eq-cookbook/
            slot.coefficients.b0 = static_cast<float>(scale);
            slot.coefficients.b1 = static_cast<float>(-2.0 * std::cos(omega) * scale);
            slot.coefficients.b2 = slot.coefficients.b0;
            slot.coefficients.a1 = slot.coefficients.b1;
            slot.coefficients.a2 = static_cast<float>((1.0 - alpha) * scale);
            slot.active = true;
            return;
        }
    }

    bool occupied(double frequency) const noexcept {
        for (const auto& slot : slots_) if (slot.active) {
            const double tolerance = std::max(2.5 * analysisRate_ / fftSize_, slot.frequency / notchQ_ * 2.0);
            if (std::abs(frequency - slot.frequency) < tolerance) return true;
        }
        return false;
    }

    void applyPendingRestore() noexcept {
        for (int i = 0; i < pendingRestoreCount_; ++i)
            install(pendingRestore_[static_cast<std::size_t>(i)]);
        restorePending_ = false; pendingRestoreCount_ = 0;
    }

    void feedAnalysis(float left, float right, int channels) noexcept {
        if (decimation_ > 1) {
            for (std::size_t stage = 0; stage < 2; ++stage) {
                left = filter(antiAliasCoefficients_[stage], antiAliasState_[0][stage], left);
                if (channels == 2) right = filter(antiAliasCoefficients_[stage], antiAliasState_[1][stage], right);
            }
        }
        if (++decimationCounter_ < decimation_) return;
        decimationCounter_ = 0;
        historyLeft_[writePosition_] = left;
        historyRight_[writePosition_] = channels == 2 ? right : 0.0f;
        writePosition_ = (writePosition_ + 1) & (fftSize_ - 1);
        filled_ = std::min(filled_ + 1, fftSize_);
        if (++hopCounter_ < hopSize_) return;
        hopCounter_ = 0;
        if (filled_ < fftSize_ || hold_ || restorePending_ || amount_ == 0.0f || activeCount() == 6) return;
        analyse(channels);
    }

    void transform() noexcept {
        for (std::size_t size = 2; size <= fftSize_; size <<= 1) {
            const std::size_t half = size / 2, stride = fftSize_ / size;
            for (std::size_t base = 0; base < fftSize_; base += size) {
                for (std::size_t k = 0; k < half; ++k) {
                    const std::size_t a = base + k, b = a + half, t = k * stride;
                    const float real = twiddleReal_[t] * fftReal_[b] - twiddleImag_[t] * fftImag_[b];
                    const float imag = twiddleReal_[t] * fftImag_[b] + twiddleImag_[t] * fftReal_[b];
                    fftReal_[b] = fftReal_[a] - real; fftImag_[b] = fftImag_[a] - imag;
                    fftReal_[a] += real; fftImag_[a] += imag;
                }
            }
        }
    }

    void unpack(std::size_t k, float& lr, float& li, float& rr, float& ri) const noexcept {
        const std::size_t mirror = (fftSize_ - k) & (fftSize_ - 1);
        lr = 0.5f * (fftReal_[k] + fftReal_[mirror]);
        li = 0.5f * (fftImag_[k] - fftImag_[mirror]);
        rr = 0.5f * (fftImag_[k] + fftImag_[mirror]);
        ri = 0.5f * (fftReal_[mirror] - fftReal_[k]);
    }

    float nearbyPower(double bin) const noexcept {
        const int center = static_cast<int>(std::lround(bin));
        if (center < 2 || center >= static_cast<int>(fftSize_ / 2) - 1) return 0.0f;
        return std::max({power_[static_cast<std::size_t>(center - 1)],
                         power_[static_cast<std::size_t>(center)],
                         power_[static_cast<std::size_t>(center + 1)]});
    }

    void analyse(int channels) noexcept {
        ++analysisFrames_;
        for (std::size_t i = 0; i < fftSize_; ++i) {
            const std::size_t read = (writePosition_ + i) & (fftSize_ - 1);
            const auto destination = static_cast<std::size_t>(reversed_[i]);
            fftReal_[destination] = historyLeft_[read] * window_[i];
            fftImag_[destination] = historyRight_[read] * window_[i];
        }
        transform();
        for (std::size_t k = 0; k <= fftSize_ / 2; ++k) {
            float lr = 0.0f, li = 0.0f, rr = 0.0f, ri = 0.0f;
            unpack(k, lr, li, rr, ri);
            power_[k] = std::max(lr * lr + li * li, rr * rr + ri * ri);
        }
        int bestBin = -1;
        float bestScore = 0.0f;
        // -54 dBFS sine peak. Require both absolute level and spectral isolation.
        const float minimumPower = 0.25f * windowSum_ * windowSum_ * 3.9810717e-6f;
        for (int bin = minimumBin_; bin <= maximumBin_; ++bin) {
            const auto k = static_cast<std::size_t>(bin);
            const float p = power_[k];
            if (p < minimumPower || p <= power_[k - 1] || p < power_[k + 1]) continue;
            const double hz = static_cast<double>(bin) * analysisRate_ / fftSize_;
            if (occupied(hz)) continue;
            float surround = 0.0f;
            int neighbours = 0;
            for (int offset = 4; offset <= 12; ++offset) {
                if (bin - offset >= 1) { surround += power_[static_cast<std::size_t>(bin - offset)]; ++neighbours; }
                if (bin + offset < static_cast<int>(fftSize_ / 2)) {
                    surround += power_[static_cast<std::size_t>(bin + offset)]; ++neighbours;
                }
            }
            const float mean = surround / static_cast<float>(std::max(neighbours, 1)) + 1.0e-18f;
            if (p < mean * 31.6228f) continue; // 15 dB local prominence.
            // Reject harmonic families that commonly belong to voices/instruments.
            bool harmonic = false;
            for (int ratio = 2; ratio <= 4; ++ratio) {
                const double subharmonicBin = static_cast<double>(bin) / ratio;
                if (nearbyPower(static_cast<double>(bin) * ratio) > p * 0.10f
                    || (bin - subharmonicBin >= 4.0
                        && nearbyPower(subharmonicBin) > p * 0.15f)) {
                    harmonic = true; break;
                }
            }
            if (harmonic) continue;
            const float score = p / std::sqrt(mean);
            if (score > bestScore) { bestScore = score; bestBin = bin; }
        }
        if (bestBin < 0) { candidateFrames_ = 0; previousBin_ = -1; return; }
        const auto k = static_cast<std::size_t>(bestBin);
        const double before = std::log(std::max(1.0e-30f, power_[k - 1]));
        const double centre = std::log(std::max(1.0e-30f, power_[k]));
        const double after = std::log(std::max(1.0e-30f, power_[k + 1]));
        const double denominator = before - 2.0 * centre + after;
        const double offset = std::abs(denominator) > 1.0e-12
            ? std::clamp(0.5 * (before - after) / denominator, -0.5, 0.5) : 0.0;
        double frequency = (bestBin + offset) * analysisRate_ / fftSize_;
        float lr = 0.0f, li = 0.0f, rr = 0.0f, ri = 0.0f;
        unpack(k, lr, li, rr, ri);
        const int selectedChannel = channels == 2 && rr * rr + ri * ri > lr * lr + li * li ? 1 : 0;
        const double phase = selectedChannel == 0 ? std::atan2(li, lr) : std::atan2(ri, rr);
        if (bestBin == previousBin_ && selectedChannel == previousChannel_) {
            const double expected = 2.0 * pi_ * bestBin * hopSize_ / fftSize_;
            const double phaseError = std::remainder(phase - previousPhase_ - expected, 2.0 * pi_);
            const double phaseFrequency = (bestBin + phaseError * fftSize_ / (2.0 * pi_ * hopSize_))
                * analysisRate_ / fftSize_;
            if (std::abs(phaseFrequency - frequency) < analysisRate_ / fftSize_ * 0.55)
                frequency = phaseFrequency;
        }
        previousBin_ = bestBin; previousChannel_ = selectedChannel; previousPhase_ = phase;
        const double maximumFrequency = std::min(12000.0, analysisRate_ * 0.42);
        if (frequency < 119.5 || frequency > maximumFrequency + 0.5) {
            candidateFrames_ = 0;
            return;
        }
        frequency = std::clamp(frequency, 120.0, maximumFrequency);
        if (candidateFrames_ > 0
            && std::abs(frequency - candidateFrequency_) < std::max(3.0, frequency * 0.012)) {
            ++candidateFrames_;
            candidateFrequency_ += 0.22 * (frequency - candidateFrequency_);
        } else {
            candidateFrames_ = 1; candidateFrequency_ = frequency;
        }
        if (candidateFrames_ >= requiredFrames_) {
            if (!occupied(candidateFrequency_)) install(candidateFrequency_);
            candidateFrames_ = 0; previousBin_ = -1;
        }
    }

    inline static std::atomic<unsigned> instanceCounter_{0};
    unsigned instancePhase_ = 0;
    double sampleRate_ = 48000.0, analysisRate_ = 48000.0;
    int channels_ = 2, decimation_ = 1, decimationCounter_ = 0;
    int hopCounter_ = 0, requiredFrames_ = 27, minimumBin_ = 6, maximumBin_ = 512;
    std::size_t writePosition_ = 0, filled_ = 0;
    float amount_ = 0.5f, depthTarget_ = 0.498812766f, fadeStep_ = 0.001f;
    float windowSum_ = 1023.5f;
    bool hold_ = true;
    bool restorePending_ = false;
    int pendingRestoreCount_ = 0;
    std::array<float, 6> pendingRestore_{};
    int candidateFrames_ = 0, previousBin_ = -1, previousChannel_ = 0;
    double candidateFrequency_ = 0.0, previousPhase_ = 0.0;
    std::uint64_t analysisFrames_ = 0;
    std::array<Slot, 6> slots_{};
    std::array<Coefficients, 2> antiAliasCoefficients_{};
    std::array<std::array<FilterState, 2>, 2> antiAliasState_{};
    std::array<float, fftSize_> historyLeft_{}, historyRight_{}, window_{}, fftReal_{}, fftImag_{};
    std::array<float, fftSize_ / 2> twiddleReal_{}, twiddleImag_{};
    std::array<float, fftSize_ / 2 + 1> power_{};
    std::array<std::uint16_t, fftSize_> reversed_{};
};

} // namespace sculptliveclean
