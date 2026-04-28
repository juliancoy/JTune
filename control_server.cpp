#include "autotune_core.h"
#include "pitch_tracker.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double hzToMidi(double hz)
{
    if (hz <= 0.0) return 0.0;
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

double midiToHz(double midi)
{
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

bool inScaleForKey(int midiNote, int keyRoot, bool minor)
{
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int minorScale[7] = {0, 2, 3, 5, 7, 8, 10};
    const int pc = ((midiNote % 12) + 12) % 12;
    const int* intervals = minor ? minorScale : major;
    for (int i = 0; i < 7; ++i) {
        if (((intervals[i] + keyRoot) % 12) == pc) return true;
    }
    return false;
}

int nearestScaleMidiForKey(double detectedMidi, int keyRoot, bool minor)
{
    const int center = static_cast<int>(std::lround(detectedMidi));
    int best = center;
    double bestDist = 1e9;
    for (int n = center - 24; n <= center + 24; ++n) {
        if (!inScaleForKey(n, keyRoot, minor)) continue;
        const double d = std::abs(static_cast<double>(n) - detectedMidi);
        if (d < bestDist) {
            bestDist = d;
            best = n;
        }
    }
    return best;
}

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
    for (size_t i = 0; i < input.size(); ++i) out[i] = proc.processSample(input[i]);
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

int parseKeyRoot(const QString& key)
{
    static const std::vector<std::string> keys = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (size_t i = 0; i < keys.size(); ++i) {
        if (QString::fromStdString(keys[i]).compare(key, Qt::CaseInsensitive) == 0) return static_cast<int>(i);
    }
    return 0;
}

int parseAlgorithm(const QString& algorithm)
{
    const QString a = algorithm.trimmed().toLower();
    if (a == "fft") return 1;
    if (a == "goertzel") return 2;
    return 0;
}

int parseResynth(const QString& raw)
{
    const QString s = raw.trimmed().toLower();
    if (s == "timedomain" || s == "time-domain" || s == "time" || s == "flow" || s == "time-domain-flow" || s == "time_domain_flow") {
        return jtune::TimeDomain;
    }
    return jtune::FrequencyDomain;
}

QByteArray jsonResponse(int status, const QJsonObject& obj)
{
    const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray header;
    header += "HTTP/1.1 ";
    header += QByteArray::number(status);
    header += status == 200 ? " OK\r\n" : " ERROR\r\n";
    header += "Content-Type: application/json\r\n";
    header += "Content-Length: ";
    header += QByteArray::number(body.size());
    header += "\r\nConnection: close\r\n\r\n";
    return header + body;
}

QJsonObject runChirpTest(const QUrlQuery& q)
{
    auto queryOr = [&](const QString& key, const QString& fallback) {
        if (!q.hasQueryItem(key)) return fallback;
        const QString v = q.queryItemValue(key);
        return v.isEmpty() ? fallback : v;
    };

    jtune::AutotuneOptions base;
    base.sampleRate = q.queryItemValue("sample_rate").toUInt();
    if (base.sampleRate == 0) base.sampleRate = 48000;
    base.keyRoot = parseKeyRoot(queryOr("key", "C"));
    base.minor = queryOr("scale", "major").trimmed().toLower() == "minor";
    base.minMidi = q.queryItemValue("min_midi").toInt();
    if (base.minMidi <= 0) base.minMidi = 40;
    base.maxMidi = q.queryItemValue("max_midi").toInt();
    if (base.maxMidi <= 0) base.maxMidi = 84;
    base.multiple = q.queryItemValue("multiple").toInt();
    if (base.multiple <= 0) base.multiple = 40;
    base.binCount = q.queryItemValue("bins").toInt();
    if (base.binCount <= 0) base.binCount = 200;
    base.analysisHop = q.queryItemValue("hop").toInt();
    if (base.analysisHop <= 0) base.analysisHop = 256;
    base.freqMinHz = q.queryItemValue("freq_min").toDouble();
    if (base.freqMinHz <= 0.0) base.freqMinHz = 100.0;
    base.freqMaxHz = q.queryItemValue("freq_max").toDouble();
    if (base.freqMaxHz <= base.freqMinHz) base.freqMaxHz = 3000.0;
    base.leakiness = q.queryItemValue("leakiness").toDouble();
    if (base.leakiness < 0.99 || base.leakiness > 1.0) base.leakiness = 0.9995;
    base.baseAFrequencyHz = q.queryItemValue("base_a").toDouble();
    if (base.baseAFrequencyHz <= 0.0) base.baseAFrequencyHz = 440.0;
    base.algorithmMode = parseAlgorithm(queryOr("algorithm", "loiacono"));
    base.resynthMode = parseResynth(queryOr("resynth", "TimeDomain"));
    base.computeMode = 1;
    base.windowMode = 0;
    base.normalizationMode = 2;
    base.windowLengthMode = 2;
    base.voicedThreshold = q.queryItemValue("voiced_threshold").toFloat();
    if (base.voicedThreshold <= 0.0f) base.voicedThreshold = 0.20f;
    base.ratioSmoothing = q.queryItemValue("ratio_smoothing").toFloat();
    if (base.ratioSmoothing <= 0.0f) base.ratioSmoothing = 0.15f;
    base.amplitudeSmoothing = q.queryItemValue("amplitude_smoothing").toFloat();
    if (base.amplitudeSmoothing <= 0.0f) base.amplitudeSmoothing = 0.15f;
    base.phasePull = q.queryItemValue("phase_pull").toFloat();
    if (base.phasePull < 0.0f) base.phasePull = 0.08f;
    base.flowGrainMs = q.queryItemValue("flow_grain_ms").toInt();
    if (base.flowGrainMs <= 0) base.flowGrainMs = 20;
    base.flowOverlap = q.queryItemValue("flow_overlap").toFloat();
    if (base.flowOverlap <= 0.0f) base.flowOverlap = 0.75f;
    base.flowBaseDelayMs = q.queryItemValue("flow_base_delay_ms").toInt();
    if (base.flowBaseDelayMs <= 0) base.flowBaseDelayMs = 40;
    base.flowDriftCorrection = q.queryItemValue("flow_drift_correction").toFloat();
    if (base.flowDriftCorrection < 0.0f) base.flowDriftCorrection = 0.01f;

    const auto chirp = generateChirp(base.sampleRate, 4.0, 275.0, 282.0);

    auto passthroughOpts = base;
    passthroughOpts.strength = 0.0f;
    const auto passthrough = runProcessor(chirp, passthroughOpts);
    const auto passMetrics = compareSignals(chirp, passthrough);

    auto tunedOpts = base;
    tunedOpts.strength = 1.0f;
    const auto tuned = runProcessor(chirp, tunedOpts);

    QString verifierInfo;
    const double rawHz = estimateMedianPitchHzCrossValidated(passthrough, tunedOpts, tunedOpts.algorithmMode, &verifierInfo);
    const double tunedHz = estimateMedianPitchHzCrossValidated(tuned, tunedOpts, tunedOpts.algorithmMode, nullptr);
    const double actualShiftHz = tunedHz - rawHz;

    double expectedShiftHz = 0.0;
    double targetHz = 0.0;
    double rawTargetErrHz = 0.0;
    double tunedTargetErrHz = 0.0;
    bool pitchOk = false;
    if (std::isfinite(rawHz) && std::isfinite(tunedHz) && rawHz > 0.0 && tunedHz > 0.0) {
        const double rawMidi = hzToMidi(rawHz);
        const int targetMidi = nearestScaleMidiForKey(rawMidi, base.keyRoot, base.minor);
        targetHz = midiToHz(static_cast<double>(targetMidi));
        double semitones = 12.0 * std::log2(targetHz / rawHz);
        semitones = std::clamp(semitones, -6.0, 6.0);
        const double expectedRatio = std::pow(2.0, semitones / 12.0);
        expectedShiftHz = rawHz * expectedRatio - rawHz;
        rawTargetErrHz = std::abs(rawHz - targetHz);
        tunedTargetErrHz = std::abs(tunedHz - targetHz);
        if (std::abs(expectedShiftHz) < 1.5) {
            pitchOk = std::abs(actualShiftHz) < 2.0;
        } else {
            const bool moved = std::abs(actualShiftHz) > 2.0;
            const bool improved = tunedTargetErrHz <= std::max(2.0, rawTargetErrHz * 0.75);
            pitchOk = moved && improved;
        }
    }

    const bool passthroughOk =
        passMetrics.corr >= 0.99999 &&
        passMetrics.rmse <= 1e-5 &&
        passMetrics.maxAbsErr <= 1e-4;
    const bool ok = pitchOk && passthroughOk;

    QJsonObject out;
    out["ok"] = ok;
    out["key"] = QString::number(base.keyRoot);
    out["scale"] = base.minor ? "minor" : "major";
    out["algorithm"] = queryOr("algorithm", "loiacono");
    out["resynth"] = base.resynthMode == jtune::TimeDomain ? "TimeDomain" : "FrequencyDomain";
    out["verifier"] = "cross-over";
    out["verifier_algorithms"] = verifierInfo;
    out["sample_rate"] = static_cast<int>(base.sampleRate);
    out["raw_hz"] = rawHz;
    out["tuned_hz"] = tunedHz;
    out["actual_shift_hz"] = actualShiftHz;
    out["expected_shift_hz"] = expectedShiftHz;
    out["target_hz"] = targetHz;
    out["raw_target_error_hz"] = rawTargetErrHz;
    out["tuned_target_error_hz"] = tunedTargetErrHz;
    out["passthrough_corr"] = passMetrics.corr;
    out["passthrough_rmse"] = passMetrics.rmse;
    out["passthrough_max_abs_err"] = passMetrics.maxAbsErr;
    out["pitch_ok"] = pitchOk;
    out["passthrough_ok"] = passthroughOk;
    return out;
}

class ControlServer : public QObject {
public:
    explicit ControlServer(quint16 port)
    {
        QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket* s = server_.nextPendingConnection();
                QObject::connect(s, &QTcpSocket::readyRead, [this, s]() { handleSocket(s); });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QTcpSocket::deleteLater);
            }
        });

        if (!server_.listen(QHostAddress::LocalHost, port)) {
            std::cerr << "Failed to bind control server on 127.0.0.1:" << port << "\n";
            std::exit(1);
        }
        std::cout << "JTune control server listening on http://127.0.0.1:" << port << "\n";
        std::cout << "Use GET /health and GET /chirp-test?key=C&scale=major&algorithm=loiacono\n";
    }

