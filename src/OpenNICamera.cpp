#include "OpenNICamera.h"

#include <QDateTime>
#include <QDebug>

namespace {

// Turbo-like colormap: t in [0,1] -> RGB. Compact polynomial approximation
// good enough for depth visualization.
inline QRgb colormapTurbo(double t)
{
    t = qBound(0.0, t, 1.0);
    t = (t - 0.5) * 2.0; // -1..1
    const double r = qBound(0.0, 0.15 + 1.9 * t + 1.2 * t * t, 1.0);
    const double g = qBound(0.0, 0.85 - 0.9 * t * t, 1.0);
    const double b = qBound(0.0, 0.95 - 1.9 * t + 1.2 * t * t, 1.0);
    return qRgb(int(r * 255), int(g * 255), int(b * 255));
}

} // namespace

// One relay per stream: the Orbbec driver keys listener registrations by
// pointer, so a single object cannot serve both depth and IR.
struct OpenNICamera::FrameRelay : openni::VideoStream::NewFrameListener {
    OpenNICamera* owner = nullptr;
    StreamState* state = nullptr;
    void onNewFrame(openni::VideoStream& stream) override
    {
        owner->handleFrame(*state, stream);
    }
};

OpenNICamera::OpenNICamera(QObject* parent)
    : QObject(parent),
      depthRelay_(std::make_unique<FrameRelay>()),
      irRelay_(std::make_unique<FrameRelay>())
{
    depthRelay_->owner = this;
    depthRelay_->state = &depth_;
    irRelay_->owner = this;
    irRelay_->state = &ir_;
}

OpenNICamera::~OpenNICamera()
{
    close();
}

bool OpenNICamera::open()
{
    if (openni::OpenNI::initialize() != openni::STATUS_OK) {
        lastError_ = QString::fromUtf8(openni::OpenNI::getExtendedError());
        return false;
    }
    if (device_.open(openni::ANY_DEVICE) != openni::STATUS_OK) {
        lastError_ = QString::fromUtf8(openni::OpenNI::getExtendedError());
        return false;
    }
    return true;
}

void OpenNICamera::close()
{
    stopStream(openni::SENSOR_DEPTH);
    stopStream(openni::SENSOR_IR);
    if (device_.isValid())
        device_.close();
}

QString OpenNICamera::deviceInfo()
{
    if (!device_.isValid())
        return tr("not open");
    const openni::DeviceInfo& di = device_.getDeviceInfo();
    return tr("%1 / %2 (USB %3:%4)")
        .arg(QString::fromUtf8(di.getName()),
             QString::fromUtf8(di.getVendor()))
        .arg(di.getUsbVendorId(), 4, 16, QChar('0'))
        .arg(di.getUsbProductId(), 4, 16, QChar('0'));
}

QString OpenNICamera::pixelFormatName(openni::PixelFormat format)
{
    switch (format) {
    case openni::PIXEL_FORMAT_DEPTH_1_MM: return tr("1 mm");
    case openni::PIXEL_FORMAT_DEPTH_100_UM: return tr("0.1 mm");
    case openni::PIXEL_FORMAT_SHIFT_9_2: return tr("shift 9:2");
    case openni::PIXEL_FORMAT_SHIFT_9_3: return tr("shift 9:3");
    case openni::PIXEL_FORMAT_RGB888: return tr("RGB888");
    case openni::PIXEL_FORMAT_YUV422: return tr("YUV422");
    case openni::PIXEL_FORMAT_GRAY8: return tr("8-bit");
    case openni::PIXEL_FORMAT_GRAY16: return tr("16-bit");
    case openni::PIXEL_FORMAT_JPEG: return tr("JPEG");
    case openni::PIXEL_FORMAT_YUYV: return tr("YUYV");
    case openni::PIXEL_FORMAT_LOG: return tr("log");
    default: return tr("format %1").arg(int(format));
    }
}

QString OpenNICamera::Mode::toString() const
{
    return QString("%1x%2@%3 (%4)")
        .arg(width).arg(height).arg(fps)
        .arg(pixelFormatName(format));
}

QList<OpenNICamera::Mode> OpenNICamera::modes(openni::SensorType type)
{
    QList<Mode> out;
    if (!device_.isValid())
        return out;
    const openni::SensorInfo* si = device_.getSensorInfo(type);
    if (!si)
        return out;
    const auto& arr = si->getSupportedVideoModes();
    for (int i = 0; i < arr.getSize(); ++i) {
        const openni::VideoMode& m = arr[i];
        Mode mm;
        mm.width = m.getResolutionX();
        mm.height = m.getResolutionY();
        mm.fps = m.getFps();
        mm.format = m.getPixelFormat();
        out.append(mm);
    }
    return out;
}

