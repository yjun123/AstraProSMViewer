#include "StreamView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace {
const QColor kSurface("#15181f");
const QColor kSurfaceTop("#1a1e27");
const QColor kSurfaceBottom("#11141a");
const QColor kBorder("#262b37");
constexpr int kRadius = 8;
constexpr int kHeaderH = 34;
} // namespace

StreamView::StreamView(const QString& title, const QColor& accent, QWidget* parent)
    : QWidget(parent), title_(title), accent_(accent)
{
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void StreamView::setImage(const QImage& img)
{
    image_ = img;
    update();
}

void StreamView::setSubtitle(const QString& text)
{
    subtitle_ = text;
    update();
}

void StreamView::setPixelReadout(const QString& text)
{
    readout_ = text;
    update();
}

QRect StreamView::imageRect() const
{
    const int top = kHeaderH + 6;
    const int availW = width() - 16;
    const int availH = height() - top - 8;
    if (image_.isNull() || availW <= 0 || availH <= 0)
        return {};
    const double scale = qMin(double(availW) / image_.width(),
                              double(availH) / image_.height());
    const QSize scaled(qMax<qint64>(1, qRound64(image_.width() * scale)),
                       qMax<qint64>(1, qRound64(image_.height() * scale)));
    return QRect(QPoint((width() - scaled.width()) / 2,
                        top + (availH - scaled.height()) / 2),
                 scaled);
}

void StreamView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Card background with a subtle vertical gradient.
    QLinearGradient grad(0, 0, 0, height());
    grad.setColorAt(0, kSurfaceTop);
    grad.setColorAt(1, kSurfaceBottom);
    QPainterPath card;
    card.addRoundedRect(rect().adjusted(0, 0, -1, -1), kRadius, kRadius);
    p.fillPath(card, grad);
    p.setPen(QPen(kBorder, 1));
    p.drawPath(card);

    // Header: accent bar + title (left), fps subtitle (right).
    p.fillRect(QRect(kRadius, 4, 4, kHeaderH - 12), accent_);
    const int basePx = qMax(12, font().pixelSize()); // QSS sets pixelSize, not pointSize
    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPixelSize(basePx + 2);
    p.setFont(titleFont);
    p.setPen(Accent::text);
    p.drawText(QRect(kRadius + 12, 0, width() - kRadius * 2 - 12, kHeaderH),
               Qt::AlignLeft | Qt::AlignVCenter, title_);

    if (!subtitle_.isEmpty()) {
        QFont f = font();
        f.setBold(false);
        f.setPixelSize(qMax(10, basePx - 1));
        f.setFamily("monospace");
        p.setFont(f);
        p.setPen(Accent::textDim);
        p.drawText(QRect(0, 0, width() - kRadius - 10, kHeaderH),
                   Qt::AlignRight | Qt::AlignVCenter, subtitle_);
    }

    if (!image_.isNull()) {
        const QRect r = imageRect();
        QPainterPath clip;
        clip.addRoundedRect(r.adjusted(-1, -1, 1, 1), 6, 6);
        p.setClipPath(clip);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, image_);
        p.setClipping(false);
        p.setPen(QPen(kBorder, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), 6, 6);
    } else {
        p.setFont(titleFont);
        p.setPen(Accent::textDim);
        p.drawText(rect().adjusted(0, kHeaderH, 0, 0),
                   Qt::AlignCenter, tr("no signal"));
    }

    // Hover readout (depth distance) — amber monospace on a pill.
    if (!readout_.isEmpty()) {
        QFont rf = font();
        rf.setFamily("monospace");
        rf.setPixelSize(qMax(10, basePx - 1));
        p.setFont(rf);
        const QString s = readout_;
        QFontMetrics fm(rf);
        const QRect textRect = fm.boundingRect(s).adjusted(-10, -4, 10, 4);
        const QRect pillRect(QRect(QPoint(12, height() - textRect.height() - 10),
                                   textRect.size()));
        QPainterPath pill;
        pill.addRoundedRect(pillRect, 6, 6);
        p.fillPath(pill, QColor(12, 12, 16, 190));
        p.setPen(QPen(QColor("#fbbf24")));
        p.drawText(pillRect, Qt::AlignCenter, s);
    }
}

void StreamView::mouseMoveEvent(QMouseEvent* e)
{
    const QRect r = imageRect();
    if (!image_.isNull() && r.contains(e->pos())) {
        hovering_ = true;
        hoverPos_ = QPoint(
            (e->pos().x() - r.x()) * image_.width() / qMax(1, r.width()),
            (e->pos().y() - r.y()) * image_.height() / qMax(1, r.height()));
    } else {
        hovering_ = false;
    }
    emit pixelHover(hovering_ ? hoverPos_ : QPoint(-1, -1));
}

void StreamView::leaveEvent(QEvent*)
{
    hovering_ = false;
    emit pixelHover(QPoint(-1, -1));
}
