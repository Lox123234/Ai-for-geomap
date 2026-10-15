#pragma once

// ONNX Runtime pieces shared by the models (only for .cpp files built with GEOMAP_HAS_ONNXRUNTIME).
#include <QString>
#include <QThread>
#include <memory>
#include <string>
#include <vector>
#include <onnxruntime_cxx_api.h>

namespace ai::ort {

// One environment for the whole program; function-local, so callers that keep sessions in
// statics must call it before creating them (destroyed in reverse order).
inline Ort::Env &environment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "GeoMapAI");
    return env;
}

// CPU session: all cores but one; idle threads sleep instead of spinning, so the interface and
// other sessions keep the CPU.
inline std::unique_ptr<Ort::Session> open(const QString &path)
{
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.SetIntraOpNumThreads(qMax(1, QThread::idealThreadCount() - 1));
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
#ifdef _WIN32
    return std::make_unique<Ort::Session>(environment(), path.toStdWString().c_str(), options);
#else
    return std::make_unique<Ort::Session>(environment(), path.toStdString().c_str(), options);
#endif
}

inline std::vector<std::string> names(Ort::Session &s, bool inputs)
{
    Ort::AllocatorWithDefaultOptions alloc;
    std::vector<std::string> list;
    const size_t n = inputs ? s.GetInputCount() : s.GetOutputCount();
    for (size_t i = 0; i < n; ++i)
        list.push_back(inputs ? s.GetInputNameAllocated(i, alloc).get() : s.GetOutputNameAllocated(i, alloc).get());
    return list;
}

inline QString error(const Ort::Exception &e) { return QStringLiteral("ONNX Runtime: %1").arg(QString::fromUtf8(e.what())); }

} // namespace ai::ort
