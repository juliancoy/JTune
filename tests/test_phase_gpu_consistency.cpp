#include "loiacono_gpu_rolling_compute.h"
#include "loiacono_rolling.h"

#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct DiffStats {
    double maxAbs = 0.0;
    double meanAbs = 0.0;
    int comparedBins = 0;
};

double wrapToPi(double x)
{
    while (x > kPi) x -= 2.0 * kPi;
    while (x < -kPi) x += 2.0 * kPi;
    return x;
}

std::vector<float> makeSignal(unsigned int sampleRate, int frames)
{
    std::vector<float> x(static_cast<size_t>(frames), 0.0f);
    for (int n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        const double chirpHz = 180.0 + 980.0 * std::clamp(t / 0.2, 0.0, 1.0);
        const double chirp = std::sin(2.0 * kPi * chirpHz * t);
        const double h1 = std::sin(2.0 * kPi * 220.0 * t);
        const double h2 = std::sin(2.0 * kPi * 440.0 * t + 0.7);
        const double h3 = std::sin(2.0 * kPi * 660.0 * t - 0.4);
        x[static_cast<size_t>(n)] = static_cast<float>(0.45 * chirp + 0.30 * h1 + 0.18 * h2 + 0.12 * h3);
    }
    return x;
}

DiffStats comparePhase(const std::vector<float>& cpuMag,
                       const std::vector<float>& cpuPhase,
                       const std::vector<float>& gpuMag,
                       const std::vector<float>& gpuPhase)
{
    DiffStats s;
    if (cpuMag.size() != gpuMag.size() || cpuPhase.size() != gpuPhase.size() || cpuMag.size() != cpuPhase.size()) {
        return s;
    }

    float maxMag = 0.0f;
    for (float v : cpuMag) maxMag = std::max(maxMag, std::abs(v));
    const double magThresh = std::max(1e-4, static_cast<double>(maxMag) * 0.02);

    double sumAbs = 0.0;
    for (size_t i = 0; i < cpuMag.size(); ++i) {
        if (std::abs(cpuMag[i]) < magThresh || std::abs(gpuMag[i]) < magThresh) continue;
        const double d = std::abs(wrapToPi(static_cast<double>(gpuPhase[i]) - static_cast<double>(cpuPhase[i])));
        s.maxAbs = std::max(s.maxAbs, d);
        sumAbs += d;
        s.comparedBins++;
    }
    if (s.comparedBins > 0) s.meanAbs = sumAbs / static_cast<double>(s.comparedBins);
    return s;
}

bool runCpuReference(LoiaconoRolling::AlgorithmMode algo,
                     const std::vector<float>& signal,
                     std::vector<float>& mag,
                     std::vector<float>& phase)
{
    LoiaconoRolling cpu;
    cpu.setComputeMode(LoiaconoRolling::ComputeMode::MultiThread);
    cpu.setWindowMode(LoiaconoRolling::WindowMode::RectangularWindow);
    cpu.setNormalizationMode(LoiaconoRolling::NormalizationMode::Energy);
    cpu.setWindowLengthMode(LoiaconoRolling::WindowLengthMode::PeriodMultiple);
    cpu.setAlgorithmMode(algo);
    cpu.setLeakiness(1.0);
    cpu.setPhaseCalculationEnabled(true);
    cpu.configure(48000.0, 80.0, 3000.0, 160, 24);
    cpu.processChunk(signal.data(), static_cast<int>(signal.size()));
    cpu.getSpectrum(mag);
    cpu.getPhase(phase);
    return !mag.empty() && mag.size() == phase.size();
}

bool runGpuBackend(LoiaconoRolling::ComputeMode mode,
                   LoiaconoRolling::AlgorithmMode algo,
                   const std::vector<float>& signal,
                   std::vector<float>& mag,
                   std::vector<float>& phase)
{
    LoiaconoRolling gpu;
    gpu.setComputeMode(mode);
    gpu.setWindowMode(LoiaconoRolling::WindowMode::RectangularWindow);
    gpu.setNormalizationMode(LoiaconoRolling::NormalizationMode::Energy);
    gpu.setWindowLengthMode(LoiaconoRolling::WindowLengthMode::PeriodMultiple);
    gpu.setAlgorithmMode(algo);
    gpu.setLeakiness(1.0);
    gpu.setPhaseCalculationEnabled(true);
    gpu.configure(48000.0, 80.0, 3000.0, 160, 24);

    if (mode == LoiaconoRolling::ComputeMode::GpuCompute && !gpu.gpuComputeAvailable()) return false;
    if (mode == LoiaconoRolling::ComputeMode::VulkanCompute && !gpu.vulkanComputeAvailable()) return false;

    gpu.processChunk(signal.data(), static_cast<int>(signal.size()));
    gpu.getSpectrum(mag);
    gpu.getPhase(phase);
    return !mag.empty() && mag.size() == phase.size();
}

