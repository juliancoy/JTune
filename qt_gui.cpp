#include "autotune_core.h"
#include "audio_device_labels.h"
#include "loiacono_rolling.h"
#include "pitch_tracker.h"
#include "pitch_system.hpp"

#include "RtAudio.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressDialog>
#include <QPushButton>
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QCheckBox>
#include <QSlider>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <vector>

namespace {

QString apiName(RtAudio::Api api)
{
    switch (api) {
    case RtAudio::UNSPECIFIED: return "unspecified";
    case RtAudio::LINUX_ALSA: return "alsa";
    case RtAudio::LINUX_PULSE: return "pulse";
    case RtAudio::UNIX_JACK: return "jack";
    default: return "other";
    }
}

QString algorithmName(int mode)
{
    switch (static_cast<LoiaconoRolling::AlgorithmMode>(mode)) {
    case LoiaconoRolling::AlgorithmMode::Loiacono: return "Loiacono";
    case LoiaconoRolling::AlgorithmMode::FFT: return "FFT";
    case LoiaconoRolling::AlgorithmMode::Goertzel: return "Goertzel";
    }
    return "Unknown";
}

QString resynthName(int mode)
{
    switch (mode) {
    case jtune::FrequencyDomain: return "FrequencyDomain";
    case jtune::TimeDomain: return "TimeDomain";
    default: return "FrequencyDomain";
    }
}

int parseResynthMode(const QString& raw)
{
    const QString s = raw.trimmed().toLower();
    if (s == "timedomain" || s == "time-domain" || s == "time" || s == "timedomainflow" || s == "time_domain_flow" || s == "flow") {
        return jtune::TimeDomain;
    }
    return jtune::FrequencyDomain;
}

int parseKeyRoot(const QString& key)
{
    const QString k = key.trimmed().toUpper();
    if (k == "C") return 0;
    if (k == "C#" || k == "DB") return 1;
    if (k == "D") return 2;
    if (k == "D#" || k == "EB") return 3;
    if (k == "E") return 4;
    if (k == "F") return 5;
    if (k == "F#" || k == "GB") return 6;
    if (k == "G") return 7;
    if (k == "G#" || k == "AB") return 8;
    if (k == "A") return 9;
    if (k == "A#" || k == "BB") return 10;
    if (k == "B") return 11;
    return 0;
}

int parseAlgorithmMode(const QString& algorithm)
{
    const QString a = algorithm.trimmed().toLower();
    if (a == "fft") return static_cast<int>(LoiaconoRolling::AlgorithmMode::FFT);
    if (a == "goertzel") return static_cast<int>(LoiaconoRolling::AlgorithmMode::Goertzel);
    return static_cast<int>(LoiaconoRolling::AlgorithmMode::Loiacono);
}

struct ChirpTestResult {
    bool ok = false;
    bool pitchOk = false;
    bool passthroughOk = false;
    double rawHz = 0.0;
    double tunedHz = 0.0;
    double shiftHz = 0.0;
    double expectedShiftHz = 0.0;
    double targetHz = 0.0;
    double rawTargetErrHz = 0.0;
    double tunedTargetErrHz = 0.0;
    double corr = 0.0;
    double rmse = 0.0;
    double maxAbsErr = 0.0;
    QString verifierAlgorithms;
};

constexpr double kPi = 3.14159265358979323846;

std::vector<float> generateChirp(unsigned int sampleRate, double seconds, double f0Hz, double f1Hz)
{
    const size_t total = static_cast<size_t>(seconds * static_cast<double>(sampleRate));
    std::vector<float> out(total, 0.0f);
    double phase = 0.0;
    for (size_t n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        const double alpha = std::clamp(t / seconds, 0.0, 1.0);
        const double f = f0Hz + (f1Hz - f0Hz) * alpha;
        phase += 2.0 * kPi * f / static_cast<double>(sampleRate);
        out[n] = static_cast<float>(0.2 * std::sin(phase));
    }
    return out;
}

std::vector<float> runProcessor(const std::vector<float>& input, const jtune::AutotuneOptions& opts)
{
    jtune::ConstantQAutotuneProcessor proc(opts);
    std::vector<float> out(input.size(), 0.0f);
    for (size_t i = 0; i < input.size(); ++i) {
        out[i] = proc.processSample(input[i]);
    }
    return out;
}

double estimateMedianPitchHzForAlgorithm(const std::vector<float>& signal,
                                         const jtune::AutotuneOptions& opts,
                                         int algorithmMode)
{
    const int trackerBins = std::clamp(opts.binCount / 2, 48, 200);
    const int trackerHop = std::max(opts.analysisHop, 96);
    auto trackerOpts = jtune::PitchTrackerOptions{
        opts.sampleRate,
        opts.minMidi,
        opts.maxMidi,
        opts.multiple,
        trackerBins,
        trackerHop,
        0.20f,
        opts.freqMinHz,
        opts.freqMaxHz,
        opts.leakiness,
        opts.baseAFrequencyHz,
        opts.computeMode,
        opts.windowMode,
        opts.normalizationMode,
        opts.windowLengthMode,
        algorithmMode};
    jtune::RollingPitchTracker tracker(
        trackerOpts);

    std::vector<double> hz;
    hz.reserve(signal.size() / 256);
    const size_t startCollect = signal.size() / 2;
    for (size_t i = 0; i < signal.size(); ++i) {
        if (tracker.processSample(signal[i]) && i >= startCollect) {
            const double pitch = tracker.pitchHz();
            if (pitch > 0.0) hz.push_back(pitch);
        }
    }
    if (hz.empty()) return 0.0;
    const auto mid = hz.begin() + static_cast<std::ptrdiff_t>(hz.size() / 2);
    std::nth_element(hz.begin(), mid, hz.end());
    return *mid;
}

double estimateMedianPitchHzCrossValidated(const std::vector<float>& signal,
                                           const jtune::AutotuneOptions& opts,
                                           int dutAlgorithm,
                                           QString* verifierInfo = nullptr)
{
    const int algos[3] = {0, 1, 2};
    std::vector<double> estimates;
    std::vector<QString> used;
    for (int a : algos) {
        if (a == dutAlgorithm) continue;
        const double hz = estimateMedianPitchHzForAlgorithm(signal, opts, a);
        if (hz > 0.0 && std::isfinite(hz)) {
            estimates.push_back(hz);
            if (a == 0) used.push_back("loiacono");
            else if (a == 1) used.push_back("fft");
            else used.push_back("goertzel");
        }
    }
    if (verifierInfo) {
        QString joined;
        for (size_t i = 0; i < used.size(); ++i) {
            if (i > 0) joined += ",";
            joined += used[i];
        }
        *verifierInfo = joined;
    }
    if (estimates.empty()) {
        return estimateMedianPitchHzForAlgorithm(signal, opts, dutAlgorithm);
    }
    if (estimates.size() == 1) {
        return estimates[0];
    }
    std::sort(estimates.begin(), estimates.end());
    return 0.5 * (estimates[0] + estimates[1]);
}

struct SignalMetrics {
    double corr = 0.0;
    double rmse = 0.0;
    double maxAbsErr = 0.0;
};

SignalMetrics compareSignals(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double dot = 0.0;
    double ea = 0.0;
    double eb = 0.0;
    double err2 = 0.0;
    double maxAbs = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(a[i]);
        const double y = static_cast<double>(b[i]);
        dot += x * y;
        ea += x * x;
        eb += y * y;
        const double e = x - y;
        err2 += e * e;
        maxAbs = std::max(maxAbs, std::abs(e));
    }

    SignalMetrics m;
    m.corr = dot / std::sqrt(std::max(1e-12, ea * eb));
    m.rmse = std::sqrt(err2 / std::max<size_t>(1, n));
    m.maxAbsErr = maxAbs;
    return m;
}

struct DeviceEntry {
    unsigned int id = 0;
    QString label;
    unsigned int inChannels = 0;
    unsigned int outChannels = 0;
    bool isDefaultIn = false;
    bool isDefaultOut = false;
};

struct StreamSettings {
    RtAudio::Api api = RtAudio::UNSPECIFIED;
    unsigned int inputDevice = 0;
    unsigned int outputDevice = 0;
    jtune::AutotuneOptions dsp;
    unsigned int bufferFrames = 512;
    unsigned int bufferCount = 0;
    RtAudioStreamFlags audioFlags = 0;
    int historySize = 400;

    int freqMin = 100;
    int freqMax = 3000;
    double leakiness = 0.9995;
    double baseA = 440.0;
    int computeMode = static_cast<int>(LoiaconoRolling::ComputeMode::MultiThread);
    int windowMode = static_cast<int>(LoiaconoRolling::WindowMode::RectangularWindow);
    int normalizationMode = static_cast<int>(LoiaconoRolling::NormalizationMode::Energy);
    int windowLengthMode = static_cast<int>(LoiaconoRolling::WindowLengthMode::PeriodMultiple);
    int algorithmMode = static_cast<int>(LoiaconoRolling::AlgorithmMode::Loiacono);
};

struct AdvancedSettings {
    int freqMin = 100;
    int freqMax = 3000;
    int multiple = 40;
    int bins = 200;
    int sampleRate = 48000;
    int bufferFrames = 512;
    int bufferCount = 0;
    double leakiness = 0.9995;
    double baseA = 440.0;
    int computeMode = static_cast<int>(LoiaconoRolling::ComputeMode::MultiThread);
    int windowMode = static_cast<int>(LoiaconoRolling::WindowMode::RectangularWindow);
    int normalizationMode = static_cast<int>(LoiaconoRolling::NormalizationMode::Energy);
    int windowLengthMode = static_cast<int>(LoiaconoRolling::WindowLengthMode::PeriodMultiple);
    int algorithmMode = static_cast<int>(LoiaconoRolling::AlgorithmMode::Loiacono);
    bool flagNonInterleaved = false;
    bool flagMinimizeLatency = false;
    bool flagHogDevice = false;
    bool flagScheduleRealtime = false;
};

