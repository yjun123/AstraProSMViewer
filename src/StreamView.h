#pragma once

#include <QColor>
#include <QWidget>

// Central accent palette (see GUI_DESIGN.md §3).
namespace Accent {
inline const QColor depth = QColor("#4cc2ff");
inline const QColor ir = QColor("#ff9e3d");
inline const QColor rgb = QColor("#b48cff");
inline const QColor ok = QColor("#34d399");
inline const QColor warn = QColor("#fbbf24");
inline const QColor textDim = QColor("#8b93a5");
inline const QColor text = QColor("#e7ebf3");
} // namespace Accent

// A scalable image card: keeps aspect ratio, letterboxes, and reports pixel
// coordinates under the cursor so the depth view can show a distance readout.
// Renders a title band with a per-stream accent bar, the frame, and overlay
// readouts (fps / distance). See GUI_DESIGN.md §5.1.
class StreamView : public QWidget {
    Q_OBJECT
public:
    explicit StreamView(const QString& title, const QColor& accent,
                        QWidget* parent = nullptr);

    void setImage(const QImage& img);
    const QImage& image() const { return image_; }
    QSize imageSize() const { return image_.size(); }

    void setSubtitle(const QString& text);   // fps etc., top-right
    void setPixelReadout(const QString& text); // hover distance, bottom-left

signals:
    // pos is in image pixel coordinates, (-1,-1) when leaving the widget.
    void pixelHover(const QPoint& pos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    QRect imageRect() const;

    QString title_, subtitle_, readout_;
    QColor accent_;
    QImage image_;
    bool hovering_ = false;
    QPoint hoverPos_;
};