bool OpenNICamera::setupStream(StreamState& s, openni::SensorType type, const Mode& mode)
{
    if (s.stream.create(device_, type) != openni::STATUS_OK) {
        lastError_ = tr("stream create: %1").arg(openni::OpenNI::getExtendedError());
        return false;
    }
    const auto& supported = device_.getSensorInfo(type)->getSupportedVideoModes();
    int chosen = openni::STATUS_ERROR;
    for (int i = 0; i < supported.getSize(); ++i) {
        const openni::VideoMode& m = supported[i];
        if (m.getResolutionX() == mode.width && m.getResolutionY() == mode.height
            && m.getFps() == mode.fps && m.getPixelFormat() == mode.format) {
            chosen = s.stream.setVideoMode(m);
            break;
        }
    }
    if (chosen != openni::STATUS_OK) {
        lastError_ = tr("set video mode %1: %2")
                         .arg(mode.toString(), openni::OpenNI::getExtendedError());
        s.stream.destroy();
        return false;
    }
    QMutexLocker lock(&mutex_);
    s.width = mode.width;
    s.height = mode.height;
    s.format = mode.format;
    s.raw.assign(size_t(mode.width) * size_t(mode.height), 0);
    s.image = QImage();
    s.frameCount = 0;
    s.fps = 0.0;
    s.lastFpsMark = QDateTime::currentMSecsSinceEpoch();
    s.mode = mode;
    s.active = true;
    return true;
}

bool OpenNICamera::startStreams(bool depthEnabled, const Mode& depthMode,
                                bool irEnabled, const Mode& irMode)
{
    if (!device_.isValid()) {
        lastError_ = tr("device not open");
        return false;
    }

    // Compute what actually changed vs. the current running state, so toggling
    // one stream doesn't needlessly tear down and restart the other.
    const bool depthSame = depth_.active && depthEnabled && depth_.mode == depthMode;
    const bool irSame = ir_.active && irEnabled && ir_.mode == irMode;

    // Streams that need to be (re)created: enabled but not in the target mode.
    const bool depthNeedCreate = depthEnabled && !depthSame;
    const bool irNeedCreate = irEnabled && !irSame;

    // Streams that must be stopped: currently active but disabled now, or
    // active in a different mode (so they need a rebuild).
    const bool depthNeedStop = depth_.active && (!depthEnabled || depthNeedCreate);
    const bool irNeedStop = ir_.active && (!irEnabled || irNeedCreate);

    // Nothing to do.
    if (!depthNeedCreate && !irNeedCreate && !depthNeedStop && !irNeedStop)
        return true;

    // Pure shutdown (no stream needs to be created): just stop what changed,
    // leaving the other stream untouched — this is the "disable one stream"
    // path and must not disturb the still-running stream.
    if (!depthNeedCreate && !irNeedCreate) {
        if (depthNeedStop)
            stopStream(openni::SENSOR_DEPTH);
        if (irNeedStop)
            stopStream(openni::SENSOR_IR);
        return true;
    }

    // At least one stream must be created. The MX400 re-initializes the sensor
    // when a second stream is created while another is live (silently killing
    // the first), so stop every running stream, then create + configure all
    // enabled ones before starting any.
    if (depth_.active)
        stopStream(openni::SENSOR_DEPTH);
    if (ir_.active)
        stopStream(openni::SENSOR_IR);

    if (depthEnabled && !setupStream(depth_, openni::SENSOR_DEPTH, depthMode)) {
        stopStream(openni::SENSOR_IR);
        return false;
    }
    if (irEnabled && !setupStream(ir_, openni::SENSOR_IR, irMode)) {
        stopStream(openni::SENSOR_DEPTH);
        return false;
    }

    bool ok = true;
    if (depthEnabled) {
        depth_.stream.addNewFrameListener(depthRelay_.get());
        if (depth_.stream.start() != openni::STATUS_OK) {
            lastError_ = tr("depth start: %1").arg(openni::OpenNI::getExtendedError());
            depth_.stream.removeNewFrameListener(depthRelay_.get());
            depth_.stream.destroy();
            depth_.active = false;
            ok = false;
        }
    }
    if (irEnabled) {
        ir_.stream.addNewFrameListener(irRelay_.get());
        if (ir_.stream.start() != openni::STATUS_OK) {
            lastError_ = tr("ir start: %1").arg(openni::OpenNI::getExtendedError());
            ir_.stream.removeNewFrameListener(irRelay_.get());
            ir_.stream.destroy();
            ir_.active = false;
            ok = false;
        }
    }
    return ok;
}

void OpenNICamera::stopStream(openni::SensorType type)
{
    StreamState* s = stateFor(type);
    if (!s || !s->stream.isValid())
        return;
    s->stream.stop();
    s->stream.removeNewFrameListener(s == &depth_ ? depthRelay_.get() : irRelay_.get());
    s->stream.destroy();
    QMutexLocker lock(&mutex_);
    s->image = QImage();
    s->active = false;
}

bool OpenNICamera::streamRunning(openni::SensorType type)
{
    const StreamState* s = nullptr;
    switch (type) {
    case openni::SENSOR_DEPTH: s = &depth_; break;
    case openni::SENSOR_IR: s = &ir_; break;
    default: return false;
    }
    return s->stream.isValid() && s->stream.isValid();
}

