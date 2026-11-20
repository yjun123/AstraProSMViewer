#pragma once

#include <OpenNI.h>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <memory>
#include <vector>

// Owns one OpenNI2 device (Astra Pro S M depth MCU) and its depth + IR
// streams. Frames arrive on OpenNI-internal callback threads, are copied
// under a mutex, and frameReady() is emitted (queued) to the GUI thread.
//
// NOTE: the Orbbec OpenNI2 driver registers frame listeners keyed by the
// listener pointer — one listener object can only serve one stream. Each
// stream therefore gets its own FrameRelay.
class OpenNICamera : public QObject {
    Q_OBJECT
public:
    explicit OpenNICamera(QObject* parent = nullptr);
    ~OpenNICamera() override;

    bool open();
    void close();
    bool isOpen() const { return device_.isValid(); }

    QString deviceInfo();

    struct Mode {
        int width = 0, height = 0, fps = 0;
        openni::PixelFormat format = openni::PIXEL_FORMAT_DEPTH_1_MM;
        QString toString() const;
        bool operator==(const Mode& o) const {
            return width == o.width && height == o.height && fps == o.fps
                   && format == o.format;
        }
    };
    // Human-readable name for any OpenNI PixelFormat (see GUI_DESIGN.md §5.2).
    static QString pixelFormatName(openni::PixelFormat format);
    QList<Mode> modes(openni::SensorType type);

    // (Re)start the OpenNI streams with the given modes and enable flags.
    // Enabled streams are all created before any is started: creating a second
    // stream while the first is live re-initializes the sensor on the MX400
    // and silently kills the first stream's frame delivery. Disabled streams
    // are stopped and left off.
    bool startStreams(bool depthEnabled, const Mode& depthMode,
                      bool irEnabled, const Mode& irMode);
    void stopStream(openni::SensorType type);
    bool streamRunning(openni::SensorType type);

    // Property helpers exposed in the UI.
    bool setRegistration(bool on);
    // Applies horizontal mirroring (UI semantics: true = flip). Must be called
    // while the stream is running — the driver applies it to the live frame
    // pipeline and ignores a pre-start set. The driver's
    // STREAM_PROPERTY_MIRRORING is inverted vs. intuition ("false" flips the
    // image), so mirrored=true maps to setMirroringEnabled(false).
    bool applyMirroring(openni::SensorType type, bool mirrored);

    // Called from GUI thread; returns the latest converted frame.
    QImage frame(openni::SensorType type);
    // Raw 16-bit pixel at image coordinates (depth: mm; IR: sensor counts).
    // Returns 0 for out-of-range coordinates.
    quint16 depthPixel(int x, int y) const;
    QString lastError() const { return lastError_; }

signals:
    void frameReady(int sensorType); // openni::SensorType
    void fpsReport(int sensorType, double fps);

private:
    // Per-stream NewFrameListener forwarding into handleFrame().
    struct FrameRelay;
    friend struct FrameRelay;

    struct StreamState {
        openni::VideoStream stream;
        QImage image;          // latest converted frame (GUI owned via mutex)
        std::vector<quint16> raw; // raw 16-bit pixels (depth mm or IR)
        int width = 0, height = 0;
        openni::PixelFormat format = openni::PIXEL_FORMAT_DEPTH_1_MM;
        int frameCount = 0;
        qint64 lastFpsMark = 0;
        double fps = 0.0;
        int depthMin = 0;      // auto-range for colormap (depth only)
        int depthMax = 0;
        bool active = false;   // stream created + started
        Mode mode;             // active video mode
    };

    void convert(StreamState& s);
    StreamState* stateFor(openni::SensorType type);
    // create + configure one stream (no start). Caller must not have the
    // mutex.
    bool setupStream(StreamState& s, openni::SensorType type, const Mode& mode);
    void handleFrame(StreamState& s, openni::VideoStream& stream);

    openni::Device device_;
    StreamState depth_;
    StreamState ir_;
    std::unique_ptr<struct FrameRelay> depthRelay_;
    std::unique_ptr<struct FrameRelay> irRelay_;
    mutable QMutex mutex_;
    QString lastError_;
};