class AdvancedSettingsDialog : public QDialog {
public:
    explicit AdvancedSettingsDialog(const AdvancedSettings& settings, QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("Advanced Settings (Loiacono-Compatible)");
        resize(560, 520);

        auto* root = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        form->setLabelAlignment(Qt::AlignRight);

        freqMin_ = new QSpinBox;
        freqMin_->setRange(20, 12000);
        freqMin_->setValue(settings.freqMin);
        form->addRow("Freq min (Hz)", freqMin_);

        freqMax_ = new QSpinBox;
        freqMax_->setRange(50, 20000);
        freqMax_->setValue(settings.freqMax);
        form->addRow("Freq max (Hz)", freqMax_);

        multiple_ = new QSpinBox;
        multiple_->setRange(2, 240);
        multiple_->setValue(settings.multiple);
        form->addRow("Multiple", multiple_);

        bins_ = new QSpinBox;
        bins_->setRange(32, 2400);
        bins_->setValue(settings.bins);
        form->addRow("Bins", bins_);

        sampleRate_ = new QSpinBox;
        sampleRate_->setRange(8000, 192000);
        sampleRate_->setSingleStep(1000);
        sampleRate_->setValue(settings.sampleRate);
        form->addRow("Sample rate", sampleRate_);

        bufferFrames_ = new QSpinBox;
        bufferFrames_->setRange(64, 4096);
        bufferFrames_->setSingleStep(64);
        bufferFrames_->setValue(settings.bufferFrames);
        form->addRow("Buffer frames", bufferFrames_);

        bufferCount_ = new QSpinBox;
        bufferCount_->setRange(0, 8);
        bufferCount_->setValue(settings.bufferCount);
        form->addRow("Buffer count", bufferCount_);

        leakiness_ = new QDoubleSpinBox;
        leakiness_->setRange(0.99, 1.0);
        leakiness_->setDecimals(5);
        leakiness_->setSingleStep(0.00005);
        leakiness_->setValue(settings.leakiness);
        form->addRow("Leakiness", leakiness_);

        baseA_ = new QDoubleSpinBox;
        baseA_->setRange(400.0, 500.0);
        baseA_->setDecimals(2);
        baseA_->setSingleStep(0.5);
        baseA_->setValue(settings.baseA);
        form->addRow("Base A (Hz)", baseA_);

        computeMode_ = new QComboBox;
        computeMode_->addItem("Single-thread", static_cast<int>(LoiaconoRolling::ComputeMode::SingleThread));
        computeMode_->addItem("Multi-thread", static_cast<int>(LoiaconoRolling::ComputeMode::MultiThread));
        computeMode_->addItem("GPU compute", static_cast<int>(LoiaconoRolling::ComputeMode::GpuCompute));
        computeMode_->addItem("Vulkan compute", static_cast<int>(LoiaconoRolling::ComputeMode::VulkanCompute));
        computeMode_->setCurrentIndex(std::max(0, computeMode_->findData(settings.computeMode)));
        form->addRow("Compute mode", computeMode_);

        windowMode_ = new QComboBox;
        windowMode_->addItem("Rectangular", static_cast<int>(LoiaconoRolling::WindowMode::RectangularWindow));
        windowMode_->addItem("Hann", static_cast<int>(LoiaconoRolling::WindowMode::HannWindow));
        windowMode_->addItem("Hamming", static_cast<int>(LoiaconoRolling::WindowMode::HammingWindow));
        windowMode_->addItem("Blackman", static_cast<int>(LoiaconoRolling::WindowMode::BlackmanWindow));
        windowMode_->addItem("Blackman-Harris", static_cast<int>(LoiaconoRolling::WindowMode::BlackmanHarrisWindow));
        windowMode_->addItem("Leaky", static_cast<int>(LoiaconoRolling::WindowMode::LeakyWindow));
        windowMode_->setCurrentIndex(std::max(0, windowMode_->findData(settings.windowMode)));
        form->addRow("Temporal weighting", windowMode_);

        normalizationMode_ = new QComboBox;
        normalizationMode_->addItem("Raw sum", static_cast<int>(LoiaconoRolling::NormalizationMode::RawSum));
        normalizationMode_->addItem("Coherent amplitude", static_cast<int>(LoiaconoRolling::NormalizationMode::CoherentAmplitude));
        normalizationMode_->addItem("Energy", static_cast<int>(LoiaconoRolling::NormalizationMode::Energy));
        normalizationMode_->setCurrentIndex(std::max(0, normalizationMode_->findData(settings.normalizationMode)));
        form->addRow("Normalization", normalizationMode_);

        windowLengthMode_ = new QComboBox;
        windowLengthMode_->addItem("Constant samples", static_cast<int>(LoiaconoRolling::WindowLengthMode::ConstantSamples));
        windowLengthMode_->addItem("Sqrt period", static_cast<int>(LoiaconoRolling::WindowLengthMode::SqrtPeriod));
        windowLengthMode_->addItem("Period multiple", static_cast<int>(LoiaconoRolling::WindowLengthMode::PeriodMultiple));
        windowLengthMode_->setCurrentIndex(std::max(0, windowLengthMode_->findData(settings.windowLengthMode)));
        form->addRow("Window length mode", windowLengthMode_);

        algorithmMode_ = new QComboBox;
        algorithmMode_->addItem("Loiacono", static_cast<int>(LoiaconoRolling::AlgorithmMode::Loiacono));
        algorithmMode_->addItem("FFT", static_cast<int>(LoiaconoRolling::AlgorithmMode::FFT));
        algorithmMode_->addItem("Goertzel", static_cast<int>(LoiaconoRolling::AlgorithmMode::Goertzel));
        algorithmMode_->setCurrentIndex(std::max(0, algorithmMode_->findData(settings.algorithmMode)));
        form->addRow("Algorithm mode", algorithmMode_);

        flagNonInterleaved_ = new QCheckBox("Non-interleaved");
        flagNonInterleaved_->setChecked(settings.flagNonInterleaved);
        flagMinimizeLatency_ = new QCheckBox("Minimize latency");
        flagMinimizeLatency_->setChecked(settings.flagMinimizeLatency);
        flagHogDevice_ = new QCheckBox("Hog device");
        flagHogDevice_->setChecked(settings.flagHogDevice);
        flagScheduleRealtime_ = new QCheckBox("Schedule realtime");
        flagScheduleRealtime_->setChecked(settings.flagScheduleRealtime);
        auto* flagsBox = new QWidget;
        auto* flagsLay = new QVBoxLayout(flagsBox);
        flagsLay->setContentsMargins(0, 0, 0, 0);
        flagsLay->addWidget(flagNonInterleaved_);
        flagsLay->addWidget(flagMinimizeLatency_);
        flagsLay->addWidget(flagHogDevice_);
        flagsLay->addWidget(flagScheduleRealtime_);
        form->addRow("Audio flags", flagsBox);

        root->addLayout(form);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);
    }

    AdvancedSettings settings() const
    {
        AdvancedSettings s;
        s.freqMin = freqMin_->value();
        s.freqMax = freqMax_->value();
        s.multiple = multiple_->value();
        s.bins = bins_->value();
        s.sampleRate = sampleRate_->value();
        s.bufferFrames = bufferFrames_->value();
        s.bufferCount = bufferCount_->value();
        s.leakiness = leakiness_->value();
        s.baseA = baseA_->value();
        s.computeMode = computeMode_->currentData().toInt();
        s.windowMode = windowMode_->currentData().toInt();
        s.normalizationMode = normalizationMode_->currentData().toInt();
        s.windowLengthMode = windowLengthMode_->currentData().toInt();
        s.algorithmMode = algorithmMode_->currentData().toInt();
        s.flagNonInterleaved = flagNonInterleaved_->isChecked();
        s.flagMinimizeLatency = flagMinimizeLatency_->isChecked();
        s.flagHogDevice = flagHogDevice_->isChecked();
        s.flagScheduleRealtime = flagScheduleRealtime_->isChecked();
        if (s.freqMin >= s.freqMax - 10) s.freqMax = s.freqMin + 10;
        return s;
    }

private:
    QSpinBox* freqMin_ = nullptr;
    QSpinBox* freqMax_ = nullptr;
    QSpinBox* multiple_ = nullptr;
    QSpinBox* bins_ = nullptr;
    QSpinBox* sampleRate_ = nullptr;
    QSpinBox* bufferFrames_ = nullptr;
    QSpinBox* bufferCount_ = nullptr;
    QDoubleSpinBox* leakiness_ = nullptr;
    QDoubleSpinBox* baseA_ = nullptr;
    QComboBox* computeMode_ = nullptr;
    QComboBox* windowMode_ = nullptr;
    QComboBox* normalizationMode_ = nullptr;
    QComboBox* windowLengthMode_ = nullptr;
    QComboBox* algorithmMode_ = nullptr;
    QCheckBox* flagNonInterleaved_ = nullptr;
    QCheckBox* flagMinimizeLatency_ = nullptr;
    QCheckBox* flagHogDevice_ = nullptr;
    QCheckBox* flagScheduleRealtime_ = nullptr;
};

class PitchHistoryWidget : public QWidget {
public:
    explicit PitchHistoryWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(260);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Click to open the dedicated pitch monitor");
    }

    void setClickHandler(std::function<void()> handler)
    {
        clickHandler_ = std::move(handler);
    }

    void setData(const std::vector<float>& in,
                 const std::vector<float>& out,
                 const std::vector<std::vector<float>>& spectra,
                 const std::vector<double>& expectedNoteHz,
                 int historySize,
                 double spectrumMinHz,
                 double spectrumMaxHz,
                 int minMidi,
                 int maxMidi)
    {
        in_ = in;
        out_ = out;
        spectra_ = spectra;
        expectedNoteHz_ = expectedNoteHz;
        historySize_ = std::max(2, historySize);
        spectrumMinHz_ = std::max(1.0, spectrumMinHz);
        spectrumMaxHz_ = std::max(spectrumMinHz_ * 1.01, spectrumMaxHz);
        minMidi_ = minMidi;
        maxMidi_ = std::max(minMidi + 1, maxMidi);
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && clickHandler_) clickHandler_();
        QWidget::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 22, 28));

        const QRectF graph = rect().adjusted(48, 16, -16, -28);
        p.setPen(QColor(65, 75, 90));
        p.drawRect(graph);

        const double graphMinHz = expectedNoteHz_.empty()
            ? spectrumMinHz_
            : expectedNoteHz_.front();
        const double graphMaxHz = std::max(graphMinHz * 1.01, expectedNoteHz_.empty()
            ? spectrumMaxHz_
            : expectedNoteHz_.back());
        const double graphLogMin = std::log(std::max(1.0e-6, graphMinHz));
        const double graphLogSpan = std::max(1.0e-9, std::log(graphMaxHz) - graphLogMin);
        auto frequencyY = [&](double hz) {
            const double t = std::clamp(
                (std::log(std::max(1.0e-6, hz)) - graphLogMin) / graphLogSpan, 0.0, 1.0);
            return graph.bottom() - t * graph.height();
        };

        auto mapY = [&](float hz) {
            if (hz <= 0.0f) return std::numeric_limits<double>::quiet_NaN();
            return frequencyY(hz);
        };

        if (!spectra_.empty() && graph.width() > 1.0 && graph.height() > 1.0) {
            const int imageHeight = std::max(1, static_cast<int>(std::lround(graph.height())));
            QImage spectrogram(historySize_, imageHeight, QImage::Format_ARGB32);
            spectrogram.fill(QColor(18, 22, 28));
            float peak = 0.0f;
            for (const auto& column : spectra_) {
                for (float amplitude : column) peak = std::max(peak, amplitude);
            }
            peak = std::max(peak, 1.0e-9f);
            const size_t visibleSize = std::min(spectra_.size(), static_cast<size_t>(historySize_));
            const size_t first = spectra_.size() - visibleSize;
            const int firstSlot = historySize_ - static_cast<int>(visibleSize);
            const double logMin = std::log(spectrumMinHz_);
            const double logSpan = std::log(spectrumMaxHz_) - logMin;
            auto colorFor = [&](float amplitude) {
                float t = std::log1p(9.0f * std::max(0.0f, amplitude) / peak) / std::log(10.0f);
                t = t < 0.05f ? 0.0f : std::pow(t, 0.6f);
                const float r = std::clamp(1.5f * t - 0.5f, 0.0f, 1.0f);
                const float g = std::clamp(1.5f - std::abs(3.0f * t - 1.5f), 0.0f, 1.0f);
                const float b = std::clamp(1.0f - 1.5f * t, 0.0f, 1.0f);
                return qRgba(static_cast<int>(255.0f * r), static_cast<int>(255.0f * g),
                             static_cast<int>(255.0f * b), 220);
            };
            for (size_t i = first; i < spectra_.size(); ++i) {
                const auto& column = spectra_[i];
                if (column.empty()) continue;
                const int x = firstSlot + static_cast<int>(i - first);
                for (int y = 0; y < imageHeight; ++y) {
                    const double graphT = 1.0 -
                        static_cast<double>(y) / std::max(1, imageHeight - 1);
                    const double hz = std::exp(graphLogMin + graphT * graphLogSpan);
                    if (hz < spectrumMinHz_ || hz > spectrumMaxHz_) continue;
                    const double binT = (std::log(hz) - logMin) / logSpan;
                    const size_t bin = static_cast<size_t>(std::clamp(
                        std::lround(binT * static_cast<double>(column.size() - 1)),
                        0L, static_cast<long>(column.size() - 1)));
                    spectrogram.setPixel(x, y, colorFor(column[bin]));
                }
            }
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(graph, spectrogram);
        }

        // Draw the evaluated targets of the selected tuning system. A measured
        // pitch on target lands exactly on its guide, including low registers.
        for (double expectedHz : expectedNoteHz_) {
            const double y = frequencyY(expectedHz);
            p.setPen(QColor(75, 85, 102, 190));
            p.drawLine(QPointF(graph.left(), y), QPointF(graph.right(), y));
        }
        p.setPen(QColor(90, 100, 118));
        p.drawRect(graph);

        auto drawSeries = [&](const std::vector<float>& v, const QColor& c) {
            if (v.size() < 2) return;
            QPainterPath path;
            bool hasStarted = false;
            const size_t visibleSize = std::min(v.size(), static_cast<size_t>(historySize_));
            const size_t first = v.size() - visibleSize;
            const double firstSlot = static_cast<double>(historySize_ - static_cast<int>(visibleSize));
            for (size_t i = first; i < v.size(); ++i) {
                const double y = mapY(v[i]);
                if (!std::isfinite(y)) {
                    hasStarted = false;
                    continue;
                }
                const double slot = firstSlot + static_cast<double>(i - first);
                const double x = graph.left() +
                    (slot / static_cast<double>(historySize_ - 1)) * graph.width();
                if (!hasStarted) {
                    path.moveTo(x, y);
                    hasStarted = true;
                } else {
                    path.lineTo(x, y);
                }
            }
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(c, 3.0));
            p.drawPath(path);
            if (hasStarted) {
                const double y = mapY(v.back());
                if (std::isfinite(y)) {
                    p.setPen(QPen(QColor(18, 22, 28), 2.0));
                    p.setBrush(c);
                    p.drawEllipse(QPointF(graph.right(), y), 5.0, 5.0);
                }
            }
        };

        drawSeries(in_, QColor(79, 195, 247));
        drawSeries(out_, QColor(255, 167, 38));

        p.setPen(QColor(210, 220, 235));
        p.drawText(QPointF(2, graph.top() + 6), QString("%1 Hz").arg(graphMaxHz, 0, 'f', 1));
        p.drawText(QPointF(2, graph.bottom() + 4), QString("%1 Hz").arg(graphMinHz, 0, 'f', 1));

        p.setPen(QColor(79, 195, 247));
        p.drawText(QPointF(graph.left(), height() - 8), "Input pitch");
        p.setPen(QColor(255, 167, 38));
        p.drawText(QPointF(graph.left() + 95, height() - 8), "Measured output pitch");
    }

private:
    std::vector<float> in_;
    std::vector<float> out_;
    std::vector<std::vector<float>> spectra_;
    std::vector<double> expectedNoteHz_;
    int historySize_ = 500;
    double spectrumMinHz_ = 100.0;
    double spectrumMaxHz_ = 3000.0;
    int minMidi_ = 40;
    int maxMidi_ = 84;
    std::function<void()> clickHandler_;
};

class AudioEngine {
public:
    ~AudioEngine() { stop(); }

