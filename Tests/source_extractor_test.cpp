#include "../Source/LiveCleanEngine.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> tracking{false};
std::atomic<std::size_t> allocationCount{0};
void allocation() { if (tracking.load(std::memory_order_relaxed)) allocationCount.fetch_add(1); }
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void releaseMemory(void* p) noexcept { std::free(p); }
}
void* operator new(std::size_t n) {
    allocation();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { releaseMemory(p); }
void operator delete[](void* p) noexcept { releaseMemory(p); }
void operator delete(void* p, std::size_t) noexcept { releaseMemory(p); }
void operator delete[](void* p, std::size_t) noexcept { releaseMemory(p); }
void* operator new(std::size_t n, std::align_val_t align) {
    allocation(); void* p = nullptr;
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
constexpr double rate = 48000.0;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// Independent scalar transcription of the original Strip's enabled gate.
// Threshold, Depth(%) and helpers retain the upstream units and expression
// order. This is a numerical reference, not a call into SourceExtractor.
struct StripReference {
    double sr, threshold = -32.0, targetThreshold = -32.0;
    double depth = 50.0, targetDepth = 50.0, envelope = 0.0, gr = 0.0;
    int hold = 0;
    explicit StripReference(double sampleRate) : sr(sampleRate) {}
    double coefficient(double milliseconds) const { return std::exp(-1.0 / (milliseconds * 0.001 * sr)); }
    static double computer(double level, double thresholdDb, double ratio, double knee) {
        const double slope = 1.0 - 1.0 / std::max(1.0, ratio);
        const double over = level - thresholdDb;
        if (knee > 1.0e-3 && 2.0 * std::abs(over) <= knee) {
            const double t = over + knee / 2.0;
            return slope * t * t / (2.0 * knee);
        }
        return over > 0.0 ? slope * over : 0.0;
    }
    double tick(float l, float r) {
        threshold = targetThreshold + (threshold - targetThreshold) * coefficient(20.0);
        depth = targetDepth + (depth - targetDepth) * coefficient(20.0);
        const double peak = std::max(std::abs(static_cast<double>(l)), std::abs(static_cast<double>(r)));
        envelope = std::max(peak, envelope * coefficient(5.0));
        const double db = 20.0 * std::log10(std::max(1.0e-12, envelope));
        double target = std::min(computer(-db, -threshold, 4.0, 6.0), depth * 0.48);
        if (db > threshold + 2.0) hold = static_cast<int>(0.06 * sr);
        if (target > gr && hold > 0) { --hold; target = gr; }
        gr = target + (gr - target) * coefficient(target < gr ? 2.0 : 180.0);
        return std::pow(10.0, -gr / 20.0);
    }
};

void testReference() {
    double worst = 0.0;
    for (double sr : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        SourceExtractor source;
        source.setParameters(-32.0f, 0.5f, true); source.prepare(sr);
        StripReference reference(sr);
        const int count = static_cast<int>(sr * 0.8);
        for (int i = 0; i < count; ++i) {
            if (i == count / 3) {
                source.setParameters(-24.0f, 0.75f, true);
                reference.targetThreshold = -24.0; reference.targetDepth = 75.0;
            }
            if (i == 2 * count / 3) {
                source.setParameters(-40.0f, 0.25f, true);
                reference.targetThreshold = -40.0; reference.targetDepth = 25.0;
            }
            const double t = static_cast<double>(i) / sr;
            const double amplitude = t < 0.1 || t > 0.45 ? 0.00007 : 0.16;
            const float rawL = static_cast<float>(amplitude * std::sin(i * 0.137));
            const float rawR = static_cast<float>(-0.37 * rawL);
            float l = rawL * 0.41f, r = rawR * 0.73f;
            const float originalL = l, originalR = r;
            const double gain = reference.tick(rawL, rawR);
            source.processSample(l, r, rawL, rawR, 2);
            worst = std::max({worst, std::abs(l - static_cast<double>(originalL) * gain),
                             std::abs(r - static_cast<double>(originalR) * gain)});
            require(std::abs(source.getReductionDb() - reference.gr) < 2.0e-5,
                    "Source gain computer differs from the original Strip");
        }
    }
    require(worst < 2.0e-8, "Source output differs from original Strip gate");
    std::cout << "Reference maximum absolute sample error: " << worst << '\n';
}

void feed(SourceExtractor& source, int count, float level, float rightScale = 1.0f) {
    for (int i = 0; i < count; ++i) {
        float l = level, r = level * rightScale;
        source.processSample(l, r, l, r, 2);
    }
}

void testFloorAndOpening() {
    for (float depth : {0.25f, 0.5f, 1.0f}) {
        SourceExtractor source;
        source.setParameters(-20.0f, depth, true); source.prepare(rate);
        feed(source, 96000, 0.000001f);
        require(std::abs(source.getReductionDb() - depth * 48.0f) < 0.002f,
                "Depth does not set the original bounded attenuation floor");
        float l = 0.000001f, r = -l;
        source.processSample(l, r, l, r, 2);
        require(l > 0.0f && l >= 0.000001f * std::pow(10.0f, -depth * 48.0f / 20.0f),
                "Source closed below its selected floor");
        feed(source, 2400, 0.5f);
        require(source.getReductionDb() < 0.001f, "Source opening did not recover above threshold");
    }
}

void testHoldAndStereo() {
    SourceExtractor source;
    source.setParameters(-32.0f, 1.0f, true); source.prepare(rate);
    feed(source, 9600, 0.5f);
    feed(source, 1920, 0.000001f);
    require(source.getReductionDb() < 0.001f, "Short gap bypassed the original hold");
    feed(source, 4800, 0.000001f);
    require(source.getReductionDb() > 8.0f, "Long gap never closed after hold");
    for (int i = 0; i < 1000; ++i) {
        float l = 0.02f, r = -0.004f;
        source.processSample(l, r, 0.0001f, -0.00003f, 2);
        require(std::abs(static_cast<double>(r) + 0.2 * l) < 2.0e-9, "Source changed stereo balance");
    }
    // A loud raw detector must keep an already cleaned, quiet signal open.
    source.reset();
    for (int i = 0; i < 24000; ++i) {
        float l = 0.0001f, r = -0.0001f;
        source.processSample(l, r, 0.1f, -0.1f, 2);
        require(l == 0.0001f && r == -0.0001f, "Source detector used processed audio or stereo cancellation");
    }
}

void testAutomationNeutralAndLatency() {
    SourceExtractor source;
    source.prepare(rate);
    float l = 0.3f, r = -0.3f;
    source.processSample(l, r, l, r, 2);
    require(l == 0.3f && r == -0.3f, "Disabled source is not bit-exact neutral");
    source.setParameters(-20.0f, 1.0f, true);
    float previous = 1.0f;
    double largestStep = 0.0;
    for (int i = 0; i < 144000; ++i) {
        if (i == 48000) source.setParameters(-20.0f, 1.0f, false);
        if (i == 96000) source.setParameters(-20.0f, 0.0f, true);
        l = 0.0001f; r = -l;
        source.processSample(l, r, l, r, 2);
        const float gain = l / 0.0001f;
        largestStep = std::max(largestStep, std::abs(static_cast<double>(gain) - previous));
        previous = gain;
        if (i == 95999 || i == 143999) require(l == 0.0001f && r == -0.0001f,
            "Disabled/zero-depth source did not settle to exact unity");
    }
    require(largestStep < 0.003, "Enable/depth automation produced an abrupt gain jump");
    source.setParameters(-32.0f, 0.5f, true); source.reset();
    l = 0.3f; r = 0.3f;
    source.processSample(l, r, l, r, 2);
    require(l == 0.3f, "Source delayed a new direct impulse");
}

void testResetAndInvalidInput() {
    SourceExtractor source, fresh;
    source.setParameters(-26.0f, 0.75f, true); source.prepare(rate);
    fresh.setParameters(-26.0f, 0.75f, true); fresh.prepare(rate);
    feed(source, 24000, 0.00001f);
    source.reset();
    for (int i = 0; i < 1000; ++i) {
        float a = 0.0001f, b = -a, c = a, d = b;
        source.processSample(a, b, a, b, 2); fresh.processSample(c, d, c, d, 2);
        require(a == c && b == d, "Source reset retained audio history");
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    source.setParameters(nan, std::numeric_limits<float>::infinity(), true);
    for (float value : {nan, std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()}) {
        float a = value, b = value;
        source.processSample(a, b, value, value, 2);
        require(std::isfinite(a) && std::isfinite(b) && std::isfinite(source.getReductionDb()),
                "Invalid source input escaped into output/state");
    }
    source.prepare(std::numeric_limits<double>::quiet_NaN());
    float a = 0.1f, b = -a;
    source.processSample(a, b, a, b, 2);
    require(std::isfinite(a) && std::isfinite(b), "Invalid sample rate poisoned the source");
}

void testEngineIntegration() {
    LiveCleanEngine engine;
    Parameters p;
    p.dereverb = p.defeedback = 0.0f; p.denoise = 1.0f;
    p.sourceEnabled = true; p.sourceThresholdDb = -24.0f; p.sourceDepth = 1.0f;
    engine.setParameters(p); engine.prepare(rate, 128, 2); engine.requestNoiseLearn();
    std::array<float, 128> l{}, r{};
    float* channels[]{l.data(), r.data()};
    for (int block = 0; block < 1500; ++block) {
        l.fill(0.1f); r.fill(-0.1f); engine.process(channels, 2, 128);
    }
    require(l.back() < 0.04f && l.back() > 0.02f, "Denoise fixture did not lower audio below source threshold");
    require(engine.getMeters().sourceReductionDb < 0.001f,
            "Denoise changed source threshold action: detector must be raw");
    p.bypass = true; engine.setParameters(p);
    for (int block = 0; block < 10; ++block) {
        l.fill(0.0001f); r.fill(-0.0001f); engine.process(channels, 2, 128);
    }
    require(l.back() == 0.0001f && r.back() == -0.0001f && engine.getMeters().sourceReductionDb == 0.0f,
            "Engine full bypass did not bypass Source Extractor");
    require(engine.getLatencySamples() == 0, "Source integration added buffering latency");
}

void testBlockIndependence() {
    std::array<std::vector<float>, 2> a, b;
    for (int c = 0; c < 2; ++c) {
        a[static_cast<std::size_t>(c)].resize(18000);
        for (int i = 0; i < 18000; ++i)
            a[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = static_cast<float>(
                (i < 3000 ? 0.2 : 0.0001) * std::sin(i * 0.17) * (c == 0 ? 1.0 : -0.31));
    }
    b = a;
    Parameters p; p.denoise = p.dereverb = p.defeedback = 0.0f;
    p.sourceEnabled = true;
    for (int which = 0; which < 2; ++which) {
        auto& audio = which == 0 ? a : b;
        LiveCleanEngine engine; engine.setParameters(p); engine.prepare(rate, 128, 2);
        const int blockSize = which == 0 ? 128 : 17;
        for (int offset = 0; offset < 18000; offset += blockSize) {
            float* channels[]{audio[0].data() + offset, audio[1].data() + offset};
            engine.process(channels, 2, std::min(blockSize, 18000 - offset));
        }
    }
    require(a == b, "Source integration depends on host block partitioning");
}

void testNoAllocations() {
    LiveCleanEngine engine; Parameters p;
    p.denoise = p.dereverb = p.defeedback = 0.0f; p.sourceEnabled = true;
    engine.setParameters(p); engine.prepare(rate, 128, 2);
    std::array<float, 128> l{}, r{}; float* channels[]{l.data(), r.data()};
    allocationCount = 0; tracking = true;
    for (int i = 0; i < 1000; ++i) {
        if (i % 31 == 0) { p.sourceEnabled = !p.sourceEnabled; engine.setParameters(p); }
        l.fill(i % 17 == 0 ? 0.1f : 0.0001f); r = l;
        engine.process(channels, 2, 128);
    }
    tracking = false;
    require(allocationCount == 0, "Source processing/automation allocated on the audio thread");
}
}

int main() {
    try {
        testReference(); std::cout << "PASS: original Strip reference at six sample rates\n";
        testFloorAndOpening(); std::cout << "PASS: depth floor and direct-source reopening\n";
        testHoldAndStereo(); std::cout << "PASS: hold, linked stereo and raw-input detection\n";
        testAutomationNeutralAndLatency(); std::cout << "PASS: smooth automation, neutral endpoints and no sample delay\n";
        testResetAndInvalidInput(); std::cout << "PASS: reset and invalid input/parameter handling\n";
        testEngineIntegration(); std::cout << "PASS: denoise interaction and global bypass\n";
        testBlockIndependence(); std::cout << "PASS: block-size independence\n";
        testNoAllocations(); std::cout << "PASS: no audio-thread allocations\n";
        std::cout << "All 8 Source Extractor checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        tracking = false;
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