bool OpenNICamera::setRegistration(bool on)
{
    if (!device_.isValid())
        return false;
    return device_.setImageRegistrationMode(on ? openni::IMAGE_REGISTRATION_DEPTH_TO_COLOR
                                               : openni::IMAGE_REGISTRATION_OFF)
        == openni::STATUS_OK;
}

bool OpenNICamera::applyMirroring(openni::SensorType type, bool mirrored)
{
    StreamState* s = stateFor(type);
    if (!s || !s->stream.isValid())
        return false;
    // Driver inverts the property: false flips the image.
    return s->stream.setMirroringEnabled(!mirrored) == openni::STATUS_OK;
}

QImage OpenNICamera::frame(openni::SensorType type)
{
    const StreamState* s = stateFor(type);
    if (!s)
        return {};
    QMutexLocker lock(&mutex_);
    return s->image;
}

quint16 OpenNICamera::depthPixel(int x, int y) const
{
    QMutexLocker lock(&mutex_);
    if (x < 0 || y < 0 || x >= depth_.width || y >= depth_.height)
        return 0;
    return depth_.raw[size_t(y) * depth_.width + x];
}

void OpenNICamera::handleFrame(StreamState& s, openni::VideoStream& stream)
{
    openni::VideoFrameRef ref;
    if (stream.readFrame(&ref) != openni::STATUS_OK || !ref.isValid())
        return;

    const bool isDepth = (&s == &depth_);
    const int w = ref.getWidth(), h = ref.getHeight();

    QMutexLocker lock(&mutex_);
    if (int(s.raw.size()) < w * h)
        s.raw.assign(size_t(w) * size_t(h), 0);
    const int copyable = qMin(int(s.raw.size()), w * h);

    if (s.format == openni::PIXEL_FORMAT_GRAY8) {
        const quint8* src = static_cast<const quint8*>(ref.getData());
        for (int i = 0; i < copyable; ++i)
            s.raw[i] = src[i];
    } else {
        memcpy(s.raw.data(), ref.getData(), size_t(copyable) * sizeof(quint16));
    }
    s.width = w;
    s.height = h;

    // FPS accounting.
    ++s.frameCount;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = now - s.lastFpsMark;
    if (elapsed >= 1000) {
        s.fps = s.frameCount * 1000.0 / double(elapsed);
        s.frameCount = 0;
        s.lastFpsMark = now;
        lock.unlock();
        emit fpsReport(isDepth ? int(openni::SENSOR_DEPTH) : int(openni::SENSOR_IR), s.fps);
        lock.relock();
    }

    convert(s);
    lock.unlock();
    emit frameReady(isDepth ? int(openni::SENSOR_DEPTH) : int(openni::SENSOR_IR));
}

OpenNICamera::StreamState* OpenNICamera::stateFor(openni::SensorType type)
{
    switch (type) {
    case openni::SENSOR_DEPTH: return &depth_;
    case openni::SENSOR_IR: return &ir_;
    default: return nullptr;
    }
}

void OpenNICamera::convert(StreamState& s)
{
    // Caller holds the mutex.
    const int w = s.width, h = s.height;
    if (w <= 0 || h <= 0)
        return;
    if (s.image.size() != QSize(w, h) || s.image.format() != QImage::Format_RGB32)
        s.image = QImage(w, h, QImage::Format_RGB32);

    if (&s == &depth_) {
        // Auto-range over sampled pixels for the colormap.
        int lo = INT_MAX, hi = 0;
        const int step = qMax(1, (w * h) / 4096);
        for (int i = 0; i < w * h; i += step) {
            const quint16 d = s.raw[size_t(i)];
            if (d) {
                if (d < lo) lo = d;
                if (d > hi) hi = d;
            }
        }
        if (lo != INT_MAX && hi > lo) {
            s.depthMin = lo;
            s.depthMax = hi;
        }
        const double range = qMax(1, s.depthMax - s.depthMin);
        auto* px = reinterpret_cast<QRgb*>(s.image.bits());
        for (int i = 0; i < w * h; ++i) {
            const quint16 d = s.raw[size_t(i)];
            px[i] = d ? colormapTurbo((d - s.depthMin) / range) : qRgb(28, 28, 32);
        }
    } else {
        // IR: stretch to histogram-free fixed 8-bit response; GRAY16 gets
        // gain applied so speckle pattern is clearly visible.
        const bool is16 = (s.format == openni::PIXEL_FORMAT_GRAY16);
        int peak = is16 ? 255 : 255;
        if (is16) {
            int hi = 0;
            const int step = qMax(1, (w * h) / 4096);
            for (int i = 0; i < w * h; i += step)
                hi = qMax<int>(hi, s.raw[size_t(i)]);
            if (hi > 0)
                peak = hi;
        }
        auto* px = reinterpret_cast<QRgb*>(s.image.bits());
        for (int i = 0; i < w * h; ++i) {
            const int v = qMin(255, s.raw[size_t(i)] * 255 / qMax(1, peak));
            px[i] = qRgb(v, v, v);
        }
    }
}
