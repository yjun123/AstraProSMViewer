#include "MainWindow.h"
#include "StreamView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

namespace {
// Roles for resolution combo entries (W/H) and fps/format combo entries.
constexpr int RoleW = Qt::UserRole;
constexpr int RoleH = Qt::UserRole + 1;
constexpr int RoleFmt = Qt::UserRole + 2;
constexpr int RoleFps = Qt::UserRole + 3;

// Settings live next to the executable (see DESIGN.md §2).
QString settingsFile()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/AstraProSMViewer.ini");
}

// Locates the on-board RGB UVC camera (2bc5:0501) among V4L2 capture nodes.
// Empty if the camera is not present.
QString astraRgbDevice()
{
    for (const auto& d : RgbCamera::listDevices()) {
        if (d.vid == RgbCamera::kOrbbecVid && d.pid == RgbCamera::kAstraRgbPid)
            return d.deviceFile;
    }
    return {};
}

// Astra Pro S M is two USB devices: the depth/IR MCU (2bc5:0403, OpenNI2)
// and the RGB UVC camera (2bc5:0501). Both are shown in the header.
constexpr int kAstraDepthPid = 0x0403;

QString usbId(int vid, int pid)
{
    return QString("%1:%2")
        .arg(vid, 4, 16, QChar('0'))
        .arg(pid, 4, 16, QChar('0'));
}

// Header badge: coloured status dot + device label + state.
QString badgeText(const QColor& dot, const QString& device, const QString& state)
{
    return QString("<span style='color:%1'>●</span>&nbsp; %2 &nbsp;·&nbsp; %3")
        .arg(dot.name(), device, state);
}

// Select the combo entry whose data(role) equals value; no-op if absent.
void selectByData(QComboBox* cb, int role, const QVariant& value)
{
    for (int i = 0; i < cb->count(); ++i) {
        if (cb->itemData(i, role) == value) {
            cb->setCurrentIndex(i);
            return;
        }
    }
}

// Select the combo entry by (width, height); no-op if absent.
void selectRes(QComboBox* cb, int w, int h)
{
    for (int i = 0; i < cb->count(); ++i) {
        if (cb->itemData(i, RoleW).toInt() == w && cb->itemData(i, RoleH).toInt() == h) {
            cb->setCurrentIndex(i);
            return;
        }
    }
}
} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Astra Pro S M Viewer"));
    resize(1480, 860);
    setObjectName("mainWindow");

    // --- Views ---
    depthView_ = new StreamView(tr("Depth"), Accent::depth);
    irView_ = new StreamView(tr("Infrared"), Accent::ir);
    rgbView_ = new StreamView(tr("RGB"), Accent::rgb);

    auto* viewsLayout = new QHBoxLayout;
    viewsLayout->setContentsMargins(12, 8, 12, 0);
    viewsLayout->setSpacing(12);
    viewsLayout->addWidget(depthView_, 1);
    viewsLayout->addWidget(irView_, 1);
    viewsLayout->addWidget(rgbView_, 1);

    auto* central = new QWidget;
    central->setObjectName("central");
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(buildHeader());
    rootLayout->addLayout(viewsLayout, 1);

    statusLabel_ = new QLabel;
    statusLabel_->setObjectName("statusLabel");
    rootLayout->addWidget(statusLabel_);
    setCentralWidget(central);

    // --- Dock: controls ---
    auto* dock = new QDockWidget(tr("Controls"), this);
    dock->setObjectName("controlsDock");
    dock->setWidget(buildControls());
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // --- Wiring ---
    connect(&oni_, &OpenNICamera::frameReady, this, [this](int t) {
        if (t == openni::SENSOR_DEPTH)
            onDepthReady();
        else if (t == openni::SENSOR_IR)
            onIrReady();
    });
    connect(&oni_, &OpenNICamera::fpsReport, this, [this](int t, double f) {
        const QString s = tr("%1: %2 fps").arg(t == openni::SENSOR_DEPTH ? "Depth" : "IR")
                              .arg(f, 0, 'f', 1);
        if (t == openni::SENSOR_DEPTH)
            depthView_->setSubtitle(s);
        else
            irView_->setSubtitle(s);
    });
    connect(&rgb_, &RgbCamera::frameReady, this, &MainWindow::onRgbReady);
    connect(&rgb_, &RgbCamera::fpsReport, this, [this](double f) {
        rgbView_->setSubtitle(tr("%1 fps").arg(f, 0, 'f', 1));
    });
    connect(&rgb_, &RgbCamera::failed, this, [this](const QString& msg) {
        statusLabel_->setText(msg);
        rgbView_->setImage(QImage());
    });
    connect(depthView_, &StreamView::pixelHover, this, [this](const QPoint& pos) {
        depthView_->setPixelReadout(pos.x() < 0 ? QString() : tr("%1 mm @ (%2,%3)")
            .arg(depthAt(pos)).arg(pos.x()).arg(pos.y()));
    });

    QSettings s(settingsFile(), QSettings::IniFormat);
    restoreGeometry(s.value("geo").toByteArray());

    restartAll();
}