    std::vector<DeviceEntry> queryDevices(RtAudio::Api api, QString& error) const
    {
        std::vector<DeviceEntry> out;
        try {
            RtAudio audio(api);
            const auto ids = audio.getDeviceIds();
            for (unsigned int id : ids) {
                auto info = audio.getDeviceInfo(id);
                DeviceEntry d;
                d.id = id;
                d.inChannels = info.inputChannels;
                d.outChannels = info.outputChannels;
                d.isDefaultIn = info.isDefaultInput;
                d.isDefaultOut = info.isDefaultOutput;
                d.label = displayNameForDevice(
                    apiName(api),
                    QString::fromStdString(info.name),
                    info.inputChannels,
                    info.outputChannels);
                out.push_back(std::move(d));
            }
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        return out;
    }

    bool start(const StreamSettings& settings, QString& error)
    {
        stop();

        try {
            auto rtAudio = std::make_unique<RtAudio>(settings.api);
            if (rtAudio->getDeviceCount() < 1) {
                error = "No audio devices available.";
                return false;
            }

            auto st = std::make_unique<State>();
            st->settings = settings;
            st->historyLimit = settings.historySize;
            st->processor = std::make_unique<jtune::ConstantQAutotuneProcessor>(settings.dsp);

            const int trackerBins = std::clamp(settings.dsp.binCount / 2, 48, 160);
            const int trackerHop = std::max(settings.dsp.analysisHop * 2, 192);
            constexpr unsigned int trackerDecimation = 4;
            const unsigned int trackerSampleRate =
                std::max(1u, settings.dsp.sampleRate / trackerDecimation);
            st->inTracker = std::make_unique<jtune::RollingPitchTracker>(
                jtune::PitchTrackerOptions{
                    trackerSampleRate,
                    settings.dsp.minMidi,
                    settings.dsp.maxMidi,
                    settings.dsp.multiple,
                    trackerBins,
                    trackerHop,
                    0.20f,
                    settings.dsp.freqMinHz,
                    settings.dsp.freqMaxHz,
                    settings.dsp.leakiness,
                    settings.dsp.baseAFrequencyHz,
                    settings.dsp.computeMode,
                    settings.dsp.windowMode,
                    settings.dsp.normalizationMode,
                    settings.dsp.windowLengthMode,
                    settings.dsp.algorithmMode});
            st->outTracker = std::make_unique<jtune::RollingPitchTracker>(
                jtune::PitchTrackerOptions{
                    trackerSampleRate,
                    settings.dsp.minMidi,
                    settings.dsp.maxMidi,
                    settings.dsp.multiple,
                    trackerBins,
                    trackerHop,
                    0.20f,
                    settings.dsp.freqMinHz,
                    settings.dsp.freqMaxHz,
                    settings.dsp.leakiness,
                    settings.dsp.baseAFrequencyHz,
                    settings.dsp.computeMode,
                    settings.dsp.windowMode,
                    settings.dsp.normalizationMode,
                    settings.dsp.windowLengthMode,
                    settings.dsp.algorithmMode});
            st->trackerDecimation = static_cast<int>(trackerDecimation);
            RtAudio::StreamParameters input;
            RtAudio::StreamParameters output;
            input.deviceId = settings.inputDevice;
            output.deviceId = settings.outputDevice;
            input.nChannels = 1;
            output.nChannels = 1;
            input.firstChannel = 0;
            output.firstChannel = 0;

            unsigned int frames = settings.bufferFrames;
            RtAudio::StreamOptions streamOptions;
            streamOptions.flags = settings.audioFlags;
            streamOptions.numberOfBuffers = settings.bufferCount;
            streamOptions.streamName = "JTune Qt GUI";
            if (settings.audioFlags & RTAUDIO_SCHEDULE_REALTIME) {
                streamOptions.priority = 10;
            }
            rtAudio->openStream(
                &output,
                &input,
                RTAUDIO_FLOAT32,
                settings.dsp.sampleRate,
                &frames,
                &AudioEngine::audioCallback,
                st.get(),
                &streamOptions);
            rtAudio->startStream();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_ = std::move(rtAudio);
                state_ = std::move(st);
                running_ = true;
            }
            return true;
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
            stop();
            return false;
        }
    }

    void stop()
    {
        std::unique_ptr<RtAudio> audio;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            audio = std::move(audio_);
            state_.reset();
        }

        if (audio) {
            try {
                if (audio->isStreamRunning()) audio->stopStream();
                if (audio->isStreamOpen()) audio->closeStream();
            } catch (...) {
            }
        }
    }

    bool isRunning() const
    {
        return running_.load();
    }

    struct Snapshot {
        std::vector<float> in;
        std::vector<float> out;
        std::vector<std::vector<float>> spectra;
        int historySize = 500;
        double spectrumMinHz = 100.0;
        double spectrumMaxHz = 3000.0;
        float inHz = 0.0f;
        float outHz = 0.0f;
        float ratio = 1.0f;
        uint64_t underruns = 0;
    };

    Snapshot snapshot() const
    {
        Snapshot snap;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_) return snap;
        std::lock_guard<std::mutex> dataLock(state_->dataMutex);
        snap.in.assign(state_->inHistory.begin(), state_->inHistory.end());
        snap.out.assign(state_->outHistory.begin(), state_->outHistory.end());
        snap.spectra.assign(state_->spectrumHistory.begin(), state_->spectrumHistory.end());
        snap.historySize = state_->historyLimit;
        snap.spectrumMinHz = state_->settings.dsp.freqMinHz;
        snap.spectrumMaxHz = state_->settings.dsp.freqMaxHz;
        snap.inHz = state_->latestInHz;
        snap.outHz = state_->latestOutHz;
        snap.ratio = state_->latestRatio;
        snap.underruns = state_->underruns;
        return snap;
    }

private:
    struct State {
        StreamSettings settings;
        std::unique_ptr<jtune::ConstantQAutotuneProcessor> processor;
        std::unique_ptr<jtune::RollingPitchTracker> inTracker;
        std::unique_ptr<jtune::RollingPitchTracker> outTracker;
        int trackerCounter = 0;
        int trackerDecimation = 4;

        std::deque<float> inHistory;
        std::deque<float> outHistory;
        std::deque<std::vector<float>> spectrumHistory;
        int historyLimit = 400;

        float latestInHz = 0.0f;
        float latestOutHz = 0.0f;
        float latestRatio = 1.0f;
        uint64_t underruns = 0;

        mutable std::mutex dataMutex;
    };

    static int audioCallback(void* outputBuffer,
                             void* inputBuffer,
                             unsigned int nFrames,
                             double,
                             RtAudioStreamStatus status,
                             void* userData)
    {
        auto* st = static_cast<State*>(userData);
        auto* in = static_cast<const float*>(inputBuffer);
        auto* out = static_cast<float*>(outputBuffer);
        if (!out || !st || !st->processor) return 0;

        if (status) {
            std::lock_guard<std::mutex> lock(st->dataMutex);
            st->underruns++;
        }

        for (unsigned int i = 0; i < nFrames; ++i) {
            const float sIn = in ? in[i] : 0.0f;
            const float sOut = st->processor->processSample(sIn);
            out[i] = sOut;

            st->trackerCounter++;
            if (st->trackerCounter >= st->trackerDecimation) {
                st->trackerCounter = 0;
                const bool inUpdated = st->inTracker->processSample(sIn);
                // Always measure the samples sent to the output device. Even
                // passthrough can differ because of the active wet/dry and
                // synthesis path, so deriving this from input is misleading.
                const bool outUpdated = st->outTracker->processSample(sOut);
                if (inUpdated || outUpdated) {
                    std::vector<float> spectrum;
                    st->processor->copyCurrentSpectrum(spectrum);
                    std::lock_guard<std::mutex> lock(st->dataMutex);
                    st->latestInHz = static_cast<float>(st->inTracker->pitchHz());
                    st->latestOutHz = static_cast<float>(st->outTracker->pitchHz());
                    st->latestRatio = st->processor->currentPitchRatio();
                    st->inHistory.push_back(st->latestInHz);
                    st->outHistory.push_back(st->latestOutHz);
                    st->spectrumHistory.push_back(std::move(spectrum));
                    while (static_cast<int>(st->inHistory.size()) > st->historyLimit) st->inHistory.pop_front();
                    while (static_cast<int>(st->outHistory.size()) > st->historyLimit) st->outHistory.pop_front();
                    while (static_cast<int>(st->spectrumHistory.size()) > st->historyLimit) st->spectrumHistory.pop_front();
                }
            }
        }

        return 0;
    }

    mutable std::mutex mutex_;
    std::unique_ptr<RtAudio> audio_;
    std::unique_ptr<State> state_;
    std::atomic<bool> running_{false};
};

