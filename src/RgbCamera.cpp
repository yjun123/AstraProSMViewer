#include "RgbCamera.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

struct RgbCamera::CvCapture {
    cv::VideoCapture cap;
};

RgbCamera::RgbCamera(QObject* parent) : QObject(parent) {}

RgbCamera::~RgbCamera()
{
    close();
}

namespace {
// Resolves the USB VID:PID backing a video4linux node, or {0,0}.
// videoN/device symlinks to the USB interface directory; idVendor/idProduct
// live one level up (the device directory).
void usbIdForNode(const QString& base, int* vid, int* pid)
{
    *vid = *pid = 0;
    // /sys/class/video4linux/videoN is itself a symlink into /sys/devices;
    // the "device" symlink target ("../../../1-x:1.0") is relative to that
    // real path, so canonicalize first.
    const QString realBase = QFileInfo(base).canonicalFilePath();
    if (realBase.isEmpty())
        return;
    const QFileInfo link(realBase + "/device");
    const QString target = link.symLinkTarget();
    if (target.isEmpty())
        return;
    const QString ifaceDir = QDir::isAbsolutePath(target)
        ? target
        : QDir::cleanPath(QDir(realBase).absoluteFilePath(target));
    QDir devDir(ifaceDir); // parent of the USB interface = USB device
    if (!devDir.cdUp())
        return;
    auto readHex = [](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return 0;
        return f.readAll().trimmed().toInt(nullptr, 16);
    };
    *vid = readHex(devDir.filePath("idVendor"));
    *pid = readHex(devDir.filePath("idProduct"));
}
} // namespace

QList<RgbCamera::DeviceInfo> RgbCamera::listDevices()
{
    QList<DeviceInfo> out;
    // /sys/class/video4linux/videoN/name holds the card name. Each UVC
    // camera exposes a capture node (index 0) plus a metadata node (index 1);
    // only list capture nodes. Enumerate the directory instead of counting
    // videoN from 0: node numbers are not contiguous (e.g. video2 can be
    // absent while video3 exists).
    QDir v4l("/sys/class/video4linux");
    QStringList nodes = v4l.entryList({QStringLiteral("video*")}, QDir::Dirs, QDir::Name);
    std::sort(nodes.begin(), nodes.end(), [](const QString& a, const QString& b) {
        return a.mid(5).toInt() < b.mid(5).toInt();
    });
    for (const QString& node : nodes) {
        const QString base = v4l.filePath(node);
        const QString nameFile = base + "/name";
        QFile idx(base + "/index");
        if (idx.open(QIODevice::ReadOnly)) {
            if (idx.readAll().trimmed() != "0")
                continue; // metadata node
        }
        QFile f(nameFile);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        DeviceInfo di;
        di.deviceFile = "/dev/" + node;
        di.name = QString::fromUtf8(f.readAll().trimmed());
        usbIdForNode(base, &di.vid, &di.pid);
        out.append(di);
    }
    return out;
}

namespace {
QString fourccToString(quint32 p)
{
    char s[5];
    s[0] = char(p & 0xff);
    s[1] = char((p >> 8) & 0xff);
    s[2] = char((p >> 16) & 0xff);
    s[3] = char((p >> 24) & 0xff);
    s[4] = 0;
    return QString::fromLatin1(s);
}
} // namespace

QList<RgbCamera::Mode> RgbCamera::listModes(const QString& deviceFile)
{
    QList<Mode> out;
    const int fd = ::open(deviceFile.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return out;

    for (quint32 idx = 0;; ++idx) {
        struct v4l2_fmtdesc fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.index = idx;
        if (::ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0)
            break;
        const QString fourcc = fourccToString(fmt.pixelformat);
        if (fourcc != "MJPG" && fourcc != "YUYV")
            continue; // only formats OpenCV can hand us as RGB

        for (quint32 s = 0;; ++s) {
            struct v4l2_frmsizeenum frmsize {};
            frmsize.index = s;
            frmsize.pixel_format = fmt.pixelformat;
            if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != 0)
                break;
            if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                continue;
            const int w = int(frmsize.discrete.width);
            const int h = int(frmsize.discrete.height);

            for (quint32 f = 0;; ++f) {
                struct v4l2_frmivalenum interval {};
                interval.index = f;
                interval.pixel_format = fmt.pixelformat;
                interval.width = frmsize.discrete.width;
                interval.height = frmsize.discrete.height;
                if (::ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) != 0)
                    break;
                if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE)
                    continue;
                Mode m;
                m.width = w;
                m.height = h;
                m.fourcc = fourcc;
                m.fps = double(interval.discrete.denominator)
                        / double(interval.discrete.numerator);
                out.append(m);
            }
        }
    }
    ::close(fd);
    return out;
}

bool RgbCamera::open(const QString& deviceFile, const QString& formatFourcc,
                     int width, int height, double fps)
{
    close();
    deviceName_ = deviceFile;

    auto* c = new CvCapture;
    c->cap.open(deviceFile.toStdString(), cv::CAP_V4L2);
    if (!c->cap.isOpened()) {
        delete c;
        lastError_ = tr("cannot open %1 (is it busy?)").arg(deviceFile);
        return false;
    }
    const int fourcc = cv::VideoWriter::fourcc(
        formatFourcc[0].toLatin1(), formatFourcc[1].toLatin1(),
        formatFourcc[2].toLatin1(), formatFourcc[3].toLatin1());
    c->cap.set(cv::CAP_PROP_FOURCC, fourcc);
    c->cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    c->cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    if (fps > 0)
        c->cap.set(cv::CAP_PROP_FPS, fps);
    cap_ = c;

    qInfo() << "RGB opened" << deviceFile << formatFourcc
            << cap_->cap.get(cv::CAP_PROP_FOURCC) << "as"
            << cap_->cap.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
            << cap_->cap.get(cv::CAP_PROP_FRAME_HEIGHT) << "@"
            << cap_->cap.get(cv::CAP_PROP_FPS);
    fpsTimer_.start();
    frames_ = 0;
    opened_.store(true);
    quit_.store(false);
    worker_.start();
    return true;
}

void RgbCamera::close()
{
    if (opened_.load()) {
        quit_.store(true);
        worker_.wait();
    }
    opened_.store(false);
    delete cap_;
    cap_ = nullptr;
    QMutexLocker lock(&imageMutex_);
    image_ = QImage();
}

QImage RgbCamera::frame() const
{
    QMutexLocker lock(&imageMutex_);
    return image_;
}

void RgbCamera::pollLoop()
{
    while (!quit_.load()) {
        cv::Mat mat;
        if (!cap_->cap.read(mat) || mat.empty()) {
            qWarning() << "RGB read failed on" << deviceName_;
            if (!quit_.load())
                QTimer::singleShot(0, this,
                    [this] { emit failed(tr("UVC read failed on %1").arg(deviceName_)); });
            break;
        }
        // The V4L2 backend always converts to BGR.
        cv::cvtColor(mat, mat, cv::COLOR_BGR2RGB);

        QImage img(mat.data, mat.cols, mat.rows, int(mat.step), QImage::Format_RGB888);
        {
            QMutexLocker lock(&imageMutex_);
            image_ = img.copy();
        }
        ++frames_;
        if (fpsTimer_.elapsed() >= 1000) {
            const double f = frames_ * 1000.0 / double(fpsTimer_.elapsed());
            frames_ = 0;
            fpsTimer_.restart();
            emit fpsReport(f);
        }
        emit frameReady();
    }
}
