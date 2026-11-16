#include "../Source/LiveCleanEngine.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<bool> track{false};
std::atomic<std::size_t> allocations{0};
void countAllocation() { if (track.load(std::memory_order_relaxed)) allocations.fetch_add(1); }
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void releaseMemory(void* p) noexcept { std::free(p); }
}
void* operator new(std::size_t n) {
    countAllocation();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { releaseMemory(p); }
void operator delete[](void* p) noexcept { releaseMemory(p); }
void operator delete(void* p, std::size_t) noexcept { releaseMemory(p); }
void operator delete[](void* p, std::size_t) noexcept { releaseMemory(p); }
void* operator new(std::size_t n, std::align_val_t align) {
    countAllocation(); void* p = nullptr;
    if (posix_memalign(&p, std::max(sizeof(void*), static_cast<std::size_t>(align)), n ? n : 1) == 0) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t align) { return ::operator new(n, align); }
void operator delete(void* p, std::align_val_t) noexcept { releaseMemory(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseMemory(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseMemory(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseMemory(p); }

namespace {
using namespace sculptliveclean;
constexpr double rate = 48000.0, pi = 3.14159265358979323846;
struct Audio {
    std::vector<float> l, r;
    explicit Audio(int n) : l(static_cast<std::size_t>(n)), r(l.size()) {}
};
std::uint32_t rng = 123456789;
float noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return static_cast<float>(rng >> 8) / 8388608.0f - 1.0f; }
void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
Parameters neutral() { Parameters p; p.denoise = p.dereverb = p.defeedback = 0; return p; }
void setup(LiveCleanEngine& e, Parameters p, double sr = rate, int ch = 2) {
    e.setParameters(p); e.prepare(sr, 128, ch);
}
void run(LiveCleanEngine& e, Audio& a, int block = 128, int ch = 2) {
    for (int pos = 0; pos < static_cast<int>(a.l.size());) {
        int n = std::min(block, static_cast<int>(a.l.size()) - pos);
        float* data[]{a.l.data() + pos, a.r.data() + pos};
        e.process(data, ch, n); pos += n;
    }
    for (std::size_t i = 0; i < a.l.size(); ++i)
        require(std::isfinite(a.l[i]) && std::isfinite(a.r[i]), "Non-finite output");
}
Audio white(double seconds, double sr = rate, float level = 0.03f) {
    Audio a(static_cast<int>(seconds * sr));
    for (std::size_t i = 0; i < a.l.size(); ++i) { a.l[i] = level * noise(); a.r[i] = level * noise(); }
    return a;
}
Audio tone(double seconds, double hz = 997.0, double sr = rate, bool anti = false) {
    Audio a(static_cast<int>(seconds * sr));
    for (std::size_t i = 0; i < a.l.size(); ++i) {
        a.l[i] = static_cast<float>(0.12 * std::sin(2 * pi * hz * static_cast<double>(i) / sr));
        a.r[i] = anti ? -a.l[i] : a.l[i];
    }
    return a;
}
double rms(const Audio& a, double from, double to, double sr = rate) {
    const auto lo = static_cast<std::size_t>(from * sr), hi = static_cast<std::size_t>(to * sr);
    require(lo < hi && hi <= a.l.size(), "Bad RMS interval");
    double sum = 0;
    for (auto i = lo; i < hi; ++i) sum += static_cast<double>(a.l[i]) * a.l[i] + static_cast<double>(a.r[i]) * a.r[i];
    return std::sqrt(sum / (2 * static_cast<double>(hi - lo)));
}
double dbRatio(double a, double b) { return 20 * std::log10(std::max(1e-15, a) / std::max(1e-15, b)); }
double difference(const Audio& a, const Audio& b) {
    double d = 0;
    for (std::size_t i = 0; i < a.l.size(); ++i)
        d = std::max({d, std::abs(static_cast<double>(a.l[i]) - b.l[i]), std::abs(static_cast<double>(a.r[i]) - b.r[i])});
    return d;
}
void learn(LiveCleanEngine& e) {
    e.requestNoiseLearn(); auto n = white(2.1); run(e, n);
    require(e.getMeters().noiseProfileReady && !e.getMeters().noiseLearning, "Noise learning did not finish in 2 seconds");
}
void testNeutralAndLatency() {
    for (int ch : {1, 2}) {
        LiveCleanEngine e; setup(e, neutral(), rate, ch);
        auto a = white(0.2); const auto original = a; run(e, a, 63, ch);
        require(difference(a, original) == 0, "Neutral path is not bit-exact");
        Audio impulse(512); impulse.l[0] = impulse.r[0] = 0.5f; run(e, impulse, 128, ch);
        require(impulse.l[0] == 0.5f, "Audio buffer delay detected");
        for (std::size_t i = 1; i < impulse.l.size(); ++i) require(impulse.l[i] == 0, "Neutral impulse has a tail");
        require(e.getLatencySamples() == 0, "Reported latency is not zero");
    }
}
void testUnlearnedAndDenoise() {
    Parameters p = neutral(); p.denoise = 1;
    LiveCleanEngine e; setup(e, p);
    auto a = white(0.5); auto original = a; run(e, a);
    require(difference(a, original) == 0, "Unlearned denoiser changed audio");
    learn(e);
    a = white(1.0); original = a; run(e, a);
    const double reduction = dbRatio(rms(a, 0.3, 1), rms(original, 0.3, 1));
    std::cout << "Noise-only attenuation: " << reduction << " dB\n";
    require(reduction < -5.0 && reduction > -13.5, "Denoise noise-only reduction outside meaningful bounded range");
    auto voice = tone(1.0, 440);
    for (std::size_t i = 0; i < voice.l.size(); ++i) {
        voice.l[i] = voice.r[i] = voice.l[i] * 2.0f + 0.02f * noise();
    }
    const auto voiceOriginal = voice; run(e, voice);
    require(dbRatio(rms(voice, 0.3, 1), rms(voiceOriginal, 0.3, 1)) > -2.5, "Denoise removed the dominant wanted tone");
}
void testDereverbDecay() {
    Parameters p = neutral(); p.dereverb = 1;
    LiveCleanEngine e; setup(e, p);
    Audio a(static_cast<int>(1.3 * rate));
    for (std::size_t i = 0; i < a.l.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double envelope = t < 0.22 ? 1.0 : std::exp(-6.907755 * (t - 0.22) / 0.9);
        a.l[i] = static_cast<float>(0.15 * envelope * noise()); a.r[i] = a.l[i];
    }
    const auto original = a; run(e, a);
    const double attack = dbRatio(rms(a, 0.01, 0.10), rms(original, 0.01, 0.10));
    const double tail = dbRatio(rms(a, 0.45, 0.9), rms(original, 0.45, 0.9));
    std::cout << "Dereverb direct onset: " << attack << " dB; decay: " << tail << " dB\n";
    require(attack > -1.0 && attack < 1.0, "Dereverb damaged the direct onset");
    require(tail < -3.0 && tail > -11.0, "Dereverb did not reduce the synthetic late decay");
    // This is a controlled exponential tail fixture, not a claim of source separation.
}
void testFeedbackDetection() {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Parameters p = neutral(); p.defeedback = 1; p.feedbackHold = false;
        LiveCleanEngine e; setup(e, p, sr);
        auto a = tone(1.2, 997, sr, true); const auto original = a; run(e, a);
        const double cut = dbRatio(rms(a, 0.8, 1.2, sr), rms(original, 0.8, 1.2, sr));
        std::cout << "Feedback " << sr << " Hz / anti-phase stereo: " << cut << " dB\n";
        require(e.getMeters().activeNotches >= 1, "Feedback detector failed on persistent isolated tone");
        require(cut < -8 && cut > -13, "Feedback notch has wrong attenuation");
        p.feedbackHold = true; e.setParameters(p);
        const int before = e.getMeters().activeNotches;
        auto other = tone(0.8, 1901, sr); run(e, other);
        require(e.getMeters().activeNotches == before, "Hold added a new feedback notch");
        e.requestFeedbackReset(); auto cleared = tone(0.8, 997, sr); const auto dry = cleared; run(e, cleared);
        require(e.getMeters().activeNotches == 0, "Reset did not clear feedback filters");
        require(dbRatio(rms(cleared, 0.6, 0.8, sr), rms(dry, 0.6, 0.8, sr)) > -0.1, "Clear did not restore dry response");
    }
}
void testFeedbackMusicalGuard() {
    Parameters p = neutral(); p.defeedback = 1; p.feedbackHold = false;
    LiveCleanEngine e; setup(e, p);
    auto a = white(1.4, rate, 0.005f);
    for (std::size_t i = 0; i < a.l.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const float x = static_cast<float>(0.12 * std::sin(2*pi*330*t) + 0.08*std::sin(2*pi*660*t) + 0.06*std::sin(2*pi*990*t));
        a.l[i] += x; a.r[i] += x;
    }
    run(e, a);
    require(e.getMeters().activeNotches == 0, "Harmonic musical fixture falsely notched");
}
void testLearnedRecallAndValidation() {
    Parameters p = neutral(); p.denoise = p.defeedback = 1;
    LiveCleanEngine e; setup(e, p); learn(e);
    auto state = e.getLearnedState();
    state.feedbackFrequencies = {997, 1901, 0, 0, 0, 0}; state.activeNotches = 2;
    LiveCleanEngine restored; setup(restored, p); restored.restoreLearnedState(state);
    require(restored.getLearnedState().activeNotches == 2, "Restore intent lost before first audio callback");
    auto a = tone(0.8); run(restored, a);
    require(restored.getMeters().noiseProfileReady && restored.getMeters().activeNotches == 2, "Learned state was not recalled");
    const auto recalled = restored.getLearnedState();
    for (std::size_t i = 0; i < state.noisePower.size(); ++i)
        require(recalled.noisePower[i] == state.noisePower[i], "Noise profile changed during Hold");
    restored.requestFeedbackReset();
    require(restored.getLearnedState().activeNotches == 0, "Clear intent lost before fade completed");
    state.noisePower[3] = std::numeric_limits<float>::quiet_NaN();
    restored.restoreLearnedState(state); auto quiet = white(0.6); run(restored, quiet);
    require(!restored.getMeters().noiseProfileReady, "Corrupt learned noise accepted");
    state = e.getLearnedState(); state.sampleRate = 44100;
    restored.restoreLearnedState(state);
    require(!restored.getMeters().noiseProfileReady, "Incompatible sample-rate profile accepted");
}
void testBypassAndStereo() {
    Parameters p; p.denoise = p.dereverb = p.defeedback = 1; p.outputGain = 1.6f;
    LiveCleanEngine e; setup(e, p); learn(e);
    p.bypass = true; e.setParameters(p); auto warm = white(0.1); run(e, warm);
    auto a = white(0.4); const auto original = a; run(e, a);
    require(difference(a, original) == 0, "Bypass is not exact dry or retains output trim");
    p = neutral(); p.dereverb = 0.7f; setup(e, p);
    a = tone(0.6); run(e, a);
    for (std::size_t i = 0; i < a.l.size(); ++i) require(a.l[i] == a.r[i], "Stereo linked gains changed a mono image");
}
void testBlockIndependence() {
    Parameters p = neutral(); p.dereverb = 0.6f;
    LiveCleanEngine a, b; setup(a, p); setup(b, p);
    auto x = white(0.7); auto y = x;
    run(a, x, 1); run(b, y, 257);
    require(difference(x, y) == 0, "Result depends on host block size");
}
void testRatesStabilityAndBadInput() {
    for (double sr : {8000.0, 44100.0, 192000.0, 384000.0}) {
        Parameters p; p.denoise = p.dereverb = p.defeedback = 1; p.feedbackHold = false;
        LiveCleanEngine e; setup(e, p, sr);
        auto a = white(0.08, sr, 1.0f);
        a.l[10] = std::numeric_limits<float>::quiet_NaN();
        a.r[20] = std::numeric_limits<float>::infinity(); run(e, a);
        p.outputGain = std::numeric_limits<float>::quiet_NaN(); e.setParameters(p);
        Audio silence(2048); run(e, silence);
        e.reset();
        std::fill(silence.l.begin(), silence.l.end(), 0.0f);
        std::fill(silence.r.begin(), silence.r.end(), 0.0f);
        run(e, silence);
        for (float x : silence.l) require(x == 0, "Reset left filter memory");
    }
}
void testNoAllocations() {
    Parameters p; p.feedbackHold = false;
    LiveCleanEngine e; setup(e, p);
    auto state = e.getLearnedState(); state.feedbackFrequencies = {550, 997, 1901, 3400, 5100, 7500}; state.activeNotches = 6;
    std::array<float,128> l{}, r{}; float* ptrs[]{l.data(),r.data()};
    allocations.store(0); track.store(true);
    e.requestNoiseLearn(); e.restoreLearnedState(state);
    for (int block = 0; block < 900; ++block) {
        for (std::size_t i = 0; i < l.size(); ++i) { l[i] = noise()*0.1f; r[i] = noise()*0.1f; }
        if (block == 300) e.requestFeedbackReset();
        if (block == 500) { p.feedbackHold = true; e.setParameters(p); }
        e.process(ptrs,2,128);
    }
    e.reset(); track.store(false);
    require(allocations.load() == 0, "Allocation occurred on the audio path");
}
}

int main() {
    const std::array<std::pair<const char*,void(*)()>,10> tests{{
        {"neutral, mono/stereo, zero buffering delay",testNeutralAndLatency},
        {"learned denoise and dominant-tone retention",testUnlearnedAndDenoise},
        {"late decay reduction and onset protection",testDereverbDecay},
        {"feedback, anti-phase stereo, Hold and Clear",testFeedbackDetection},
        {"harmonic material guard",testFeedbackMusicalGuard},
        {"learned state recall and validation",testLearnedRecallAndValidation},
        {"bypass and linked stereo",testBypassAndStereo},
        {"block-size independence",testBlockIndependence},
        {"sample rates, reset and nonfinite input",testRatesStabilityAndBadInput},
        {"no audio-thread allocations",testNoAllocations}
    }};
    try {
        std::cout << std::fixed << std::setprecision(3);
        for (const auto& t : tests) { t.second(); std::cout << "PASS: " << t.first << '\n'; }
        std::cout << "All 10 Sculpt Live Clean core checks passed.\n"; return 0;
    } catch (const std::exception& e) {
        track.store(false); std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
