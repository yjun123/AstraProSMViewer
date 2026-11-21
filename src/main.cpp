#include "MainWindow.h"
#include "StreamView.h"

#include <QApplication>
#include <QDockWidget>

namespace {

// Global dark theme — see GUI_DESIGN.md §3 for the token table.
const char* kStyleSheet = R"QSS(
/* ---- base ---- */
QMainWindow, QWidget#central, QWidget#controls {
    background: #0d0f13;
}
QWidget { color: #e7ebf3; font-size: 13px; }
QToolTip { background: #1b1f29; color: #e7ebf3; border: 1px solid #262b37; padding: 4px; }

/* ---- header ---- */
QWidget#header { background: #15181f; border-bottom: 1px solid #262b37; }
QLabel#appTitle { font-size: 17px; font-weight: 700; color: #e7ebf3; }
QLabel#deviceBadge { font-size: 12px; color: #8b93a5; }
QLabel#statusLabel {
    color: #8b93a5; font-size: 12px; padding: 6px 16px;
    background: #0d0f13; border-top: 1px solid #1b1f29;
}

/* ---- dock / controls ---- */
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title {
    background: #15181f; padding: 8px 12px; border-bottom: 1px solid #262b37;
    font-weight: 600;
}

QGroupBox {
    background: #15181f; border: 1px solid #262b37; border-radius: 8px;
    margin-top: 12px; padding: 12px; font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left; left: 12px;
    padding: 0 6px;
}
QGroupBox#depthBox::title { color: #4cc2ff; }
QGroupBox#irBox::title { color: #ff9e3d; }
QGroupBox#rgbBox::title { color: #b48cff; }

QFormLayout { spacing: 8px; }

/* ---- inputs ---- */
QComboBox {
    background: #1b1f29; border: 1px solid #262b37; border-radius: 6px;
    padding: 6px 10px; min-height: 18px;
}
QComboBox:hover { border-color: #3a4150; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox::down-arrow {
    image: none; border-left: 4px solid transparent; border-right: 4px solid transparent;
    border-top: 5px solid #8b93a5; margin-right: 8px;
}
QComboBox QAbstractItemView {
    background: #1b1f29; border: 1px solid #262b37; border-radius: 6px;
    selection-background-color: #2a3448; outline: none; padding: 4px;
}

QCheckBox { spacing: 8px; }
QCheckBox::indicator {
    width: 16px; height: 16px; border: 1px solid #3a4150; border-radius: 4px;
    background: #1b1f29;
}
QCheckBox::indicator:hover { border-color: #8b93a5; }
QCheckBox::indicator:checked { background: #4cc2ff; border-color: #4cc2ff; }

/* ---- buttons ---- */
QPushButton {
    background: #1b1f29; border: 1px solid #262b37; border-radius: 6px;
    padding: 7px 12px;
}
QPushButton:hover { background: #232936; border-color: #3a4150; }
QPushButton:pressed { background: #171b24; }
QPushButton#primaryBtn {
    background: #4cc2ff; color: #08131c; font-weight: 700; border: none; padding: 9px 12px;
}
QPushButton#primaryBtn:hover { background: #6fd0ff; }
QPushButton#primaryBtn:pressed { background: #3aa8e0; }

/* ---- scrollbar ---- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #262b37; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #3a4150; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #262b37; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: #3a4150; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)QSS";

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("AstraProSMViewer");
    QApplication::setOrganizationName("E-Waste");

    MainWindow w;
    // Theme only our own widgets. Applying the stylesheet application-wide
    // would also repaint QFileDialog/QMessageBox, whose system icon set does
    // not match a dark palette; those dialogs keep the platform default.
    w.setStyleSheet(QStringLiteral("QMainWindow { background: #0d0f13; }"));
    const QString style = QString::fromLatin1(kStyleSheet);
    if (auto* central = w.centralWidget())
        central->setStyleSheet(style);
    if (auto* dock = w.findChild<QDockWidget*>(QStringLiteral("controlsDock")))
        dock->setStyleSheet(style);

    w.show();
    return QApplication::exec();
}