class MainWindow : public QWidget {
public:
    MainWindow()
    {
        setWindowTitle("JTune Live GUI");
        resize(1280, 760);

        auto* root = new QHBoxLayout(this);
        auto* splitter = new QSplitter(Qt::Horizontal, this);
        root->addWidget(splitter);

        auto* leftPane = new QWidget;
        auto* leftRoot = new QVBoxLayout(leftPane);

        auto* controlBox = new QGroupBox("Core Controls");
        auto* grid = new QGridLayout(controlBox);

        apiCombo_ = new QComboBox;
        apiCombo_->addItem("Unspecified", static_cast<int>(RtAudio::UNSPECIFIED));
        apiCombo_->addItem("Pulse", static_cast<int>(RtAudio::LINUX_PULSE));
        apiCombo_->addItem("ALSA", static_cast<int>(RtAudio::LINUX_ALSA));
        apiCombo_->addItem("JACK", static_cast<int>(RtAudio::UNIX_JACK));

        inputCombo_ = new QComboBox;
        outputCombo_ = new QComboBox;

        profileCombo_ = new QComboBox;
        profileCombo_->addItem("Balanced");
        profileCombo_->addItem("Low Notes Stable");
        profileCombo_->addItem("Low Notes Aggressive");
        profileCombo_->addItem("High Notes Fast");

        tuningCombo_ = new QComboBox;
        const auto& pitchSystems = jtune::pitchSystemRegistry().definitions();
        for (int i = 0; i < static_cast<int>(pitchSystems.size()); ++i)
            tuningCombo_->addItem(QString::fromStdString(pitchSystems[static_cast<size_t>(i)].displayName),
                                  QString::fromStdString(pitchSystems[static_cast<size_t>(i)].id));
        collectionCombo_ = new QComboBox;
        for (const auto& collection : jtune::builtInPitchCollections())
            collectionCombo_->addItem(QString::fromStdString(collection.displayName),
                                      QString::fromStdString(collection.id));
        tonicNoteCombo_ = new QComboBox;
        static const char* tonicNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        for (int midi = 0; midi < 128; ++midi)
            tonicNoteCombo_->addItem(QStringLiteral("%1%2 (MIDI %3)")
                .arg(tonicNames[midi % 12]).arg(midi / 12 - 1).arg(midi), midi);
        tonicNoteCombo_->setCurrentIndex(60);
        auto* editDegreesButton = new QPushButton("Edit active degrees");
        connect(editDegreesButton, &QPushButton::clicked, this, [this]() {
            const auto* definition = jtune::pitchSystemRegistry().byId(
                tuningCombo_->currentData().toString().toStdString());
            if (!definition) return;
            QDialog dialog(this);
            dialog.setWindowTitle("Active correction targets");
            auto* layout = new QVBoxLayout(&dialog);
            layout->addWidget(new QLabel("Degree offsets from tonic; the tuning system is unchanged."));
            std::vector<QCheckBox*> checks;
            for (size_t degree = 0; degree < definition->targets.size(); ++degree) {
                const QString name = degree == 0 ? "0 — tonic / 1:1"
                    : QString("%1 — %2").arg(degree).arg(QString::fromStdString(definition->targets[degree - 1].name));
                auto* check = new QCheckBox(name);
                check->setChecked(std::find(customEnabledDegrees_.begin(), customEnabledDegrees_.end(),
                    static_cast<int>(degree)) != customEnabledDegrees_.end());
                checks.push_back(check);
                layout->addWidget(check);
            }
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            if (dialog.exec() != QDialog::Accepted) return;
            customEnabledDegrees_.clear();
            for (size_t i = 0; i < checks.size(); ++i)
                if (checks[i]->isChecked()) customEnabledDegrees_.push_back(static_cast<int>(i));
            collectionCombo_->setCurrentIndex(collectionCombo_->findData("custom"));
            QVariantList saved;
            for (int degree : customEnabledDegrees_) saved << degree;
            QSettings("JTune", "JTune").setValue("pitch_collection/custom_degrees", saved);
        });
        auto* importPitchSystemButton = new QPushButton("Import .scl/.json");
        auto* pitchSystemDetailsButton = new QPushButton("Details");
        connect(importPitchSystemButton, &QPushButton::clicked, this, [this]() {
            const QStringList paths = QFileDialog::getOpenFileNames(
                this, "Import Pitch Systems", QString(), "Pitch systems (*.scl *.json)");
            if (paths.isEmpty()) return;
            QStringList failures;
            QString lastImportedPath;
            for (const QString& path : paths) {
                std::vector<std::string> errors;
                if (!jtune::pitchSystemRegistry().loadFile(path.toStdString(), errors)) {
                    for (const auto& error : errors)
                        failures << QString("%1: %2").arg(path, QString::fromStdString(error));
                    continue;
                }
                const auto& definition = jtune::pitchSystemRegistry().definitions().back();
                tuningCombo_->addItem(QString::fromStdString(definition.displayName),
                                      QString::fromStdString(definition.id));
                lastImportedPath = path;
            }
            if (lastImportedPath.isEmpty()) {
                QMessageBox::critical(this, "Pitch System Import Failed", failures.join('\n'));
                return;
            }
            tuningCombo_->setCurrentIndex(tuningCombo_->count() - 1);
            QSettings settings("JTune", "JTune");
            settings.setValue("pitch_system/file", lastImportedPath);
            settings.setValue("pitch_system/id", tuningCombo_->currentData().toString());
            if (!failures.isEmpty())
                QMessageBox::warning(this, "Some Pitch Systems Were Not Imported", failures.join('\n'));
        });
        connect(tuningCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            const auto* definition = jtune::pitchSystemRegistry().byIndex(index);
            if (!definition) return;
            tuningCombo_->setToolTip(QString::fromStdString(
                definition->scope + "\nCorrection: " +
                (definition->correctionEligible ? "enabled" : "disabled (reference only)") +
                "\nUse: " + definition->appropriateUse + "\nLimits: " + definition->limitations));
            const QString previous = collectionCombo_->currentData().toString();
            collectionCombo_->clear();
            for (const auto& collection : jtune::builtInPitchCollections()) {
                if (collection.degreeCount != 0 &&
                    collection.degreeCount != static_cast<int>(definition->targets.size())) continue;
                collectionCombo_->addItem(QString::fromStdString(collection.displayName),
                                          QString::fromStdString(collection.id));
            }
            const int restored = collectionCombo_->findData(previous);
            collectionCombo_->setCurrentIndex(restored >= 0 ? restored : 0);
        });
        connect(pitchSystemDetailsButton, &QPushButton::clicked, this, [this]() {
            const auto* definition = jtune::pitchSystemRegistry().byId(tuningCombo_->currentData().toString().toStdString());
            if (!definition) return;
            QString details = QString("ID: %1\nVersion: %2\nModel: %3\nTradition: %4\nRegion/locality: %5 / %6\nScope: %7\nAuthor/community: %8\nEnsemble: %9\nInstrument: %10\nReviewed: %11\n\nAppropriate use: %12\n\nLimitations: %13\n\nSources:\n")
                .arg(QString::fromStdString(definition->id), QString::fromStdString(definition->version))
                .arg(static_cast<int>(definition->modelType))
                .arg(QString::fromStdString(definition->tradition), QString::fromStdString(definition->region),
                     QString::fromStdString(definition->locality), QString::fromStdString(definition->scope),
                     QString::fromStdString(definition->authorOrCommunity), QString::fromStdString(definition->ensemble),
                     QString::fromStdString(definition->instrument))
                .arg(definition->reviewed ? "yes" : "no")
                .arg(QString::fromStdString(definition->appropriateUse), QString::fromStdString(definition->limitations));
            details.prepend(QString("Correction eligible: %1\n").arg(
                definition->correctionEligible ? "yes" : "no — visualization/reference only"));
            details += QString("\nSource hash: %1\nCorrection range: %2 cents\nReviews:\n")
                .arg(QString::fromStdString(definition->sourceHash))
                .arg(definition->correctionRangeCents > 0.0 ? definition->correctionRangeCents : 200.0);
            for (const auto& review : definition->reviews)
                details += QString("• %1 — %2 (%3)\n  %4\n")
                    .arg(QString::fromStdString(review.reviewer),
                         QString::fromStdString(review.scope), QString::fromStdString(review.date),
                         QString::fromStdString(review.evidenceUrl));
            details += "\nTargets:\n";
            for (const auto& target : definition->targets) {
                details += QString("• %1 [%2] ratio=%3 cents=%4 Hz=%5 uncertainty=±%6c confidence=%7 direction=%8 register=%9..%10 paired=%11 beat=%12Hz\n")
                    .arg(QString::fromStdString(target.name), QString::fromStdString(target.id))
                    .arg(target.ratio, 0, 'g', 10).arg(target.cents, 0, 'f', 3)
                    .arg(target.absoluteHz, 0, 'f', 3).arg(target.uncertaintyCents, 0, 'f', 2)
                    .arg(target.confidence, 0, 'f', 3).arg(static_cast<int>(target.direction))
                    .arg(target.registerMin).arg(target.registerMax)
                    .arg(QString::fromStdString(target.pairedTargetId)).arg(target.beatRateHz, 0, 'f', 3);
            }
            for (const auto& source : definition->sources)
                details += QString("• %1\n  %2\n  License: %3\n").arg(QString::fromStdString(source.citation),
                    QString::fromStdString(source.url), QString::fromStdString(source.license));
            QMessageBox::information(this, "Pitch System Provenance", details);
        });
        referenceNoteCombo_ = new QComboBox;
        static const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        for (int midi = 0; midi < 128; ++midi) {
            const QString label = QStringLiteral("%1%2 (MIDI %3)")
                .arg(noteNames[midi % 12]).arg(midi / 12 - 1).arg(midi);
            referenceNoteCombo_->addItem(label, midi);
        }
        referenceNoteCombo_->setCurrentIndex(69);
        octaveShiftSpin_ = new QSpinBox;
        octaveShiftSpin_->setRange(-2, 2);
        octaveShiftSpin_->setValue(0);
        octaveShiftSpin_->setSuffix(" oct");

        strengthSlider_ = new QSlider(Qt::Horizontal);
        strengthSlider_->setRange(0, 100);
        strengthSlider_->setValue(100);
        strengthValue_ = new QLabel("1.00");

        sampleRateSpin_ = new QSpinBox;
        sampleRateSpin_->setRange(8000, 192000);
        sampleRateSpin_->setSingleStep(1000);
        sampleRateSpin_->setValue(48000);

        bufferSpin_ = new QSpinBox;
        bufferSpin_->setRange(64, 4096);
        bufferSpin_->setSingleStep(64);
        bufferSpin_->setValue(512);

        minMidiSpin_ = new QSpinBox;
        minMidiSpin_->setRange(0, 127);
        minMidiSpin_->setValue(40);

        maxMidiSpin_ = new QSpinBox;
        maxMidiSpin_->setRange(0, 127);
        maxMidiSpin_->setValue(84);

        binsSpin_ = new QSpinBox;
        binsSpin_->setRange(24, 2048);
        binsSpin_->setValue(128);

        hopSpin_ = new QSpinBox;
        hopSpin_->setRange(16, 2048);
        hopSpin_->setValue(256);

        wetDrySlider_ = new QSlider(Qt::Horizontal);
        wetDrySlider_->setRange(0, 100);
        wetDrySlider_->setValue(100);
        wetDryValue_ = new QLabel("1.00");

        multipleSpin_ = new QSpinBox;
        multipleSpin_->setRange(2, 240);
        multipleSpin_->setValue(advanced_.multiple);

        freqMinSpin_ = new QSpinBox;
        freqMinSpin_->setRange(20, 12000);
        freqMinSpin_->setValue(advanced_.freqMin);
        freqMaxSpin_ = new QSpinBox;
        freqMaxSpin_->setRange(50, 20000);
        freqMaxSpin_->setValue(advanced_.freqMax);

        leakinessSpin_ = new QDoubleSpinBox;
        leakinessSpin_->setRange(0.99, 1.0);
        leakinessSpin_->setDecimals(5);
        leakinessSpin_->setSingleStep(0.00005);
        leakinessSpin_->setValue(advanced_.leakiness);

        baseASpin_ = new QDoubleSpinBox;
        baseASpin_->setRange(1.0, 20000.0);
        baseASpin_->setDecimals(2);
        baseASpin_->setSingleStep(0.5);
        baseASpin_->setValue(advanced_.baseA);

        voicedThresholdSpin_ = new QDoubleSpinBox;
        voicedThresholdSpin_->setRange(0.01, 2.0);
        voicedThresholdSpin_->setDecimals(2);
        voicedThresholdSpin_->setSingleStep(0.01);
        voicedThresholdSpin_->setValue(0.20);

        bufferCountSpin_ = new QSpinBox;
        bufferCountSpin_->setRange(0, 8);
        bufferCountSpin_->setValue(advanced_.bufferCount);

        computeModeCombo_ = new QComboBox;
        computeModeCombo_->addItem("Single-thread", static_cast<int>(LoiaconoRolling::ComputeMode::SingleThread));
        computeModeCombo_->addItem("Multi-thread", static_cast<int>(LoiaconoRolling::ComputeMode::MultiThread));
        computeModeCombo_->addItem("GPU compute", static_cast<int>(LoiaconoRolling::ComputeMode::GpuCompute));
        computeModeCombo_->addItem("Vulkan compute", static_cast<int>(LoiaconoRolling::ComputeMode::VulkanCompute));
        computeModeCombo_->setCurrentIndex(std::max(0, computeModeCombo_->findData(advanced_.computeMode)));

        windowModeCombo_ = new QComboBox;
        windowModeCombo_->addItem("Rectangular", static_cast<int>(LoiaconoRolling::WindowMode::RectangularWindow));
        windowModeCombo_->addItem("Hann", static_cast<int>(LoiaconoRolling::WindowMode::HannWindow));
        windowModeCombo_->addItem("Hamming", static_cast<int>(LoiaconoRolling::WindowMode::HammingWindow));
        windowModeCombo_->addItem("Blackman", static_cast<int>(LoiaconoRolling::WindowMode::BlackmanWindow));
        windowModeCombo_->addItem("Blackman-Harris", static_cast<int>(LoiaconoRolling::WindowMode::BlackmanHarrisWindow));
        windowModeCombo_->addItem("Leaky", static_cast<int>(LoiaconoRolling::WindowMode::LeakyWindow));
        windowModeCombo_->setCurrentIndex(std::max(0, windowModeCombo_->findData(advanced_.windowMode)));

        normalizationModeCombo_ = new QComboBox;
        normalizationModeCombo_->addItem("Raw sum", static_cast<int>(LoiaconoRolling::NormalizationMode::RawSum));
        normalizationModeCombo_->addItem("Coherent amplitude", static_cast<int>(LoiaconoRolling::NormalizationMode::CoherentAmplitude));
        normalizationModeCombo_->addItem("Energy", static_cast<int>(LoiaconoRolling::NormalizationMode::Energy));
        normalizationModeCombo_->setCurrentIndex(std::max(0, normalizationModeCombo_->findData(advanced_.normalizationMode)));

        windowLengthModeCombo_ = new QComboBox;
        windowLengthModeCombo_->addItem("Constant samples", static_cast<int>(LoiaconoRolling::WindowLengthMode::ConstantSamples));
        windowLengthModeCombo_->addItem("Sqrt period", static_cast<int>(LoiaconoRolling::WindowLengthMode::SqrtPeriod));
        windowLengthModeCombo_->addItem("Period multiple", static_cast<int>(LoiaconoRolling::WindowLengthMode::PeriodMultiple));
        windowLengthModeCombo_->setCurrentIndex(std::max(0, windowLengthModeCombo_->findData(advanced_.windowLengthMode)));

        algorithmModeCombo_ = new QComboBox;
        algorithmModeCombo_->addItem("Loiacono", static_cast<int>(LoiaconoRolling::AlgorithmMode::Loiacono));
        algorithmModeCombo_->addItem("FFT", static_cast<int>(LoiaconoRolling::AlgorithmMode::FFT));
        algorithmModeCombo_->addItem("Goertzel", static_cast<int>(LoiaconoRolling::AlgorithmMode::Goertzel));
        algorithmModeCombo_->setCurrentIndex(std::max(0, algorithmModeCombo_->findData(advanced_.algorithmMode)));

        resynthModeCombo_ = new QComboBox;
        resynthModeCombo_->addItem("FrequencyDomain", jtune::FrequencyDomain);
        resynthModeCombo_->addItem("TimeDomain", jtune::TimeDomain);
        resynthModeCombo_->setCurrentIndex(std::max(0, resynthModeCombo_->findData(jtune::TimeDomain)));

        ratioSmoothingSpin_ = new QDoubleSpinBox;
        ratioSmoothingSpin_->setRange(0.01, 1.0);
        ratioSmoothingSpin_->setDecimals(3);
        ratioSmoothingSpin_->setSingleStep(0.01);
        // A short correction glide is the normal musical behavior. The hard
        // tune profile can still select 1.0 for an instantaneous note lock.
        ratioSmoothingSpin_->setValue(0.15);

        amplitudeSmoothingSpin_ = new QDoubleSpinBox;
        amplitudeSmoothingSpin_->setRange(0.01, 1.0);
        amplitudeSmoothingSpin_->setDecimals(3);
        amplitudeSmoothingSpin_->setSingleStep(0.01);
        amplitudeSmoothingSpin_->setValue(0.15);

        phasePullSpin_ = new QDoubleSpinBox;
        phasePullSpin_->setRange(0.0, 1.0);
        phasePullSpin_->setDecimals(3);
        phasePullSpin_->setSingleStep(0.01);
        phasePullSpin_->setValue(0.08);

        flowGrainMsSpin_ = new QSpinBox;
        flowGrainMsSpin_->setRange(5, 80);
        flowGrainMsSpin_->setValue(20);

        flowOverlapSpin_ = new QDoubleSpinBox;
        flowOverlapSpin_->setRange(0.10, 0.95);
        flowOverlapSpin_->setDecimals(2);
        flowOverlapSpin_->setSingleStep(0.01);
        flowOverlapSpin_->setValue(0.75);

        flowBaseDelayMsSpin_ = new QSpinBox;
        flowBaseDelayMsSpin_->setRange(5, 200);
        flowBaseDelayMsSpin_->setValue(40);