struct CheckResult {
    bool available = false;
    bool pass = false;
};

CheckResult runGpuRollingPhaseCheck(const std::vector<float>& signal)
{
    CheckResult r;
    LoiaconoRolling cpu;
    cpu.setComputeMode(LoiaconoRolling::ComputeMode::MultiThread);
    cpu.setWindowMode(LoiaconoRolling::WindowMode::RectangularWindow);
    cpu.setNormalizationMode(LoiaconoRolling::NormalizationMode::Energy);
    cpu.setWindowLengthMode(LoiaconoRolling::WindowLengthMode::PeriodMultiple);
    cpu.setAlgorithmMode(LoiaconoRolling::AlgorithmMode::Loiacono);
    cpu.setLeakiness(1.0);
    cpu.setPhaseCalculationEnabled(true);
    cpu.configure(48000.0, 80.0, 3000.0, 160, 24);

    const auto snapshot = cpu.gpuInputSnapshot();
    LoiaconoGpuRollingCompute rolling;
    if (!rolling.configure(static_cast<int>(snapshot.ring.size()),
                           static_cast<int>(signal.size()),
                           snapshot.numBins,
                           snapshot.freqs,
                           snapshot.norms,
                           snapshot.windowLens)) {
        return r;
    }
    r.available = true;

    if (!rolling.processChunk(signal.data(),
                              static_cast<int>(signal.size()),
                              0,
                              0,
                              1.0)) {
        return r;
    }

    cpu.processChunk(signal.data(), static_cast<int>(signal.size()));

    std::vector<float> cpuMag, cpuPhase, gpuMag, gpuPhase;
    cpu.getSpectrum(cpuMag);
    cpu.getPhase(cpuPhase);
    if (!rolling.spectrum(gpuMag, &gpuPhase)) return r;

    const auto d = comparePhase(cpuMag, cpuPhase, gpuMag, gpuPhase);
    std::cout << "[gpu-rolling][loiacono] compared=" << d.comparedBins
              << " mean=" << d.meanAbs
              << " max=" << d.maxAbs << "\n";
    r.pass = (d.comparedBins >= 8 && d.meanAbs <= 0.22 && d.maxAbs <= 1.80);
    return r;
}

}  // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    }
    QGuiApplication app(argc, argv);

    const auto signal = makeSignal(48000, 4096);
    bool anyBackendTested = false;
    bool ok = true;

    const struct {
        LoiaconoRolling::AlgorithmMode mode;
        const char* name;
    } algos[] = {
        {LoiaconoRolling::AlgorithmMode::Loiacono, "loiacono"},
        {LoiaconoRolling::AlgorithmMode::FFT, "fft"},
        {LoiaconoRolling::AlgorithmMode::Goertzel, "goertzel"},
    };

    for (const auto& a : algos) {
        std::vector<float> cpuMag, cpuPhase;
        if (!runCpuReference(a.mode, signal, cpuMag, cpuPhase)) {
            std::cerr << "CPU reference failed for " << a.name << "\n";
            return 1;
        }

        std::vector<float> glMag, glPhase;
        if (runGpuBackend(LoiaconoRolling::ComputeMode::GpuCompute, a.mode, signal, glMag, glPhase)) {
            anyBackendTested = true;
            const auto d = comparePhase(cpuMag, cpuPhase, glMag, glPhase);
            std::cout << "[opengl][" << a.name << "] compared=" << d.comparedBins
                      << " mean=" << d.meanAbs
                      << " max=" << d.maxAbs << "\n";
            if (d.comparedBins < 8 || d.meanAbs > 0.12 || d.maxAbs > 0.35) ok = false;
        } else {
            std::cout << "[opengl][" << a.name << "] unavailable\n";
        }

        std::vector<float> vkMag, vkPhase;
        if (runGpuBackend(LoiaconoRolling::ComputeMode::VulkanCompute, a.mode, signal, vkMag, vkPhase)) {
            anyBackendTested = true;
            const auto d = comparePhase(cpuMag, cpuPhase, vkMag, vkPhase);
            std::cout << "[vulkan][" << a.name << "] compared=" << d.comparedBins
                      << " mean=" << d.meanAbs
                      << " max=" << d.maxAbs << "\n";
            if (d.comparedBins < 8 || d.meanAbs > 0.12 || d.maxAbs > 0.35) ok = false;
        } else {
            std::cout << "[vulkan][" << a.name << "] unavailable\n";
        }
    }

    const auto rolling = runGpuRollingPhaseCheck(signal);
    if (rolling.available) {
        anyBackendTested = true;
        if (!rolling.pass) ok = false;
    } else {
        std::cout << "[gpu-rolling][loiacono] unavailable\n";
    }

    if (!anyBackendTested) {
        std::cout << "SKIP: no GPU backend available in this environment\n";
        return 0;
    }
    return ok ? 0 : 1;
}
