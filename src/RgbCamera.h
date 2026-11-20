#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <atomic>

// RGB capture from the Astra Pro S M's UVC camera (2bc5:0501) via
// OpenCV/V4L2. Runs a polling worker thread. The UVC device is a separate
// USB device from the depth MCU and is not touched by OpenNI2.
class RgbCamera : public QObject {
    Q_OBJECT
public:
    struct DeviceInfo {
        QString deviceFile;
        QString name;
        int vid = 0; // USB idVendor, 0 if unknown
        int pid = 0; // USB idProduct, 0 if unknown
    };

    // A single advertised UVC capture mode (resolution × format × frame rate).
    struct Mode {
        int width = 0;
        int height = 0;
        double fps = 0.0;
        QString fourcc; // "MJPG" / "YUYV"
    };

    explicit RgbCamera(QObject* parent = nullptr);
    ~RgbCamera() override;

    // Lists V4L2 capture nodes (index 0) with card name and USB VID:PID.
    static QList<DeviceInfo> listDevices();

    // Enumerates the UVC device's advertised capture modes at runtime via
    // VIDIOC_ENUM_FMT / ENUM_FRAMESIZES / ENUM_FRAMEINTERVALS. Empty on error.
    static QList<Mode> listModes(const QString& deviceFile);

    // Astra Pro S M RGB UVC camera (2bc5:0501 "Astra Pro HD Camera").
    static constexpr int kOrbbecVid = 0x2bc5;
    static constexpr int kAstraRgbPid = 0x0501;

    // formatFourcc: "MJPG" or "YUYV". fps is applied via CAP_PROP_FPS.
    bool open(const QString& deviceFile, const QString& formatFourcc,
              int width, int height, double fps);
    void close();
    bool isOpen() const { return opened_.load(); }

    QImage frame() const;
    QString lastError() const { return lastError_; }

signals:
    void frameReady();
    void fpsReport(double fps);
    void failed(const QString& msg);

private:
    void pollLoop();

    struct CvCapture; // pimpl: holds cv::VideoCapture
    class Worker : public QThread {
    public:
        explicit Worker(RgbCamera* owner) : owner_(owner) {}
        void run() override { owner_->pollLoop(); }
        RgbCamera* owner_;
    };

    Worker worker_{this};
    CvCapture* cap_ = nullptr;
    std::atomic<bool> opened_{false};
    std::atomic<bool> quit_{false};

    mutable QMutex imageMutex_;
    QImage image_;
    QElapsedTimer fpsTimer_;
    qint64 frames_ = 0;
    QString lastError_;
    QString deviceName_;
};