        flowDriftCorrectionSpin_ = new QDoubleSpinBox;
        flowDriftCorrectionSpin_->setRange(0.0, 0.2);
        flowDriftCorrectionSpin_->setDecimals(3);
        flowDriftCorrectionSpin_->setSingleStep(0.005);
        flowDriftCorrectionSpin_->setValue(0.01);

        nonInterleavedCheck_ = new QCheckBox("Non-interleaved");
        nonInterleavedCheck_->setChecked(advanced_.flagNonInterleaved);
        minimizeLatencyCheck_ = new QCheckBox("Minimize latency");
        minimizeLatencyCheck_->setChecked(advanced_.flagMinimizeLatency);
        hogDeviceCheck_ = new QCheckBox("Hog device");
        hogDeviceCheck_->setChecked(advanced_.flagHogDevice);
        scheduleRealtimeCheck_ = new QCheckBox("Realtime");
        scheduleRealtimeCheck_->setChecked(advanced_.flagScheduleRealtime);

        startStopButton_ = new QPushButton("Start");
        refreshButton_ = new QPushButton("Refresh Devices");
        chirpTestButton_ = new QPushButton("Run Chirp Test");
        advancedToggleButton_ = new QPushButton("Show Advanced >>");
        advancedToggleButton_->setCheckable(true);

        statusLabel_ = new QLabel("Idle");
        pitchLabel_ = new QLabel("in: 0 Hz   out: 0 Hz   ratio: 1.00   underruns: 0");

        int r = 0;
        grid->addWidget(new QLabel("API"), r, 0);
        grid->addWidget(apiCombo_, r, 1);
        grid->addWidget(refreshButton_, r, 2);
        grid->addWidget(startStopButton_, r, 3);
        grid->addWidget(chirpTestButton_, r, 4);
        grid->addWidget(statusLabel_, r, 5, 1, 2);

        r++;
        grid->addWidget(new QLabel("Input Device"), r, 0);
        grid->addWidget(inputCombo_, r, 1, 1, 2);
        grid->addWidget(new QLabel("Output Device"), r, 3);
        grid->addWidget(outputCombo_, r, 4, 1, 2);

        r++;
        grid->addWidget(new QLabel("Reference Note"), r, 0);
        grid->addWidget(referenceNoteCombo_, r, 1, 1, 3);
        grid->addWidget(new QLabel("Profile"), r, 4);
        grid->addWidget(profileCombo_, r, 5);
        grid->addWidget(advancedToggleButton_, r, 6, 1, 2);

        r++;
        grid->addWidget(new QLabel("Pitch System"), r, 0);
        grid->addWidget(tuningCombo_, r, 1, 1, 5);
        grid->addWidget(new QLabel("Octave Shift"), r, 6);
        grid->addWidget(octaveShiftSpin_, r, 7);
        grid->addWidget(importPitchSystemButton, r, 8);
        grid->addWidget(pitchSystemDetailsButton, r, 9);

        r++;
        grid->addWidget(new QLabel("Pitch Collection"), r, 0);
        grid->addWidget(collectionCombo_, r, 1, 1, 2);
        grid->addWidget(new QLabel("Tonic"), r, 3);
        grid->addWidget(tonicNoteCombo_, r, 4, 1, 2);
        grid->addWidget(editDegreesButton, r, 6, 1, 2);

        r++;
        auto* tuningPolicyLabel = new QLabel(
            "Only mathematically exact systems are built in. Gamelan, makam, and raga require a named measured or theoretical dataset.");
        tuningPolicyLabel->setWordWrap(true);
        grid->addWidget(tuningPolicyLabel, r, 0, 1, 7);

        r++;
        grid->addWidget(new QLabel("Strength"), r, 0);
        auto* strengthBox = new QWidget;
        auto* strengthLayout = new QHBoxLayout(strengthBox);
        strengthLayout->setContentsMargins(0, 0, 0, 0);
        strengthLayout->addWidget(strengthSlider_);
        strengthLayout->addWidget(strengthValue_);
        grid->addWidget(strengthBox, r, 1, 1, 3);
        grid->addWidget(new QLabel("Wet/Dry"), r, 4);
        auto* wetDryBox = new QWidget;
        auto* wetDryLayout = new QHBoxLayout(wetDryBox);
        wetDryLayout->setContentsMargins(0, 0, 0, 0);
        wetDryLayout->addWidget(wetDrySlider_);
        wetDryLayout->addWidget(wetDryValue_);
        grid->addWidget(wetDryBox, r, 5, 1, 3);

        r++;
        grid->addWidget(new QLabel("Sample Rate"), r, 0);
        grid->addWidget(sampleRateSpin_, r, 1);
        grid->addWidget(new QLabel("Buffer"), r, 2);
        grid->addWidget(bufferSpin_, r, 3);
        grid->addWidget(new QLabel("Bins"), r, 4);
        grid->addWidget(binsSpin_, r, 5);
        grid->addWidget(new QLabel("Resynth"), r, 6);
        grid->addWidget(resynthModeCombo_, r, 7);

        r++;
        grid->addWidget(new QLabel("Min MIDI"), r, 0);
        grid->addWidget(minMidiSpin_, r, 1);
        grid->addWidget(new QLabel("Max MIDI"), r, 2);
        grid->addWidget(maxMidiSpin_, r, 3);
        grid->addWidget(new QLabel("Analysis Hop"), r, 4);
        grid->addWidget(hopSpin_, r, 5);
        grid->addWidget(new QLabel("Voiced Thresh"), r, 2);
        grid->addWidget(voicedThresholdSpin_, r, 3);
        grid->addWidget(new QLabel("Algorithm"), r, 6);
        grid->addWidget(algorithmModeCombo_, r, 7);

        leftRoot->addWidget(controlBox);
        leftRoot->addWidget(pitchLabel_);

        graph_ = new PitchHistoryWidget;
        graph_->setClickHandler([this]() { showPitchMonitor(); });
        leftRoot->addWidget(graph_, 1);

        advancedTabs_ = new QTabWidget;
        auto* pitchTab = new QWidget;
        auto* pitchForm = new QFormLayout(pitchTab);
        pitchForm->addRow("Multiple", multipleSpin_);
        pitchForm->addRow("Freq Min", freqMinSpin_);
        pitchForm->addRow("Freq Max", freqMaxSpin_);
        pitchForm->addRow("Reference frequency (Hz)", baseASpin_);
        pitchForm->addRow("Leakiness", leakinessSpin_);
        pitchForm->addRow("Buffer Count", bufferCountSpin_);

        auto* transformTab = new QWidget;
        auto* transformForm = new QFormLayout(transformTab);
        transformForm->addRow("Compute", computeModeCombo_);
        transformForm->addRow("Window", windowModeCombo_);
        transformForm->addRow("Normalize", normalizationModeCombo_);
        transformForm->addRow("Window Length", windowLengthModeCombo_);

        auto* synthesisTab = new QWidget;
        auto* synthesisForm = new QFormLayout(synthesisTab);
        synthesisForm->addRow("Note glide", ratioSmoothingSpin_);
        synthesisForm->addRow("Amp Smooth", amplitudeSmoothingSpin_);
        synthesisForm->addRow("Phase Pull", phasePullSpin_);
        synthesisForm->addRow("Flow Grain (ms)", flowGrainMsSpin_);
        synthesisForm->addRow("Flow Overlap", flowOverlapSpin_);
        synthesisForm->addRow("Flow Delay (ms)", flowBaseDelayMsSpin_);
        synthesisForm->addRow("Flow Drift", flowDriftCorrectionSpin_);

        auto* flagsTab = new QWidget;
        auto* flagsLayout = new QVBoxLayout(flagsTab);
        flagsLayout->addWidget(nonInterleavedCheck_);
        flagsLayout->addWidget(minimizeLatencyCheck_);
        flagsLayout->addWidget(hogDeviceCheck_);
        flagsLayout->addWidget(scheduleRealtimeCheck_);
        flagsLayout->addStretch(1);

        advancedTabs_->addTab(pitchTab, "Pitch");
        advancedTabs_->addTab(transformTab, "Transform");
        advancedTabs_->addTab(synthesisTab, "Synthesis");
        advancedTabs_->addTab(flagsTab, "Audio Flags");
        advancedTabs_->setMinimumWidth(360);
        advancedTabs_->setVisible(false);

        splitter->addWidget(leftPane);
        splitter->addWidget(advancedTabs_);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 0);
        splitter->setSizes({1100, 0});

        connect(refreshButton_, &QPushButton::clicked, this, [this]() { refreshDevices(); });
        connect(chirpTestButton_, &QPushButton::clicked, this, [this]() { runChirpTest(); });
        connect(apiCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            if (!engine_.isRunning()) refreshDevices();
        });
        connect(startStopButton_, &QPushButton::clicked, this, [this]() { toggleStartStop(); });
        connect(strengthSlider_, &QSlider::valueChanged, this, [this](int v) {
            const double s = static_cast<double>(v) / 100.0;
            strengthValue_->setText(QString::number(s, 'f', 2));
        });
        connect(wetDrySlider_, &QSlider::valueChanged, this, [this](int v) {
            const double w = static_cast<double>(v) / 100.0;
            wetDryValue_->setText(QString::number(w, 'f', 2));
        });
        connect(advancedToggleButton_, &QPushButton::toggled, this, [this, splitter](bool on) {
            advancedTabs_->setVisible(on);
            advancedToggleButton_->setText(on ? "Hide Advanced <<" : "Show Advanced >>");
            splitter->setSizes(on ? QList<int>{860, 420} : QList<int>{1200, 0});
        });
        connect(profileCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int ix) {
            if (ix == 0) { // Balanced
                multipleSpin_->setValue(24);
                binsSpin_->setValue(128);
                hopSpin_->setValue(256);
                minMidiSpin_->setValue(40);
                maxMidiSpin_->setValue(84);
                freqMinSpin_->setValue(100);
                freqMaxSpin_->setValue(3000);
                voicedThresholdSpin_->setValue(0.20);
                ratioSmoothingSpin_->setValue(0.15);
            } else if (ix == 1) { // Low Notes Stable
                multipleSpin_->setValue(36);
                binsSpin_->setValue(192);
                hopSpin_->setValue(192);
                minMidiSpin_->setValue(28);
                maxMidiSpin_->setValue(72);
                freqMinSpin_->setValue(50);
                freqMaxSpin_->setValue(1800);
                voicedThresholdSpin_->setValue(0.16);
                ratioSmoothingSpin_->setValue(0.10);
            } else if (ix == 2) { // Low Notes Aggressive
                multipleSpin_->setValue(48);
                binsSpin_->setValue(224);
                hopSpin_->setValue(128);
                minMidiSpin_->setValue(24);
                maxMidiSpin_->setValue(72);
                freqMinSpin_->setValue(40);
                freqMaxSpin_->setValue(1600);
                voicedThresholdSpin_->setValue(0.12);
                ratioSmoothingSpin_->setValue(0.20);
            } else if (ix == 3) { // High Notes Fast
                multipleSpin_->setValue(20);
                binsSpin_->setValue(96);
                hopSpin_->setValue(320);
                minMidiSpin_->setValue(52);
                maxMidiSpin_->setValue(96);
                freqMinSpin_->setValue(180);
                freqMaxSpin_->setValue(5200);
                voicedThresholdSpin_->setValue(0.24);
                ratioSmoothingSpin_->setValue(0.22);
            }
        });

        QSettings savedSettings("JTune", "JTune");
        const QString savedFile = savedSettings.value("pitch_system/file").toString();
        bool savedDefinitionChanged = false;
        if (!savedFile.isEmpty()) {
            std::vector<std::string> errors;
            if (jtune::pitchSystemRegistry().loadFile(savedFile.toStdString(), errors)) {
                const auto& definition = jtune::pitchSystemRegistry().definitions().back();
                if (tuningCombo_->findData(QString::fromStdString(definition.id)) < 0)
                    tuningCombo_->addItem(QString::fromStdString(definition.displayName), QString::fromStdString(definition.id));
                const QString savedVersion = savedSettings.value("pitch_system/version").toString();
                const QString savedHash = savedSettings.value("pitch_system/source_hash").toString();
                savedDefinitionChanged = (!savedVersion.isEmpty() && savedVersion != QString::fromStdString(definition.version)) ||
                    (!savedHash.isEmpty() && savedHash != QString::fromStdString(definition.sourceHash));
            }
        }
        if (savedDefinitionChanged)
            QMessageBox::warning(this, "Pitch System Changed",
                "The saved pitch-system file no longer matches its recorded version/hash. "
                "JTune selected 12-EDO instead; inspect and explicitly select the changed definition to use it.");
        const int savedPitchSystem = tuningCombo_->findData(savedDefinitionChanged
            ? QVariant("org.jtune.edo.12") : savedSettings.value("pitch_system/id", "org.jtune.edo.12"));
        if (savedPitchSystem >= 0) tuningCombo_->setCurrentIndex(savedPitchSystem);
        referenceNoteCombo_->setCurrentIndex(std::clamp(savedSettings.value("pitch_system/reference_midi", 69).toInt(), 0, 127));
        tonicNoteCombo_->setCurrentIndex(std::clamp(savedSettings.value("pitch_collection/tonic_midi", 60).toInt(), 0, 127));
        const int savedCollection = collectionCombo_->findData(savedSettings.value("pitch_collection/id", "all"));
        if (savedCollection >= 0) collectionCombo_->setCurrentIndex(savedCollection);
        for (const QVariant& degree : savedSettings.value("pitch_collection/custom_degrees").toList())
            customEnabledDegrees_.push_back(degree.toInt());
        baseASpin_->setValue(savedSettings.value("pitch_system/reference_hz", 440.0).toDouble());
        octaveShiftSpin_->setValue(std::clamp(savedSettings.value("pitch_system/octave_shift", 0).toInt(), -2, 2));
        connect(tuningCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            QSettings settings("JTune", "JTune");
            settings.setValue("pitch_system/id", tuningCombo_->currentData().toString());
            const auto* definition = jtune::pitchSystemRegistry().byId(tuningCombo_->currentData().toString().toStdString());
            if (definition) {
                settings.setValue("pitch_system/version", QString::fromStdString(definition->version));
                settings.setValue("pitch_system/source_hash", QString::fromStdString(definition->sourceHash));
            }
        });
        connect(referenceNoteCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            QSettings("JTune", "JTune").setValue("pitch_system/reference_midi", referenceNoteCombo_->currentData());
        });
        connect(collectionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            QSettings("JTune", "JTune").setValue("pitch_collection/id", collectionCombo_->currentData());
        });
        connect(tonicNoteCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            QSettings("JTune", "JTune").setValue("pitch_collection/tonic_midi", tonicNoteCombo_->currentData());
        });
        connect(baseASpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [](double value) {
            QSettings("JTune", "JTune").setValue("pitch_system/reference_hz", value);
        });
        connect(octaveShiftSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [](int value) {
            QSettings("JTune", "JTune").setValue("pitch_system/octave_shift", value);
        });

        timer_ = new QTimer(this);
        timer_->setInterval(33);
        connect(timer_, &QTimer::timeout, this, [this]() { onUiTick(); });
        timer_->start();

        refreshDevices();
        startControlServer();
    }

    ~MainWindow() override
    {
        engine_.stop();
    }