QWidget* MainWindow::buildHeader()
{
    auto* header = new QWidget;
    header->setObjectName("header");
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(16, 10, 16, 10);

    auto* title = new QLabel(tr("Astra Pro S M Viewer"));
    title->setObjectName("appTitle");
    h->addWidget(title);

    h->addStretch(1);

    deviceBadge_ = new QLabel;
    deviceBadge_->setObjectName("deviceBadge");
    h->addWidget(deviceBadge_);

    rgbBadge_ = new QLabel;
    rgbBadge_->setObjectName("deviceBadge");
    h->addWidget(rgbBadge_);

    setDeviceBadges(false, false);
    return header;
}

void MainWindow::setDeviceBadges(bool depthOnline, bool rgbOnline)
{
    const QString depthDev = tr("Depth/IR %1").arg(usbId(RgbCamera::kOrbbecVid, kAstraDepthPid));
    const QString rgbDev = tr("RGB %1").arg(usbId(RgbCamera::kOrbbecVid, RgbCamera::kAstraRgbPid));
    deviceBadge_->setText(badgeText(depthOnline ? Accent::ok : Accent::warn, depthDev,
                                    depthOnline ? tr("online") : tr("offline")));
    rgbBadge_->setText(badgeText(rgbOnline ? Accent::ok : Accent::warn, rgbDev,
                                 rgbOnline ? tr("online") : tr("not found")));
}

