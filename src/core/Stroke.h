// Ink engine: turns raw pen samples into smooth, pressure-sensitive strokes,
// recognises shapes when the pen is held still, paints and hit-tests ink.
#pragma once
#include "Document.h"
#include <QImage>
#include <QPainter>

namespace ink {

class StrokeBuilder {
public:
    enum class Shape { None, Line, Rect, Ellipse };

    // scale = screen pixels per PDF point; all tolerances are defined in screen pixels.
    void begin(QPointF p, float pressure, qint64 tMs, float baseWidth, bool usePressure, double scale);
    // Feed a sample. Returns the index of the first output point that changed.
    int add(QPointF p, float pressure, qint64 tMs);
    // Called when the pen rests: replace the stroke by a clean line / rectangle / ellipse.
    bool snapToShape();
    Shape shape() const { return shape_; }

    const QVector<QPointF> &points() const { return out_; }
    const QVector<float> &widths() const { return outW_; }
    int stableCount() const { return stable_; }   // points [0, stable) will not change any more
    InkStroke result() const;
    float baseWidth() const { return base_; }

    // Pen pressure: the width at the lightest and at full pressure, as factors of the chosen width
    // (defaults 0.3 and 1.7). Applies to strokes begun afterwards.
    static void setPressureRange(float thinnest, float thickest);
    static float pressureThinnest();
    static float pressureThickest();

private:
    float widthFor(float pressure) const;
    QVector<QPointF> f_;     // filtered input
    QVector<float> fw_;
    QVector<QPointF> out_;   // smooth output polyline
    QVector<float> outW_;
    int stable_ = 0;
    float base_ = 1.5f;
    bool pressure_ = false;
    double scale_ = 1;
    qint64 lastT_ = 0;
    Shape shape_ = Shape::None;
    // snapped shape: geometry at snap time, the fixed corner and where the pen was
    QVector<QPointF> snapPts_;
    QPointF anchor_, grab_;
    bool uniform_ = false, adjusting_ = false;
};

// Painter must be set up in page coordinates (points).
void paintInk(QPainter &p, const InkStroke &s, const QColor &c, float width, bool highlighter, int from = 0);
void paintInk(QPainter &p, const InkData &d);
bool inkHit(const InkData &d, QPointF pt, qreal radius);
QImage inkImage(const InkData &d, int pixelWidth, const QColor &background = Qt::transparent);

QJsonObject inkToJson(const InkData &d);
InkData inkFromJson(const QJsonObject &o);

} // namespace ink