private:
    RtAudio::Api selectedApi() const
    {
        return static_cast<RtAudio::Api>(apiCombo_->currentData().toInt());
    }

    void refreshDevices()
    {
        QString err;
        const auto devices = engine_.queryDevices(selectedApi(), err);
        devices_ = devices;

        inputCombo_->clear();
        outputCombo_->clear();

        int defaultInIx = -1;
        int defaultOutIx = -1;
        for (const auto& d : devices_) {
            if (d.inChannels > 0) {
                const int ix = inputCombo_->count();
                inputCombo_->addItem(QString("[%1] %2").arg(d.id).arg(d.label), static_cast<int>(d.id));
                if (d.isDefaultIn && defaultInIx < 0) defaultInIx = ix;
            }
            if (d.outChannels > 0) {
                const int ix = outputCombo_->count();
                outputCombo_->addItem(QString("[%1] %2").arg(d.id).arg(d.label), static_cast<int>(d.id));
                if (d.isDefaultOut && defaultOutIx < 0) defaultOutIx = ix;
            }
        }

        if (defaultInIx >= 0) inputCombo_->setCurrentIndex(defaultInIx);
        if (defaultOutIx >= 0) outputCombo_->setCurrentIndex(defaultOutIx);

        if (!err.isEmpty()) {
            statusLabel_->setText("Device query error: " + err);
        } else {
            statusLabel_->setText(QString("%1 devices").arg(devices_.size()));
        }
    }

    void toggleStartStop()
    {
        if (engine_.isRunning()) {
            engine_.stop();
            startStopButton_->setText("Start");
            statusLabel_->setText("Stopped");
            return;
        }

        if (inputCombo_->currentIndex() < 0 || outputCombo_->currentIndex() < 0) {
            statusLabel_->setText("Select valid input/output devices");
            return;
        }

        advanced_.sampleRate = sampleRateSpin_->value();
        advanced_.bufferFrames = bufferSpin_->value();
        advanced_.bins = binsSpin_->value();
        advanced_.multiple = multipleSpin_->value();
        advanced_.freqMin = freqMinSpin_->value();
        advanced_.freqMax = std::max(freqMinSpin_->value() + 10, freqMaxSpin_->value());
        advanced_.bufferCount = bufferCountSpin_->value();
        advanced_.leakiness = leakinessSpin_->value();
        advanced_.baseA = baseASpin_->value();
        advanced_.computeMode = computeModeCombo_->currentData().toInt();
        advanced_.windowMode = windowModeCombo_->currentData().toInt();
        advanced_.normalizationMode = normalizationModeCombo_->currentData().toInt();
        advanced_.windowLengthMode = windowLengthModeCombo_->currentData().toInt();
        advanced_.algorithmMode = algorithmModeCombo_->currentData().toInt();
        advanced_.flagNonInterleaved = nonInterleavedCheck_->isChecked();
        advanced_.flagMinimizeLatency = minimizeLatencyCheck_->isChecked();
        advanced_.flagHogDevice = hogDeviceCheck_->isChecked();
        advanced_.flagScheduleRealtime = scheduleRealtimeCheck_->isChecked();

        StreamSettings s;
        s.api = selectedApi();
        s.inputDevice = static_cast<unsigned int>(inputCombo_->currentData().toInt());
        s.outputDevice = static_cast<unsigned int>(outputCombo_->currentData().toInt());
        s.bufferFrames = static_cast<unsigned int>(advanced_.bufferFrames);
        s.bufferCount = static_cast<unsigned int>(advanced_.bufferCount);
        RtAudioStreamFlags flags = 0;
        if (advanced_.flagNonInterleaved) flags |= RTAUDIO_NONINTERLEAVED;
        if (advanced_.flagMinimizeLatency) flags |= RTAUDIO_MINIMIZE_LATENCY;
        if (advanced_.flagHogDevice) flags |= RTAUDIO_HOG_DEVICE;
        if (advanced_.flagScheduleRealtime) flags |= RTAUDIO_SCHEDULE_REALTIME;
        s.audioFlags = flags;
        s.historySize = 500;

        s.dsp.sampleRate = static_cast<unsigned int>(advanced_.sampleRate);
        s.dsp.pitchSystemId = tuningCombo_->currentData().toString().toStdString();
        s.dsp.pitchCollectionId = collectionCombo_->currentData().toString().toStdString();
        s.dsp.tonicMidiNote = tonicNoteCombo_->currentData().toInt();
        s.dsp.customEnabledDegrees = customEnabledDegrees_;
        s.dsp.referenceMidiNote = referenceNoteCombo_->currentData().toInt();
        s.dsp.octaveShift = octaveShiftSpin_->value();
        s.dsp.strength = static_cast<float>(strengthSlider_->value()) / 100.0f;
        s.dsp.wetMix = static_cast<float>(wetDrySlider_->value()) / 100.0f;
        s.dsp.minMidi = minMidiSpin_->value();
        s.dsp.maxMidi = maxMidiSpin_->value();
        s.dsp.multiple = advanced_.multiple;
        s.dsp.voicedThreshold = static_cast<float>(voicedThresholdSpin_->value());
        s.dsp.binCount = advanced_.bins;
        s.dsp.analysisHop = hopSpin_->value();
        s.dsp.freqMinHz = static_cast<double>(advanced_.freqMin);
        s.dsp.freqMaxHz = static_cast<double>(advanced_.freqMax);
        s.dsp.leakiness = advanced_.leakiness;
        s.dsp.baseAFrequencyHz = advanced_.baseA;
        s.dsp.computeMode = advanced_.computeMode;
        s.dsp.windowMode = advanced_.windowMode;
        s.dsp.normalizationMode = advanced_.normalizationMode;
        s.dsp.windowLengthMode = advanced_.windowLengthMode;
        s.dsp.algorithmMode = advanced_.algorithmMode;
        s.dsp.resynthMode = resynthModeCombo_->currentData().toInt();
        s.dsp.ratioSmoothing = static_cast<float>(ratioSmoothingSpin_->value());
        s.dsp.amplitudeSmoothing = static_cast<float>(amplitudeSmoothingSpin_->value());
        s.dsp.phasePull = static_cast<float>(phasePullSpin_->value());
        s.dsp.flowGrainMs = flowGrainMsSpin_->value();
        s.dsp.flowOverlap = static_cast<float>(flowOverlapSpin_->value());
        s.dsp.flowBaseDelayMs = flowBaseDelayMsSpin_->value();
        s.dsp.flowDriftCorrection = static_cast<float>(flowDriftCorrectionSpin_->value());

        if (s.dsp.minMidi >= s.dsp.maxMidi) {
            statusLabel_->setText("min-midi must be < max-midi");
            return;
        }

        QString err;
        if (!engine_.start(s, err)) {
            statusLabel_->setText("Start failed: " + err);
            return;
        }

        startStopButton_->setText("Stop");
        statusLabel_->setText(
            QString("Running | Algorithm: %1 | Resynth: %2")
                .arg(algorithmName(s.dsp.algorithmMode))
                .arg(resynthName(s.dsp.resynthMode)));
    }

    void runChirpTest()
    {
        QProgressDialog progress("Running chirp test...", QString(), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setCancelButton(nullptr);
        progress.setMinimumDuration(0);
        progress.show();
        QCoreApplication::processEvents();

        const auto base = currentChirpTestOptions();
        if (base.minMidi >= base.maxMidi) {
            progress.close();
            statusLabel_->setText("Chirp test invalid: min-midi must be < max-midi");
            return;
        }

        const auto result = executeChirpTest(base);

        progress.close();

        const QString summary = QString(
            "Algorithm: %1\n"
            "Verifier algos: %2\n"
            "Pitch system: %3\n"
            "Sample rate: %4 Hz\n"
            "Pitch shift: %5 Hz (raw=%6, tuned=%7)\n"
            "Expected shift: %8 Hz (target=%9)\n"
            "Target error raw/tuned: %10 / %11 Hz\n"
            "Passthrough corr: %12\n"
            "Passthrough rmse: %13\n"
            "Passthrough max abs err: %14\n"
            "Result: %15")
            .arg(algorithmName(base.algorithmMode))
            .arg(result.verifierAlgorithms.isEmpty() ? "fallback-self" : result.verifierAlgorithms)
            .arg(QString::fromStdString(base.pitchSystemId))
            .arg(base.sampleRate)
            .arg(QString::number(result.shiftHz, 'f', 2))
            .arg(QString::number(result.rawHz, 'f', 2))
            .arg(QString::number(result.tunedHz, 'f', 2))
            .arg(QString::number(result.expectedShiftHz, 'f', 2))
            .arg(QString::number(result.targetHz, 'f', 2))
            .arg(QString::number(result.rawTargetErrHz, 'f', 2))
            .arg(QString::number(result.tunedTargetErrHz, 'f', 2))
            .arg(QString::number(result.corr, 'f', 8))
            .arg(QString::number(result.rmse, 'g', 6))
            .arg(QString::number(result.maxAbsErr, 'g', 6))
            .arg(result.ok ? "PASS" : "FAIL");

        statusLabel_->setText(result.ok ? "Chirp test PASS" : "Chirp test FAIL");
        if (result.ok) {
            QMessageBox::information(this, "Chirp Test", summary);
        } else {
            QMessageBox::warning(this, "Chirp Test", summary);
        }
    }

    jtune::AutotuneOptions currentChirpTestOptions() const
    {
        jtune::AutotuneOptions base;
        base.sampleRate = static_cast<unsigned int>(sampleRateSpin_->value());
        base.pitchSystemId = tuningCombo_->currentData().toString().toStdString();
        base.pitchCollectionId = collectionCombo_->currentData().toString().toStdString();
        base.tonicMidiNote = tonicNoteCombo_->currentData().toInt();
        base.customEnabledDegrees = customEnabledDegrees_;
        base.referenceMidiNote = referenceNoteCombo_->currentData().toInt();
        base.octaveShift = octaveShiftSpin_->value();
        base.minMidi = minMidiSpin_->value();
        base.maxMidi = maxMidiSpin_->value();
        base.multiple = multipleSpin_->value();
        base.voicedThreshold = static_cast<float>(voicedThresholdSpin_->value());
        base.binCount = binsSpin_->value();
        base.analysisHop = hopSpin_->value();
        base.freqMinHz = static_cast<double>(freqMinSpin_->value());
        base.freqMaxHz = static_cast<double>(std::max(freqMinSpin_->value() + 10, freqMaxSpin_->value()));
        base.leakiness = leakinessSpin_->value();
        base.baseAFrequencyHz = baseASpin_->value();
        base.computeMode = computeModeCombo_->currentData().toInt();
        base.windowMode = windowModeCombo_->currentData().toInt();
        base.normalizationMode = normalizationModeCombo_->currentData().toInt();
        base.windowLengthMode = windowLengthModeCombo_->currentData().toInt();
        base.algorithmMode = algorithmModeCombo_->currentData().toInt();
        base.resynthMode = resynthModeCombo_->currentData().toInt();
        base.ratioSmoothing = static_cast<float>(ratioSmoothingSpin_->value());
        base.amplitudeSmoothing = static_cast<float>(amplitudeSmoothingSpin_->value());
        base.phasePull = static_cast<float>(phasePullSpin_->value());
        base.flowGrainMs = flowGrainMsSpin_->value();
        base.flowOverlap = static_cast<float>(flowOverlapSpin_->value());
        base.flowBaseDelayMs = flowBaseDelayMsSpin_->value();
        base.flowDriftCorrection = static_cast<float>(flowDriftCorrectionSpin_->value());
        return base;
    }

    ChirpTestResult executeChirpTest(const jtune::AutotuneOptions& base) const
    {
        ChirpTestResult result;
        const auto chirp = generateChirp(base.sampleRate, 4.0, 275.0, 282.0);

        auto passthroughOpts = base;
        passthroughOpts.strength = 0.0f;
        const auto passthrough = runProcessor(chirp, passthroughOpts);
        const auto passMetrics = compareSignals(chirp, passthrough);

        auto tunedOpts = base;
        tunedOpts.strength = 1.0f;
        const auto tuned = runProcessor(chirp, tunedOpts);

        result.rawHz = estimateMedianPitchHzCrossValidated(passthrough, tunedOpts, tunedOpts.algorithmMode, &result.verifierAlgorithms);
        result.tunedHz = estimateMedianPitchHzCrossValidated(tuned, tunedOpts, tunedOpts.algorithmMode, nullptr);
        result.shiftHz = result.tunedHz - result.rawHz;
        result.corr = passMetrics.corr;
        result.rmse = passMetrics.rmse;
        result.maxAbsErr = passMetrics.maxAbsErr;

        if (std::isfinite(result.rawHz) && std::isfinite(result.tunedHz) && result.rawHz > 0.0 && result.tunedHz > 0.0) {
            const auto* definition = jtune::pitchSystemRegistry().byId(base.pitchSystemId);
            jtune::PitchContext context;
            context.referenceMidi = base.referenceMidiNote;
            context.referenceHz = base.baseAFrequencyHz;
            context.octaveShift = base.octaveShift;
            context.tonicMidi = base.tonicMidiNote;
            const auto* collection = jtune::pitchCollectionById(base.pitchCollectionId);
            if (collection && collection->id == "custom") context.enabledDegrees = &base.customEnabledDegrees;
            else if (collection && definition && collection->degreeCount == static_cast<int>(definition->targets.size()))
                context.enabledDegrees = &collection->enabledDegrees;
            const auto evaluated = definition ? jtune::PitchSystemEvaluator(*definition).nearest(result.rawHz, context) : std::nullopt;
            result.targetHz = evaluated ? evaluated->frequencyHz : result.rawHz;
            double semitones = 12.0 * std::log2(result.targetHz / result.rawHz);
            const double maxShift = 6.0 + 12.0 * std::abs(base.octaveShift);
            semitones = std::clamp(semitones, -maxShift, maxShift);
            const double expectedRatio = std::pow(2.0, semitones / 12.0);
            result.expectedShiftHz = result.rawHz * expectedRatio - result.rawHz;
            result.rawTargetErrHz = std::abs(result.rawHz - result.targetHz);
            result.tunedTargetErrHz = std::abs(result.tunedHz - result.targetHz);
            if (std::abs(result.expectedShiftHz) < 1.5) {
                result.pitchOk = std::abs(result.shiftHz) < 2.0;
            } else {
                const bool moved = std::abs(result.shiftHz) > 2.0;
                const bool improved = result.tunedTargetErrHz <= std::max(2.0, result.rawTargetErrHz * 0.75);
                result.pitchOk = moved && improved;
            }
        }

        result.passthroughOk =
            result.corr >= 0.99999 &&
            result.rmse <= 1e-5 &&
            result.maxAbsErr <= 1e-4;
        result.ok = result.pitchOk && result.passthroughOk;
        return result;
    }

    bool startControlServer()
    {
        if (controlServer_) return true;
        int port = 8091;
        const QByteArray envPort = qgetenv("JTUNE_CONTROL_PORT");
        if (!envPort.isEmpty()) {
            bool ok = false;
            const int p = QString::fromUtf8(envPort).toInt(&ok);
            if (ok && p > 0 && p < 65536) port = p;
        }

        controlServer_ = std::make_unique<QTcpServer>(this);
        connect(controlServer_.get(), &QTcpServer::newConnection, this, [this]() {
            while (controlServer_->hasPendingConnections()) {
                QTcpSocket* s = controlServer_->nextPendingConnection();
                connect(s, &QTcpSocket::readyRead, this, [this, s]() { handleControlSocket(s); });
                connect(s, &QTcpSocket::disconnected, s, &QTcpSocket::deleteLater);
            }
        });

        if (!controlServer_->listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
            qWarning("JTune control server failed to start on 127.0.0.1:%d", port);
            controlServer_.reset();
            return false;
        }
        controlPort_ = port;
        qInfo("JTune control server listening on http://127.0.0.1:%d", controlPort_);
        return true;
    }

    QJsonObject currentConfigJson() const
    {
        QJsonObject pitch{{"pitch_system_id", tuningCombo_->currentData().toString()},
            {"pitch_collection_id", collectionCombo_->currentData().toString()},
            {"tonic_midi_note", tonicNoteCombo_->currentData().toInt()},
            {"reference_midi_note", referenceNoteCombo_->currentData().toInt()},
            {"octave_shift", octaveShiftSpin_->value()}, {"min_midi", minMidiSpin_->value()},
            {"max_midi", maxMidiSpin_->value()}};
        if (const auto* definition = jtune::pitchSystemRegistry().byId(
                tuningCombo_->currentData().toString().toStdString())) {
            pitch["version"] = QString::fromStdString(definition->version);
            pitch["source_hash"] = QString::fromStdString(definition->sourceHash);
            pitch["scope"] = QString::fromStdString(definition->scope);
            pitch["reviewed"] = definition->reviewed;
            pitch["correction_eligible"] = definition->correctionEligible;
            pitch["limitations"] = QString::fromStdString(definition->limitations);
        }
        return QJsonObject{
            {"audio", QJsonObject{
                {"sample_rate", sampleRateSpin_->value()},
                {"buffer_frames", bufferSpin_->value()},
                {"buffer_count", bufferCountSpin_->value()}
            }},
            {"pitch", pitch},
            {"dsp", QJsonObject{
                {"strength", static_cast<double>(strengthSlider_->value()) / 100.0},
                {"wet", static_cast<double>(wetDrySlider_->value()) / 100.0},
                {"bins", binsSpin_->value()},
                {"hop", hopSpin_->value()},
                {"multiple", multipleSpin_->value()},
                {"freq_min", freqMinSpin_->value()},
                {"freq_max", freqMaxSpin_->value()},
                {"voiced_threshold", voicedThresholdSpin_->value()},
                {"leakiness", leakinessSpin_->value()},
                {"base_a", baseASpin_->value()},
                {"compute_mode", computeModeCombo_->currentText()},
                {"algorithm", algorithmModeCombo_->currentText()},
                {"resynth", resynthName(resynthModeCombo_->currentData().toInt())},
                {"window_mode", windowModeCombo_->currentText()},
                {"normalization", normalizationModeCombo_->currentText()},
                {"window_length_mode", windowLengthModeCombo_->currentText()},
                {"ratio_smoothing", ratioSmoothingSpin_->value()},
                {"amplitude_smoothing", amplitudeSmoothingSpin_->value()},
                {"phase_pull", phasePullSpin_->value()},
                {"flow_grain_ms", flowGrainMsSpin_->value()},
                {"flow_overlap", flowOverlapSpin_->value()},
                {"flow_base_delay_ms", flowBaseDelayMsSpin_->value()},
                {"flow_drift_correction", flowDriftCorrectionSpin_->value()}
            }},
            {"flags", QJsonObject{
                {"noninterleaved", nonInterleavedCheck_->isChecked()},
                {"minimize_latency", minimizeLatencyCheck_->isChecked()},
                {"hog_device", hogDeviceCheck_->isChecked()},
                {"realtime", scheduleRealtimeCheck_->isChecked()}
            }}
        };
    }

    QJsonObject runtimeStatusJson() const
    {
        const auto snap = engine_.snapshot();
        return QJsonObject{
            {"running", engine_.isRunning()},
            {"in_hz", snap.inHz},
            {"out_hz", snap.outHz},
            {"ratio", snap.ratio},
            {"underruns", static_cast<qint64>(snap.underruns)},
            {"algorithm", algorithmName(algorithmModeCombo_->currentData().toInt())},
            {"resynth", resynthName(resynthModeCombo_->currentData().toInt())}
        };
    }

    void applyConfigQuery(const QUrlQuery& q)
    {
        auto applyIntSpin = [&](const char* key, QSpinBox* s) {
            if (!q.hasQueryItem(key)) return;
            bool ok = false;
            const int v = q.queryItemValue(key).toInt(&ok);
            if (ok) s->setValue(v);
        };
        auto applyDoubleSpin = [&](const char* key, QDoubleSpinBox* s) {
            if (!q.hasQueryItem(key)) return;
            bool ok = false;
            const double v = q.queryItemValue(key).toDouble(&ok);
            if (ok) s->setValue(v);
        };

        applyIntSpin("sample_rate", sampleRateSpin_);
        applyIntSpin("buffer_frames", bufferSpin_);
        applyIntSpin("buffer_count", bufferCountSpin_);
        applyIntSpin("min_midi", minMidiSpin_);
        applyIntSpin("max_midi", maxMidiSpin_);
        applyIntSpin("bins", binsSpin_);
        applyIntSpin("hop", hopSpin_);
        applyIntSpin("multiple", multipleSpin_);
        applyIntSpin("freq_min", freqMinSpin_);
        applyIntSpin("freq_max", freqMaxSpin_);
        applyDoubleSpin("voiced_threshold", voicedThresholdSpin_);
        applyDoubleSpin("leakiness", leakinessSpin_);
        applyDoubleSpin("base_a", baseASpin_);
        applyDoubleSpin("ratio_smoothing", ratioSmoothingSpin_);
        applyDoubleSpin("amplitude_smoothing", amplitudeSmoothingSpin_);
        applyDoubleSpin("phase_pull", phasePullSpin_);
        applyDoubleSpin("flow_overlap", flowOverlapSpin_);
        applyDoubleSpin("flow_drift_correction", flowDriftCorrectionSpin_);
        applyIntSpin("flow_grain_ms", flowGrainMsSpin_);
        applyIntSpin("flow_base_delay_ms", flowBaseDelayMsSpin_);

        if (q.hasQueryItem("strength")) {
            bool ok = false;
            const double v = q.queryItemValue("strength").toDouble(&ok);
            if (ok) strengthSlider_->setValue(static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 100.0)));
        }
        if (q.hasQueryItem("wet")) {
            bool ok = false;
            const double v = q.queryItemValue("wet").toDouble(&ok);
            if (ok) wetDrySlider_->setValue(static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 100.0)));
        }

        if (q.hasQueryItem("algorithm")) {
            const int mode = parseAlgorithmMode(q.queryItemValue("algorithm"));
            const int ix = algorithmModeCombo_->findData(mode);
            if (ix >= 0) algorithmModeCombo_->setCurrentIndex(ix);
        }
        if (q.hasQueryItem("resynth")) {
            const int mode = parseResynthMode(q.queryItemValue("resynth"));
            const int ix = resynthModeCombo_->findData(mode);
            if (ix >= 0) resynthModeCombo_->setCurrentIndex(ix);
        }
    }

    void handleControlSocket(QTcpSocket* s)
    {
        const QByteArray req = s->readAll();
        const QList<QByteArray> lines = req.split('\n');
        auto writeJson = [s](int code, const QJsonObject& obj) {
            const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
            QByteArray header;
            header += "HTTP/1.1 ";
            header += QByteArray::number(code);
            header += (code == 200 ? " OK\r\n" : " ERROR\r\n");
            header += "Content-Type: application/json\r\n";
            header += "Content-Length: ";
            header += QByteArray::number(body.size());
            header += "\r\nConnection: close\r\n\r\n";
            s->write(header + body);
            s->disconnectFromHost();
        };

        if (lines.isEmpty()) {
            writeJson(400, QJsonObject{{"ok", false}, {"error", "empty request"}});
            return;
        }
        const QList<QByteArray> first = lines[0].trimmed().split(' ');
        if (first.size() < 2 || first[0] != "GET") {
            writeJson(400, QJsonObject{{"ok", false}, {"error", "bad request"}});
            return;
        }

        const QUrl url(QString::fromUtf8(first[1]));
        const QString path = url.path();
        const QUrlQuery q(url);

        if (path == "/health" || path == "/control/health") {
            writeJson(200, QJsonObject{{"ok", true}, {"service", "jtune-gui-control"}, {"port", controlPort_}});
            return;
        }
        if (path == "/status" || path == "/control/status") {
            writeJson(200, QJsonObject{
                {"ok", true},
                {"runtime", runtimeStatusJson()},
                {"config", currentConfigJson()}});
            return;
        }
        if (path == "/config" || path == "/control/config") {
            if (q.queryItems().size() > 0) {
                applyConfigQuery(q);
                const bool applyNow = q.queryItemValue("apply_now") == "1";
                if (applyNow && engine_.isRunning()) {
                    engine_.stop();
                    startStopButton_->setText("Start");
                    toggleStartStop();
                }
            }
            writeJson(200, QJsonObject{{"ok", true}, {"runtime", runtimeStatusJson()}, {"config", currentConfigJson()}});
            return;
        }
        if (path == "/start" || path == "/control/start") {
            if (q.queryItems().size() > 0) {
                applyConfigQuery(q);
            }
            if (!engine_.isRunning()) {
                startStopButton_->setText("Start");
                toggleStartStop();
            }
            writeJson(200, QJsonObject{{"ok", engine_.isRunning()}, {"runtime", runtimeStatusJson()}, {"config", currentConfigJson()}, {"status_label", statusLabel_->text()}});
            return;
        }
        if (path == "/stop" || path == "/control/stop") {
            if (engine_.isRunning()) {
                startStopButton_->setText("Stop");
                toggleStartStop();
            }
            writeJson(200, QJsonObject{{"ok", !engine_.isRunning()}, {"runtime", runtimeStatusJson()}, {"status_label", statusLabel_->text()}});
            return;
        }
        if (path == "/chirp-test" || path == "/control/tests/chirp") {
            auto opts = currentChirpTestOptions();
            auto queryOr = [&](const QString& key, const QString& fallback) {
                if (!q.hasQueryItem(key)) return fallback;
                const QString v = q.queryItemValue(key);
                return v.isEmpty() ? fallback : v;
            };
            if (q.hasQueryItem("sample_rate")) opts.sampleRate = q.queryItemValue("sample_rate").toUInt();
            if (q.hasQueryItem("min_midi")) opts.minMidi = q.queryItemValue("min_midi").toInt();
            if (q.hasQueryItem("max_midi")) opts.maxMidi = q.queryItemValue("max_midi").toInt();
            if (q.hasQueryItem("multiple")) opts.multiple = q.queryItemValue("multiple").toInt();
            if (q.hasQueryItem("bins")) opts.binCount = q.queryItemValue("bins").toInt();
            if (q.hasQueryItem("hop")) opts.analysisHop = q.queryItemValue("hop").toInt();
            if (q.hasQueryItem("freq_min")) opts.freqMinHz = q.queryItemValue("freq_min").toDouble();
            if (q.hasQueryItem("freq_max")) opts.freqMaxHz = q.queryItemValue("freq_max").toDouble();
            if (q.hasQueryItem("leakiness")) opts.leakiness = q.queryItemValue("leakiness").toDouble();
            if (q.hasQueryItem("base_a")) opts.baseAFrequencyHz = q.queryItemValue("base_a").toDouble();
            if (q.hasQueryItem("octave_shift")) opts.octaveShift = std::clamp(q.queryItemValue("octave_shift").toInt(), -2, 2);
            if (q.hasQueryItem("voiced_threshold")) opts.voicedThreshold = q.queryItemValue("voiced_threshold").toFloat();
            if (q.hasQueryItem("ratio_smoothing")) opts.ratioSmoothing = q.queryItemValue("ratio_smoothing").toFloat();
            if (q.hasQueryItem("amplitude_smoothing")) opts.amplitudeSmoothing = q.queryItemValue("amplitude_smoothing").toFloat();
            if (q.hasQueryItem("phase_pull")) opts.phasePull = q.queryItemValue("phase_pull").toFloat();
            if (q.hasQueryItem("flow_grain_ms")) opts.flowGrainMs = q.queryItemValue("flow_grain_ms").toInt();
            if (q.hasQueryItem("flow_overlap")) opts.flowOverlap = q.queryItemValue("flow_overlap").toFloat();
            if (q.hasQueryItem("flow_base_delay_ms")) opts.flowBaseDelayMs = q.queryItemValue("flow_base_delay_ms").toInt();
            if (q.hasQueryItem("flow_drift_correction")) opts.flowDriftCorrection = q.queryItemValue("flow_drift_correction").toFloat();
            opts.pitchSystemId = queryOr("pitch_system", tuningCombo_->currentData().toString()).toStdString();
            opts.pitchCollectionId = queryOr("pitch_collection", collectionCombo_->currentData().toString()).toStdString();
            if (q.hasQueryItem("tonic_note")) opts.tonicMidiNote = q.queryItemValue("tonic_note").toInt();
            if (q.hasQueryItem("degrees")) {
                const auto values = jtune::parsePitchDegreeList(q.queryItemValue("degrees").toStdString());
                if (values) { opts.pitchCollectionId = "custom"; opts.customEnabledDegrees = *values; }
            }
            opts.algorithmMode = parseAlgorithmMode(queryOr("algorithm", algorithmName(algorithmModeCombo_->currentData().toInt())));
            opts.resynthMode = parseResynthMode(queryOr("resynth", resynthName(resynthModeCombo_->currentData().toInt())));

            const auto r = executeChirpTest(opts);
            writeJson(200, QJsonObject{
                {"ok", r.ok},
                {"pitch_ok", r.pitchOk},
                {"passthrough_ok", r.passthroughOk},
                {"verifier", "cross-over"},
                {"verifier_algorithms", r.verifierAlgorithms},
                {"sample_rate", static_cast<int>(opts.sampleRate)},
                {"pitch_system_id", QString::fromStdString(opts.pitchSystemId)},
                {"pitch_collection_id", QString::fromStdString(opts.pitchCollectionId)},
                {"tonic_midi_note", opts.tonicMidiNote},
                {"algorithm", algorithmName(opts.algorithmMode)},
                {"resynth", resynthName(opts.resynthMode)},
                {"raw_hz", r.rawHz},
                {"tuned_hz", r.tunedHz},
                {"actual_shift_hz", r.shiftHz},
                {"expected_shift_hz", r.expectedShiftHz},
                {"target_hz", r.targetHz},
                {"raw_target_error_hz", r.rawTargetErrHz},
                {"tuned_target_error_hz", r.tunedTargetErrHz},
                {"passthrough_corr", r.corr},
                {"passthrough_rmse", r.rmse},
                {"passthrough_max_abs_err", r.maxAbsErr}});
            return;
        }

        writeJson(404, QJsonObject{{"ok", false}, {"error", "not found"}});
    }

    std::vector<double> currentExpectedNoteHz() const
    {
        std::vector<double> frequencies;
        const auto* definition = jtune::pitchSystemRegistry().byId(
            tuningCombo_->currentData().toString().toStdString());
        if (!definition) return frequencies;

        jtune::PitchContext context;
        context.referenceMidi = referenceNoteCombo_->currentData().toInt();
        context.referenceHz = baseASpin_->value();
        context.octaveShift = octaveShiftSpin_->value();
        const jtune::PitchSystemEvaluator evaluator(*definition);
        for (int midi = minMidiSpin_->value(); midi <= maxMidiSpin_->value(); ++midi) {
            const auto frequency = evaluator.frequencyForMidi(midi, context);
            if (frequency && *frequency > 0.0 && std::isfinite(*frequency))
                frequencies.push_back(*frequency);
        }
        std::sort(frequencies.begin(), frequencies.end());
        frequencies.erase(std::unique(frequencies.begin(), frequencies.end(), [](double a, double b) {
            return std::abs(1200.0 * std::log2(a / b)) < 0.01;
        }), frequencies.end());
        return frequencies;
    }

    void showPitchMonitor()
    {
        if (!pitchMonitor_) {
            pitchMonitor_ = new QDialog(this);
            pitchMonitor_->setWindowTitle("JTune — Measured Pitch Monitor");
            pitchMonitor_->resize(1100, 650);
            auto* layout = new QVBoxLayout(pitchMonitor_);
            auto* readings = new QHBoxLayout;

            monitorInputLabel_ = new QLabel("INPUT\n-- Hz");
            monitorOutputLabel_ = new QLabel("OUTPUT — MEASURED\n-- Hz");
            monitorInputLabel_->setAlignment(Qt::AlignCenter);
            monitorOutputLabel_->setAlignment(Qt::AlignCenter);
            monitorInputLabel_->setStyleSheet(
                "QLabel { color: #4fc3f7; background: #12161c; border: 2px solid #4fc3f7; "
                "border-radius: 8px; padding: 12px; font-size: 22px; font-weight: 700; }");
            monitorOutputLabel_->setStyleSheet(
                "QLabel { color: #ffa726; background: #12161c; border: 2px solid #ffa726; "
                "border-radius: 8px; padding: 12px; font-size: 22px; font-weight: 700; }");
            readings->addWidget(monitorInputLabel_);
            readings->addWidget(monitorOutputLabel_);
            layout->addLayout(readings);

            monitorGraph_ = new PitchHistoryWidget;
            monitorGraph_->setMinimumHeight(440);
            layout->addWidget(monitorGraph_, 1);
        }
        pitchMonitor_->show();
        pitchMonitor_->raise();
        pitchMonitor_->activateWindow();
    }

    void onUiTick()
    {
        const auto snap = engine_.snapshot();
        const auto expectedNoteHz = currentExpectedNoteHz();

        pitchLabel_->setText(
            QString("in: %1 Hz   out: %2 Hz   ratio: %3   underruns: %4")
                .arg(QString::number(snap.inHz, 'f', 1))
                .arg(QString::number(snap.outHz, 'f', 1))
                .arg(QString::number(snap.ratio, 'f', 3))
                .arg(snap.underruns));

        graph_->setData(snap.in,
                        snap.out,
                        snap.spectra,
                        expectedNoteHz,
                        snap.historySize,
                        snap.spectrumMinHz,
                        snap.spectrumMaxHz,
                        minMidiSpin_->value(),
                        maxMidiSpin_->value());

        if (monitorGraph_) {
            monitorGraph_->setData(snap.in,
                                   snap.out,
                                   snap.spectra,
                                   expectedNoteHz,
                                   snap.historySize,
                                   snap.spectrumMinHz,
                                   snap.spectrumMaxHz,
                                   minMidiSpin_->value(),
                                   maxMidiSpin_->value());
            monitorInputLabel_->setText(
                QString("INPUT\n%1 Hz").arg(QString::number(snap.inHz, 'f', 1)));
            monitorOutputLabel_->setText(
                QString("OUTPUT — MEASURED\n%1 Hz").arg(QString::number(snap.outHz, 'f', 1)));
        }

        if (!engine_.isRunning() && startStopButton_->text() == "Stop") {
            startStopButton_->setText("Start");
            statusLabel_->setText("Stopped");
        }
    }