QWidget* MainWindow::buildControls()
{
    auto* root = new QWidget;
    root->setObjectName("controls");
    auto* form = new QVBoxLayout(root);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(12);

    auto* depthBox = new QGroupBox(tr("Depth"));
    auto* dfl = new QFormLayout(depthBox);
    depthEnableChk_ = new QCheckBox(tr("Enabled"));
    depthEnableChk_->setChecked(true);
    connect(depthEnableChk_, &QCheckBox::toggled, this, [this](bool on) {
        depthResCombo_->setEnabled(on);
        depthFmtCombo_->setEnabled(on);
        depthFpsCombo_->setEnabled(on);
        depthMirrorChk_->setEnabled(on);
        snapDepthBtn_->setEnabled(on);
        if (!on)
            depthView_->setImage(QImage());
        restartAll();
    });
    dfl->addRow(depthEnableChk_);
    depthResCombo_ = new QComboBox;
    dfl->addRow(tr("Resolution"), depthResCombo_);
    depthFmtCombo_ = new QComboBox;
    dfl->addRow(tr("Format"), depthFmtCombo_);
    depthFpsCombo_ = new QComboBox;
    dfl->addRow(tr("Frame rate"), depthFpsCombo_);
    depthMirrorChk_ = new QCheckBox(tr("Mirror (horizontal)"));
    connect(depthMirrorChk_, &QCheckBox::toggled, this, [this](bool on) {
        oni_.applyMirroring(openni::SENSOR_DEPTH, on);
    });
    dfl->addRow(depthMirrorChk_);
    snapDepthBtn_ = new QPushButton(tr("Save snapshot..."));
    connect(snapDepthBtn_, &QPushButton::clicked, [this] { saveSnapshot(0); });
    dfl->addRow(snapDepthBtn_);

    auto* irBox = new QGroupBox(tr("Infrared"));
    auto* ifl = new QFormLayout(irBox);
    irEnableChk_ = new QCheckBox(tr("Enabled"));
    irEnableChk_->setChecked(true);
    connect(irEnableChk_, &QCheckBox::toggled, this, [this](bool on) {
        irResCombo_->setEnabled(on);
        irFmtCombo_->setEnabled(on);
        irFpsCombo_->setEnabled(on);
        irMirrorChk_->setEnabled(on);
        snapIrBtn_->setEnabled(on);
        if (!on)
            irView_->setImage(QImage());
        restartAll();
    });
    ifl->addRow(irEnableChk_);
    irResCombo_ = new QComboBox;
    ifl->addRow(tr("Resolution"), irResCombo_);
    irFmtCombo_ = new QComboBox;
    ifl->addRow(tr("Format"), irFmtCombo_);
    irFpsCombo_ = new QComboBox;
    ifl->addRow(tr("Frame rate"), irFpsCombo_);
    irMirrorChk_ = new QCheckBox(tr("Mirror (horizontal)"));
    connect(irMirrorChk_, &QCheckBox::toggled, this, [this](bool on) {
        oni_.applyMirroring(openni::SENSOR_IR, on);
    });
    ifl->addRow(irMirrorChk_);
    snapIrBtn_ = new QPushButton(tr("Save snapshot..."));
    connect(snapIrBtn_, &QPushButton::clicked, [this] { saveSnapshot(1); });
    ifl->addRow(snapIrBtn_);

    // Refresh the fps dropdown when resolution or format changes.
    connect(depthResCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        const int w = depthResCombo_->currentData(RoleW).toInt();
        const int h = depthResCombo_->currentData(RoleH).toInt();
        const auto fmt = openni::PixelFormat(depthFmtCombo_->currentData(RoleFmt).toInt());
        populateFpsCombo(depthFpsCombo_, openni::SENSOR_DEPTH, w, h, fmt);
    });
    connect(depthFmtCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        const int w = depthResCombo_->currentData(RoleW).toInt();
        const int h = depthResCombo_->currentData(RoleH).toInt();
        const auto fmt = openni::PixelFormat(depthFmtCombo_->currentData(RoleFmt).toInt());
        populateFpsCombo(depthFpsCombo_, openni::SENSOR_DEPTH, w, h, fmt);
    });
    connect(irResCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        const int w = irResCombo_->currentData(RoleW).toInt();
        const int h = irResCombo_->currentData(RoleH).toInt();
        const auto fmt = openni::PixelFormat(irFmtCombo_->currentData(RoleFmt).toInt());
        populateFpsCombo(irFpsCombo_, openni::SENSOR_IR, w, h, fmt);
    });
    connect(irFmtCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        const int w = irResCombo_->currentData(RoleW).toInt();
        const int h = irResCombo_->currentData(RoleH).toInt();
        const auto fmt = openni::PixelFormat(irFmtCombo_->currentData(RoleFmt).toInt());
        populateFpsCombo(irFpsCombo_, openni::SENSOR_IR, w, h, fmt);
    });

    auto* rgbBox = new QGroupBox(tr("RGB (UVC)"));
    auto* rfl = new QFormLayout(rgbBox);
    rgbEnableChk_ = new QCheckBox(tr("Enabled"));
    rgbEnableChk_->setChecked(true);
    connect(rgbEnableChk_, &QCheckBox::toggled, this, [this](bool on) {
        rgbFormatCombo_->setEnabled(on);
        rgbResCombo_->setEnabled(on);
        rgbFpsCombo_->setEnabled(on);
        snapRgbBtn_->setEnabled(on);
        if (!on)
            rgbView_->setImage(QImage());
        restartAll();
    });
    rfl->addRow(rgbEnableChk_);
    // The on-board RGB camera is the only supported UVC device, so there is
    // no device selector: locate it by VID:PID at startup.
    rgbDevice_ = astraRgbDevice();
    rgbResCombo_ = new QComboBox;
    rfl->addRow(tr("Resolution"), rgbResCombo_);
    rgbFormatCombo_ = new QComboBox;
    rfl->addRow(tr("Format"), rgbFormatCombo_);
    rgbFpsCombo_ = new QComboBox;
    rfl->addRow(tr("Frame rate"), rgbFpsCombo_);
    snapRgbBtn_ = new QPushButton(tr("Save snapshot..."));
    connect(snapRgbBtn_, &QPushButton::clicked, [this] { saveSnapshot(2); });
    rfl->addRow(snapRgbBtn_);

    // Enumerate modes from the on-board UVC camera at runtime, then refresh
    // the fps dropdown whenever resolution/format changes.
    connect(rgbResCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::refreshRgbFps);
    connect(rgbFormatCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::refreshRgbFps);
    populateRgbModes();

    auto* applyBtn = new QPushButton(tr("Apply / restart streams"));
    applyBtn->setObjectName("primaryBtn");
    applyBtn->setCursor(Qt::PointingHandCursor);
    // Save only when the user applies: control changes themselves must not
    // persist (and populating combos emits currentIndexChanged, which would
    // otherwise clobber the config with defaults on startup).
    connect(applyBtn, &QPushButton::clicked, this, &MainWindow::restartAll);
    connect(applyBtn, &QPushButton::clicked, this, &MainWindow::saveConfig);

    depthBox->setObjectName("depthBox");
    irBox->setObjectName("irBox");
    rgbBox->setObjectName("rgbBox");

    form->addWidget(depthBox);
    form->addWidget(irBox);
    form->addWidget(rgbBox);
    form->addWidget(applyBtn);
    form->addStretch(1);
    return root;
}