private:
    void handleSocket(QTcpSocket* s)
    {
        const QByteArray req = s->readAll();
        const QList<QByteArray> lines = req.split('\n');
        if (lines.isEmpty()) {
            s->write(jsonResponse(400, QJsonObject{{"ok", false}, {"error", "empty request"}}));
            s->disconnectFromHost();
            return;
        }

        const QList<QByteArray> first = lines[0].trimmed().split(' ');
        if (first.size() < 2) {
            s->write(jsonResponse(400, QJsonObject{{"ok", false}, {"error", "bad request line"}}));
            s->disconnectFromHost();
            return;
        }

        const QString method = QString::fromUtf8(first[0]);
        const QUrl url(QString::fromUtf8(first[1]));
        const QString path = url.path();

        if (method != "GET") {
            s->write(jsonResponse(405, QJsonObject{{"ok", false}, {"error", "use GET"}}));
            s->disconnectFromHost();
            return;
        }

        if (path == "/health") {
            s->write(jsonResponse(200, QJsonObject{{"ok", true}, {"service", "jtune-control"}}));
            s->disconnectFromHost();
            return;
        }

        if (path == "/chirp-test") {
            const QJsonObject res = runChirpTest(QUrlQuery(url));
            s->write(jsonResponse(200, res));
            s->disconnectFromHost();
            return;
        }

        s->write(jsonResponse(404, QJsonObject{{"ok", false}, {"error", "not found"}}));
        s->disconnectFromHost();
    }

    QTcpServer server_;
};

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    quint16 port = 8087;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--port" && i + 1 < args.size()) {
            bool ok = false;
            const int p = args[i + 1].toInt(&ok);
            if (ok && p > 0 && p < 65536) port = static_cast<quint16>(p);
            ++i;
        }
    }

    ControlServer server(port);
    return app.exec();
}