private:
    AudioEngine engine_;
    std::vector<DeviceEntry> devices_;

    QComboBox* apiCombo_ = nullptr;
    QComboBox* inputCombo_ = nullptr;
    QComboBox* outputCombo_ = nullptr;
    QComboBox* tuningCombo_ = nullptr;
    QComboBox* collectionCombo_ = nullptr;
    QComboBox* tonicNoteCombo_ = nullptr;
    QComboBox* referenceNoteCombo_ = nullptr;
    QComboBox* profileCombo_ = nullptr;
    std::vector<int> customEnabledDegrees_;

    QSlider* strengthSlider_ = nullptr;
    QLabel* strengthValue_ = nullptr;
    QSlider* wetDrySlider_ = nullptr;
    QLabel* wetDryValue_ = nullptr;

    QSpinBox* sampleRateSpin_ = nullptr;
    QSpinBox* octaveShiftSpin_ = nullptr;
    QSpinBox* bufferSpin_ = nullptr;
    QSpinBox* bufferCountSpin_ = nullptr;
    QSpinBox* minMidiSpin_ = nullptr;
    QSpinBox* maxMidiSpin_ = nullptr;
    QSpinBox* binsSpin_ = nullptr;
    QSpinBox* hopSpin_ = nullptr;
    QSpinBox* multipleSpin_ = nullptr;
    QSpinBox* freqMinSpin_ = nullptr;
    QSpinBox* freqMaxSpin_ = nullptr;
    QDoubleSpinBox* leakinessSpin_ = nullptr;
    QDoubleSpinBox* baseASpin_ = nullptr;
    QDoubleSpinBox* voicedThresholdSpin_ = nullptr;
    QComboBox* computeModeCombo_ = nullptr;
    QComboBox* windowModeCombo_ = nullptr;
    QComboBox* normalizationModeCombo_ = nullptr;
    QComboBox* windowLengthModeCombo_ = nullptr;
    QComboBox* algorithmModeCombo_ = nullptr;
    QComboBox* resynthModeCombo_ = nullptr;
    QDoubleSpinBox* ratioSmoothingSpin_ = nullptr;
    QDoubleSpinBox* amplitudeSmoothingSpin_ = nullptr;
    QDoubleSpinBox* phasePullSpin_ = nullptr;
    QSpinBox* flowGrainMsSpin_ = nullptr;
    QDoubleSpinBox* flowOverlapSpin_ = nullptr;
    QSpinBox* flowBaseDelayMsSpin_ = nullptr;
    QDoubleSpinBox* flowDriftCorrectionSpin_ = nullptr;
    QCheckBox* nonInterleavedCheck_ = nullptr;
    QCheckBox* minimizeLatencyCheck_ = nullptr;
    QCheckBox* hogDeviceCheck_ = nullptr;
    QCheckBox* scheduleRealtimeCheck_ = nullptr;

    QPushButton* startStopButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* chirpTestButton_ = nullptr;
    QPushButton* advancedToggleButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* pitchLabel_ = nullptr;
    QTabWidget* advancedTabs_ = nullptr;

    PitchHistoryWidget* graph_ = nullptr;
    QDialog* pitchMonitor_ = nullptr;
    PitchHistoryWidget* monitorGraph_ = nullptr;
    QLabel* monitorInputLabel_ = nullptr;
    QLabel* monitorOutputLabel_ = nullptr;
    QTimer* timer_ = nullptr;
    AdvancedSettings advanced_{};
    std::unique_ptr<QTcpServer> controlServer_;
    int controlPort_ = 0;
};

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
}