void MainWindow::populateResCombo(QComboBox* cb, openni::SensorType type)
{
    // Block signals: addItem fires currentIndexChanged before we setItemData.
    QSignalBlocker block(cb);
    cb->clear();
    // Distinct resolutions from the driver's advertised modes.
    const auto modes = oni_.modes(type);
    QList<QSize> seen;
    int select = -1;
    for (const auto& m : modes) {
        const QSize sz(m.width, m.height);
        if (seen.contains(sz))
            continue;
        seen.append(sz);
        cb->addItem(QString("%1×%2").arg(m.width).arg(m.height));
        cb->setItemData(cb->count() - 1, m.width, RoleW);
        cb->setItemData(cb->count() - 1, m.height, RoleH);
        if (m.width == 640 && m.height == 480)
            select = cb->count() - 1;
    }
    if (cb->count())
        cb->setCurrentIndex(select < 0 ? 0 : select);
}

void MainWindow::populateFmtCombo(QComboBox* cb, openni::SensorType type)
{
    QSignalBlocker block(cb);
    cb->clear();
    const auto modes = oni_.modes(type);
    QList<openni::PixelFormat> seen;
    int select = -1;
    for (const auto& m : modes) {
        if (seen.contains(m.format))
            continue;
        seen.append(m.format);
        cb->addItem(OpenNICamera::pixelFormatName(m.format));
        cb->setItemData(cb->count() - 1, int(m.format), RoleFmt);
        const bool isDefault = (type == openni::SENSOR_DEPTH
                                && m.format == openni::PIXEL_FORMAT_DEPTH_1_MM)
                               || (type == openni::SENSOR_IR
                                   && m.format == openni::PIXEL_FORMAT_GRAY16);
        if (isDefault)
            select = cb->count() - 1;
    }
    if (cb->count())
        cb->setCurrentIndex(select < 0 ? 0 : select);
}

void MainWindow::populateFpsCombo(QComboBox* cb, openni::SensorType type, int w, int h,
                                  openni::PixelFormat fmt)
{
    QSignalBlocker block(cb);
    cb->clear();
    const auto modes = oni_.modes(type);
    QList<int> seen;
    int select = -1;
    for (const auto& m : modes) {
        if (m.width != w || m.height != h || m.format != fmt || seen.contains(m.fps))
            continue;
        seen.append(m.fps);
        cb->addItem(QString("%1").arg(m.fps));
        cb->setItemData(cb->count() - 1, m.fps, RoleFps);
        if (m.fps == 30)
            select = cb->count() - 1;
    }
    if (cb->count())
        cb->setCurrentIndex(select < 0 ? 0 : select);
}

