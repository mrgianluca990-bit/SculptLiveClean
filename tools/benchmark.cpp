#include "../Source/LiveCleanEngine.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int blockSize = 128;
constexpr double sampleRate = 48000.0;
struct Instance {
    sculptliveclean::LiveCleanEngine engine;
    std::array<float,blockSize> l{}, r{};
};
std::uint32_t seed = 0x12345678;
float noise() { seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; return (static_cast<float>(seed>>8)/8388608.0f-1.0f)*0.03f; }
struct Result { int instances; bool automatic; bool sourceEnabled; double mean, p99, maximum; int notches; double cpuMean, cpuP99, cpuMax; };
Result measure(int count, bool automatic, bool sourceEnabled) {
    std::vector<std::unique_ptr<Instance>> instances;
    for (int i=0;i<count;++i) {
        auto x=std::make_unique<Instance>();
        sculptliveclean::Parameters p; p.denoise=0.75f; p.dereverb=0.5f; p.defeedback=1; p.feedbackHold=!automatic;
        p.sourceEnabled=sourceEnabled; p.sourceThresholdDb=-24.0f; p.sourceDepth=0.5f;
        x->engine.setParameters(p); x->engine.prepare(sampleRate,blockSize,2);
        x->engine.requestNoiseLearn();
        for (int n=0;n<760;++n) {
            for(int j=0;j<blockSize;++j) {x->l[static_cast<std::size_t>(j)]=noise();x->r[static_cast<std::size_t>(j)]=noise();}
            float* ptrs[]{x->l.data(),x->r.data()}; x->engine.process(ptrs,2,blockSize);
        }
        auto state=x->engine.getLearnedState();
        state.feedbackFrequencies={440,997,1901,3400,5100,7500};
        state.activeNotches=automatic?4:6;
        x->engine.restoreLearnedState(state);
        instances.push_back(std::move(x));
    }
    constexpr int warm=64, measured=1500;
    std::vector<double> timings; timings.reserve(measured);
    std::vector<double> cpuTimings; cpuTimings.reserve(measured);
    volatile double resultSink=0;
    for (int b=-warm;b<measured;++b) {
        for (auto& x:instances) for(int j=0;j<blockSize;++j) {
            x->l[static_cast<std::size_t>(j)]=noise();x->r[static_cast<std::size_t>(j)]=noise();
        }
        const auto cpuStart=std::clock();
        const auto t=std::chrono::steady_clock::now();
        for (auto& x:instances) {
            float* ptrs[]{x->l.data(),x->r.data()}; x->engine.process(ptrs,2,blockSize);
        }
        const double microseconds=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count();
        const double cpuMicroseconds=1000000.0*static_cast<double>(std::clock()-cpuStart)/CLOCKS_PER_SEC;
        if(b>=0) {timings.push_back(microseconds);cpuTimings.push_back(cpuMicroseconds);}
        resultSink+=instances[0]->l[0];
    }
    if(!std::isfinite(resultSink))throw std::runtime_error("Invalid benchmark audio");
    double mean=0;
    for(double v:timings)mean+=v;
    mean/=timings.size();
    std::sort(timings.begin(),timings.end());
    double cpuMean=0;
    for(double v:cpuTimings)cpuMean+=v;
    cpuMean/=cpuTimings.size();
    std::sort(cpuTimings.begin(),cpuTimings.end());
    return {count,automatic,sourceEnabled,mean,timings[static_cast<std::size_t>(0.99*(timings.size()-1))],timings.back(),instances[0]->engine.getMeters().activeNotches,cpuMean,cpuTimings[static_cast<std::size_t>(0.99*(cpuTimings.size()-1))],cpuTimings.back()};
}
}
int main(int argc,char**argv) {
    const std::string destination=argc>1?argv[1]:"cpu-benchmark.json";
    std::ofstream out(destination);
    if(!out){std::cerr<<"Cannot write benchmark\n";return 1;}
    out<<std::fixed<<std::setprecision(4);
    out<<"{\n  \"version\":\"0.2.0\",\n  \"sampleRate\":48000,\n  \"blockSize\":128,\n  \"channels\":2,\n  \"blockBudgetMicroseconds\":2666.6667,\n  \"sourceThresholdDb\":-24,\n  \"sourceDepthPercent\":50,\n  \"method\":\"Serial independent stereo instances, learned noise and active notches, source off/on, input generation excluded, 1500 measured callbacks; wall time on this host only\",\n  \"results\":[\n";
    bool first=true;
    for(bool automatic:{false,true})for(int count:{1,8,16})for(bool sourceEnabled:{false,true}) {
        const auto r=measure(count,automatic,sourceEnabled);
        if(!first)out<<",\n";
        first=false;
        out<<"    {\"instances\":"<<r.instances<<",\"mode\":\""<<(r.automatic?"Auto":"Hold")
           <<"\",\"sourceEnabled\":"<<(r.sourceEnabled?"true":"false")
           <<",\"activeNotchesPerInstance\":"<<r.notches<<",\"meanMicroseconds\":"<<r.mean
           <<",\"p99Microseconds\":"<<r.p99<<",\"maximumMicroseconds\":"<<r.maximum
           <<",\"meanPercentOfBlockBudget\":"<<r.mean/26.6666667
           <<",\"p99PercentOfBlockBudget\":"<<r.p99/26.6666667
           <<",\"meanCpuMicroseconds\":"<<r.cpuMean<<",\"p99CpuMicroseconds\":"<<r.cpuP99
           <<",\"maxCpuMicroseconds\":"<<r.cpuMax<<",\"meanCpuPercentOfBlockBudget\":"<<r.cpuMean/26.6666667<<"}";
        std::cout<<count<<" stereo / "<<(automatic?"Auto":"Hold")<<" / Source "<<(sourceEnabled?"on":"off")<<": mean "<<r.mean
                 <<" us, p99 "<<r.p99<<" us, max "<<r.maximum<<" us; CPU mean "<<r.cpuMean<<" us\n";
    }
    out<<"\n  ]\n}\n";
}
