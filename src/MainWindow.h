#pragma once

#include <QLabel>
#include <QMainWindow>

#include "OpenNICamera.h"
#include "RgbCamera.h"

class QCheckBox;
class QComboBox;
class QPushButton;
class StreamView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void restartAll();
    void saveSnapshot(int which); // 0=depth 1=ir 2=rgb
    void onDepthReady();
    void onIrReady();
    void onRgbReady();
    void saveConfig();

private:
    QWidget* buildControls();
    QWidget* buildHeader();
    void populateResCombo(QComboBox* cb, openni::SensorType type);
    void populateFmtCombo(QComboBox* cb, openni::SensorType type);
    void populateFpsCombo(QComboBox* cb, openni::SensorType type, int w, int h,
                          openni::PixelFormat fmt);
    void populateRgbModes();
    void refreshRgbFps();
    void restoreConfig();
    OpenNICamera::Mode currentDepthMode();
    OpenNICamera::Mode currentIrMode();
    // Resolve (w,h,format,fps) to an exact driver mode, falling back to the
    // nearest fps at the same resolution+format, then any mode at that res.
    OpenNICamera::Mode resolveMode(openni::SensorType type, int w, int h,
                                   openni::PixelFormat fmt, int fps);

    OpenNICamera oni_;
    RgbCamera rgb_;

    // Views.
    StreamView* depthView_ = nullptr;
    StreamView* irView_ = nullptr;
    StreamView* rgbView_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* deviceBadge_ = nullptr;

    // Controls.
    QCheckBox* depthEnableChk_ = nullptr;
    QComboBox* depthResCombo_ = nullptr;
    QComboBox* depthFmtCombo_ = nullptr;
    QComboBox* depthFpsCombo_ = nullptr;
    QCheckBox* irEnableChk_ = nullptr;
    QComboBox* irResCombo_ = nullptr;
    QComboBox* irFmtCombo_ = nullptr;
    QComboBox* irFpsCombo_ = nullptr;
    QCheckBox* depthMirrorChk_ = nullptr;
    QCheckBox* irMirrorChk_ = nullptr;
    QCheckBox* rgbEnableChk_ = nullptr;
    QComboBox* rgbFormatCombo_ = nullptr;
    QComboBox* rgbResCombo_ = nullptr;
    QComboBox* rgbFpsCombo_ = nullptr;
    QList<RgbCamera::Mode> rgbModes_;
    // Fixed UVC node of the on-board RGB camera (2bc5:0501), located at startup.
    QString rgbDevice_;
    struct RgbConfig {
        bool enabled = false;
        QString dev;
        QString fmt;
        int w = 0;
        int h = 0;
        int fps = 0;
        bool operator==(const RgbConfig& o) const {
            return enabled == o.enabled && dev == o.dev && fmt == o.fmt
                   && w == o.w && h == o.h && fps == o.fps;
        }
    };
    RgbConfig rgbConfig_;
    QPushButton* snapDepthBtn_ = nullptr;
    QPushButton* snapIrBtn_ = nullptr;
    QPushButton* snapRgbBtn_ = nullptr;

    quint16 depthAt(const QPoint& imagePos);

protected:
    void closeEvent(QCloseEvent* e) override;
};