OpenNICamera::Mode MainWindow::resolveMode(openni::SensorType type, int w, int h,
                                           openni::PixelFormat fmt, int fps)
{
    const auto modes = oni_.modes(type);
    const OpenNICamera::Mode* nearest = nullptr;
    for (const auto& m : modes) {
        if (m.width != w || m.height != h)
            continue;
        if (m.format == fmt && m.fps == fps)
            return m; // exact
        if (m.format == fmt && (!nearest || qAbs(m.fps - fps) < qAbs(nearest->fps - fps)))
            nearest = &m;
    }
    if (nearest)
        return *nearest;
    for (const auto& m : modes)
        if (m.width == w && m.height == h)
            return m; // any format at this resolution
    return modes.isEmpty() ? OpenNICamera::Mode{} : modes.first();
}

OpenNICamera::Mode MainWindow::currentDepthMode()
{
    const int w = depthResCombo_->currentData(RoleW).toInt();
    const int h = depthResCombo_->currentData(RoleH).toInt();
    const auto fmt = openni::PixelFormat(depthFmtCombo_->currentData(RoleFmt).toInt());
    const int fps = depthFpsCombo_->currentData(RoleFps).toInt();
    return resolveMode(openni::SENSOR_DEPTH, w, h, fmt, fps);
}

OpenNICamera::Mode MainWindow::currentIrMode()
{
    const int w = irResCombo_->currentData(RoleW).toInt();
    const int h = irResCombo_->currentData(RoleH).toInt();
    const auto fmt = openni::PixelFormat(irFmtCombo_->currentData(RoleFmt).toInt());
    const int fps = irFpsCombo_->currentData(RoleFps).toInt();
    return resolveMode(openni::SENSOR_IR, w, h, fmt, fps);
}

void MainWindow::populateRgbModes()
{
    // Release the node first: UVC devices allow only one open, and OpenCV
    // holds it while streaming, which would make listModes fail with EBUSY.
    rgb_.close();

    rgbModes_ = RgbCamera::listModes(rgbDevice_);

    // Resolution: distinct (w,h) pairs advertised by the UVC device.
    {
        QSignalBlocker block(rgbResCombo_);
        rgbResCombo_->clear();
        QList<QSize> seen;
        int select = -1;
        for (const auto& m : rgbModes_) {
            const QSize sz(m.width, m.height);
            if (seen.contains(sz))
                continue;
            seen.append(sz);
            rgbResCombo_->addItem(QString("%1×%2").arg(m.width).arg(m.height));
            rgbResCombo_->setItemData(rgbResCombo_->count() - 1, m.width, RoleW);
            rgbResCombo_->setItemData(rgbResCombo_->count() - 1, m.height, RoleH);
            if (m.width == 1280 && m.height == 720)
                select = rgbResCombo_->count() - 1;
        }
        if (rgbResCombo_->count())
            rgbResCombo_->setCurrentIndex(select < 0 ? 0 : select);
    }

    // Format: distinct fourcc codes.
    {
        QSignalBlocker block(rgbFormatCombo_);
        rgbFormatCombo_->clear();
        QList<QString> seen;
        int select = -1;
        for (const auto& m : rgbModes_) {
            if (seen.contains(m.fourcc))
                continue;
            seen.append(m.fourcc);
            rgbFormatCombo_->addItem(m.fourcc);
            rgbFormatCombo_->setItemData(rgbFormatCombo_->count() - 1, m.fourcc, RoleFmt);
            if (m.fourcc == "MJPG")
                select = rgbFormatCombo_->count() - 1;
        }
        if (rgbFormatCombo_->count())
            rgbFormatCombo_->setCurrentIndex(select < 0 ? 0 : select);
    }

    refreshRgbFps();
}

void MainWindow::refreshRgbFps()
{
    const int w = rgbResCombo_->currentData(RoleW).toInt();
    const int h = rgbResCombo_->currentData(RoleH).toInt();
    const QString fmt = rgbFormatCombo_->currentData(RoleFmt).toString();

    QSignalBlocker block(rgbFpsCombo_);
    rgbFpsCombo_->clear();
    QList<int> seen;
    int select = -1;
    for (const auto& m : rgbModes_) {
        if (m.width != w || m.height != h || m.fourcc != fmt)
            continue;
        const int fps = qRound(m.fps);
        if (seen.contains(fps))
            continue;
        seen.append(fps);
        rgbFpsCombo_->addItem(QString("%1").arg(fps));
        rgbFpsCombo_->setItemData(rgbFpsCombo_->count() - 1, fps, RoleFps);
        if (fps == 30)
            select = rgbFpsCombo_->count() - 1;
    }
    if (rgbFpsCombo_->count())
        rgbFpsCombo_->setCurrentIndex(select < 0 ? 0 : select);
}

void MainWindow::saveConfig()
{
    QSettings s(settingsFile(), QSettings::IniFormat);
    s.setValue("depth/enabled", depthEnableChk_->isChecked());
    s.setValue("depth/width", depthResCombo_->currentData(RoleW).toInt());
    s.setValue("depth/height", depthResCombo_->currentData(RoleH).toInt());
    s.setValue("depth/format", depthFmtCombo_->currentData(RoleFmt).toInt());
    s.setValue("depth/fps", depthFpsCombo_->currentData(RoleFps).toInt());
    s.setValue("depth/mirror", depthMirrorChk_->isChecked());

    s.setValue("ir/enabled", irEnableChk_->isChecked());
    s.setValue("ir/width", irResCombo_->currentData(RoleW).toInt());
    s.setValue("ir/height", irResCombo_->currentData(RoleH).toInt());
    s.setValue("ir/format", irFmtCombo_->currentData(RoleFmt).toInt());
    s.setValue("ir/fps", irFpsCombo_->currentData(RoleFps).toInt());
    s.setValue("ir/mirror", irMirrorChk_->isChecked());

    s.setValue("rgb/enabled", rgbEnableChk_->isChecked());
    s.setValue("rgb/width", rgbResCombo_->currentData(RoleW).toInt());
    s.setValue("rgb/height", rgbResCombo_->currentData(RoleH).toInt());
    s.setValue("rgb/format", rgbFormatCombo_->currentData(RoleFmt).toString());
    s.setValue("rgb/fps", rgbFpsCombo_->currentData(RoleFps).toInt());
}

void MainWindow::restoreConfig()
{
    QSettings s(settingsFile(), QSettings::IniFormat);

    // Block signals: restoring must not re-trigger restartAll()/saveConfig().
    QSignalBlocker b1(depthEnableChk_), b2(depthResCombo_), b3(depthFmtCombo_),
        b4(depthFpsCombo_), b5(depthMirrorChk_),
        b6(irEnableChk_), b7(irResCombo_), b8(irFmtCombo_), b9(irFpsCombo_),
        b10(irMirrorChk_),
        b11(rgbEnableChk_), b12(rgbResCombo_),
        b13(rgbFormatCombo_), b14(rgbFpsCombo_);

    // Depth
    depthEnableChk_->setChecked(s.value("depth/enabled", true).toBool());
    selectRes(depthResCombo_, s.value("depth/width", 640).toInt(),
              s.value("depth/height", 480).toInt());
    selectByData(depthFmtCombo_, RoleFmt, s.value("depth/format").toInt());
    populateFpsCombo(depthFpsCombo_, openni::SENSOR_DEPTH,
                     depthResCombo_->currentData(RoleW).toInt(),
                     depthResCombo_->currentData(RoleH).toInt(),
                     openni::PixelFormat(depthFmtCombo_->currentData(RoleFmt).toInt()));
    selectByData(depthFpsCombo_, RoleFps, s.value("depth/fps").toInt());
    depthMirrorChk_->setChecked(s.value("depth/mirror", false).toBool());

    // IR
    irEnableChk_->setChecked(s.value("ir/enabled", true).toBool());
    selectRes(irResCombo_, s.value("ir/width", 640).toInt(),
              s.value("ir/height", 480).toInt());
    selectByData(irFmtCombo_, RoleFmt, s.value("ir/format").toInt());
    populateFpsCombo(irFpsCombo_, openni::SENSOR_IR,
                     irResCombo_->currentData(RoleW).toInt(),
                     irResCombo_->currentData(RoleH).toInt(),
                     openni::PixelFormat(irFmtCombo_->currentData(RoleFmt).toInt()));
    selectByData(irFpsCombo_, RoleFps, s.value("ir/fps").toInt());
    irMirrorChk_->setChecked(s.value("ir/mirror", false).toBool());

    // RGB: combos were already populated from the on-board UVC camera in
    // buildControls(); overlay the saved resolution/format/fps.
    rgbEnableChk_->setChecked(s.value("rgb/enabled", true).toBool());
    selectRes(rgbResCombo_, s.value("rgb/width", 1280).toInt(),
              s.value("rgb/height", 720).toInt());
    selectByData(rgbFormatCombo_, RoleFmt, s.value("rgb/format", "MJPG").toString());
    refreshRgbFps();
    selectByData(rgbFpsCombo_, RoleFps, s.value("rgb/fps").toInt());
}

void MainWindow::restartAll()
{
    bool depthOn = depthEnableChk_->isChecked();
    bool irOn = irEnableChk_->isChecked();
    bool rgbOn = rgbEnableChk_->isChecked();

    // --- OpenNI2: depth + IR ---
    OpenNICamera::Mode depthMode, irMode;
    if (!oni_.isOpen() && !oni_.open()) {
        setDeviceBadges(false, !rgbDevice_.isEmpty());
        QMessageBox::warning(this, tr("OpenNI2"),
                             tr("Cannot open depth device:\n%1").arg(oni_.lastError()));
    } else {
        setDeviceBadges(true, !rgbDevice_.isEmpty());
        const bool first = (depthResCombo_->count() == 0);
        if (first) {
            populateResCombo(depthResCombo_, openni::SENSOR_DEPTH);
            populateFmtCombo(depthFmtCombo_, openni::SENSOR_DEPTH);
            populateFpsCombo(depthFpsCombo_, openni::SENSOR_DEPTH,
                             depthResCombo_->currentData(RoleW).toInt(),
                             depthResCombo_->currentData(RoleH).toInt(),
                             openni::PixelFormat(depthFmtCombo_->currentData(RoleFmt).toInt()));
            populateResCombo(irResCombo_, openni::SENSOR_IR);
            populateFmtCombo(irFmtCombo_, openni::SENSOR_IR);
            populateFpsCombo(irFpsCombo_, openni::SENSOR_IR,
                             irResCombo_->currentData(RoleW).toInt(),
                             irResCombo_->currentData(RoleH).toInt(),
                             openni::PixelFormat(irFmtCombo_->currentData(RoleFmt).toInt()));
            restoreConfig();
            // restoreConfig() may have changed the enable checkboxes, so
            // refresh the flags we read at function entry.
            depthOn = depthEnableChk_->isChecked();
            irOn = irEnableChk_->isChecked();
            rgbOn = rgbEnableChk_->isChecked();
        }
        depthMode = currentDepthMode();
        irMode = currentIrMode();
        if (!oni_.startStreams(depthOn, depthMode, irOn, irMode)) {
            qWarning() << "stream start failed:" << oni_.lastError();
            statusLabel_->setText(oni_.lastError());
        } else {
            qInfo() << "streams started:" << depthMode.toString() << irMode.toString();
            // Mirror is applied to the live pipeline, so re-apply after a
            // (re)start resets it (only for enabled streams).
            if (depthOn)
                oni_.applyMirroring(openni::SENSOR_DEPTH, depthMirrorChk_->isChecked());
            if (irOn)
                oni_.applyMirroring(openni::SENSOR_IR, irMirrorChk_->isChecked());
        }
    }

    if (!depthOn) {
        depthView_->setImage(QImage());
        depthView_->setSubtitle(QString());
    }
    if (!irOn) {
        irView_->setImage(QImage());
        irView_->setSubtitle(QString());
    }

    // --- RGB: UVC ---
    // Re-locate the on-board camera on every apply so hot-plugging is picked
    // up; re-enumerate its modes if the node changed.
    const QString dev = astraRgbDevice();
    if (dev != rgbDevice_) {
        rgbDevice_ = dev;
        populateRgbModes();
    }
    setDeviceBadges(oni_.isOpen(), !rgbDevice_.isEmpty());

    // Only restart the UVC stream when its actual config changed: toggling
    // depth/IR also calls restartAll(), and re-opening UVC would drop frames.
    RgbConfig want;
    want.enabled = rgbOn;
    want.dev = rgbDevice_;
    want.fmt = rgbFormatCombo_->currentData(RoleFmt).toString();
    want.w = rgbResCombo_->currentData(RoleW).toInt();
    want.h = rgbResCombo_->currentData(RoleH).toInt();
    want.fps = rgbFpsCombo_->currentData(RoleFps).toInt();

    // Status bar: running mode summary (the header already carries VID:PID).
    // Transient messages (errors, saved path) stay until one replaces them.
    QStringList parts;
    parts << (depthOn ? tr("Depth %1").arg(depthMode.toString()) : tr("Depth off"));
    parts << (irOn ? tr("IR %1").arg(irMode.toString()) : tr("IR off"));
    parts << (want.enabled ? tr("RGB %1×%2@%3 %4")
                                 .arg(want.w).arg(want.h).arg(want.fps).arg(want.fmt)
                           : tr("RGB off"));
    const QString summary = parts.join(QStringLiteral("  ·  "));
    if (statusLabel_->text().isEmpty() || statusLabel_->text() == defaultStatus_)
        statusLabel_->setText(summary);
    defaultStatus_ = summary;

    if (want == rgbConfig_)
        return;

    rgb_.close();
    if (want.enabled && !want.dev.isEmpty() && want.w > 0 && want.h > 0
        && !want.fmt.isEmpty()) {
        if (rgb_.open(want.dev, want.fmt, want.w, want.h, want.fps))
            rgbConfig_ = want;
        else
            statusLabel_->setText(rgb_.lastError());
    } else {
        if (!want.enabled) {
            rgbView_->setImage(QImage());
            rgbView_->setSubtitle(QString());
        }
        rgbConfig_ = want;
    }
}

void MainWindow::onDepthReady()
{
    depthView_->setImage(oni_.frame(openni::SENSOR_DEPTH));
}

void MainWindow::onIrReady()
{
    irView_->setImage(oni_.frame(openni::SENSOR_IR));
}

void MainWindow::onRgbReady()
{
    rgbView_->setImage(rgb_.frame());
}

quint16 MainWindow::depthAt(const QPoint& imagePos)
{
    // Read back through the raw copy inside OpenNICamera: the QImage is RGB.
    return oni_.depthPixel(imagePos.x(), imagePos.y());
}

void MainWindow::saveSnapshot(int which)
{
    StreamView* v = which == 0 ? depthView_ : which == 1 ? irView_ : rgbView_;
    if (v->image().isNull()) {
        statusLabel_->setText(tr("nothing to save"));
        return;
    }
    const QString stem = which == 0 ? "depth" : which == 1 ? "ir" : "rgb";
    const QString suggested = QString("%1_%2.png")
                                  .arg(stem)
                                  .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save snapshot"),
        QDir::homePath() + "/" + suggested, "PNG (*.png)");
    if (path.isEmpty())
        return;
    if (v->image().save(path))
        statusLabel_->setText(tr("saved %1").arg(path));
    else
        statusLabel_->setText(tr("failed to save %1").arg(path));
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    QSettings s(settingsFile(), QSettings::IniFormat);
    s.setValue("geo", saveGeometry());
    rgb_.close();
    oni_.close();
    QMainWindow::closeEvent(e);
}
