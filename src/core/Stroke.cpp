#include "Stroke.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>
#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace ink {

static inline QPointF mid(const QPointF &a, const QPointF &b) { return (a + b) / 2; }
static inline qreal dist(const QPointF &a, const QPointF &b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

static float s_thin = 0.3f, s_thick = 1.7f;

void StrokeBuilder::setPressureRange(float thinnest, float thickest)
{
    s_thin = qBound(0.05f, thinnest, 1.0f);
    s_thick = qBound(1.0f, thickest, 4.0f);
}
float StrokeBuilder::pressureThinnest() { return s_thin; }
float StrokeBuilder::pressureThickest() { return s_thick; }

float StrokeBuilder::widthFor(float pressure) const
{
    return pressure_ ? base_ * (s_thin + (s_thick - s_thin) * qBound(0.f, pressure, 1.f)) : base_;
}

void StrokeBuilder::begin(QPointF p, float pressure, qint64 t, float baseWidth, bool usePressure, double scale)
{
    f_.clear(); fw_.clear(); out_.clear(); outW_.clear();
    base_ = baseWidth;
    pressure_ = usePressure && pressure >= 0;
    scale_ = scale > 0 ? scale : 1;
    lastT_ = t;
    shape_ = Shape::None;
    adjusting_ = false;
    snapPts_.clear();
    float w = widthFor(pressure);
    f_.push_back(p); fw_.push_back(w);
    out_.push_back(p); outW_.push_back(w);
    stable_ = 1;
}

static QPointF snapAngle(const QPointF &a, QPointF b)
{
    QPointF d = b - a;
    qreal ang = std::atan2(d.y(), d.x()) * 180 / M_PI;
    qreal near90 = std::round(ang / 90) * 90;
    if (std::fabs(ang - near90) < 3.0) {
        if (int(std::lround(near90 / 90)) % 2 == 0) b.setY(a.y());
        else b.setX(a.x());
    }
    return b;
}

int StrokeBuilder::add(QPointF p, float pressure, qint64 t)
{
    if (shape_ != Shape::None) {
        // Keep the pen down after the snap and drag: a line follows with its end point,
        // a rectangle / ellipse is resized against its opposite corner.
        if (!adjusting_) {
            if (dist(p, grab_) * scale_ < 6) return out_.size();
            adjusting_ = true;
            grab_ = p;
        }
        if (shape_ == Shape::Line) {
            out_[1] = snapAngle(out_[0], p);
            return 0;
        }
        QPointF d0 = grab_ - anchor_, d1 = p - anchor_;
        auto clampScale = [](qreal s) { return std::fabs(s) < 0.05 ? (s < 0 ? -0.05 : 0.05) : s; };
        qreal sx = 1, sy = 1;
        if (uniform_) sx = sy = clampScale(std::hypot(d1.x(), d1.y()) / qMax(1e-6, std::hypot(d0.x(), d0.y())));
        else {
            QRectF bb = QPolygonF(snapPts_).boundingRect();
            if (std::fabs(d0.x()) > 0.2 * bb.width()) sx = clampScale(d1.x() / d0.x());
            if (std::fabs(d0.y()) > 0.2 * bb.height()) sy = clampScale(d1.y() / d0.y());
        }
        for (int i = 0; i < snapPts_.size(); ++i)
            out_[i] = anchor_ + QPointF((snapPts_[i].x() - anchor_.x()) * sx, (snapPts_[i].y() - anchor_.y()) * sy);
        return 0;
    }

    // 1. Jitter filter: strong smoothing for slow movement, none for fast strokes (no lag).
    const QPointF prev = f_.last();
    qreal dpx = dist(p, prev) * scale_;
    qreal dt = qMax<qint64>(1, t - lastT_);
    qreal alpha = qBound(0.4, 0.4 + (dpx / dt) * 0.5, 1.0);
    QPointF fp = prev + (p - prev) * alpha;
    if (dist(fp, prev) * scale_ < 0.5) return out_.size();
    lastT_ = t;
    float target = widthFor(pressure);
    float w = fw_.last() + 0.35f * (target - fw_.last());
    f_.push_back(fp);
    fw_.push_back(w);

    // 2. Smooth curve: quadratic Béziers through the midpoints of the filtered samples.
    int first = stable_ - 1;
    out_.resize(stable_);
    outW_.resize(stable_);
    int n = f_.size();
    if (n >= 3) {
        QPointF a = out_.last(), ctrl = f_[n - 2], b = mid(f_[n - 2], f_[n - 1]);
        float wa = outW_.last(), wb = (fw_[n - 2] + fw_[n - 1]) / 2;
        int steps = qBound(1, int(std::ceil((dist(a, ctrl) + dist(ctrl, b)) * scale_ / 2.5)), 32);
        for (int i = 1; i <= steps; ++i) {
            qreal u = qreal(i) / steps, v = 1 - u;
            out_.push_back(a * (v * v) + ctrl * (2 * u * v) + b * (u * u));
            outW_.push_back(wa + (wb - wa) * float(u));
        }
        stable_ = out_.size();
    }
    out_.push_back(f_.last());       // provisional tail up to the pen tip
    outW_.push_back(fw_.last());
    return qMax(0, first);
}

bool StrokeBuilder::snapToShape()
{
    if (shape_ != Shape::None || f_.size() < 3) return false;
    qreal len = 0;
    for (int i = 1; i < f_.size(); ++i) len += dist(f_[i - 1], f_[i]);
    if (len * scale_ < 24) return false;
    const QPointF a = f_.first(), b = f_.last();
    qreal chord = dist(a, b);
    float wsum = 0;
    for (float w : fw_) wsum += w;
    float wavg = wsum / fw_.size();
    auto finish = [&](Shape s, const QVector<QPointF> &pts, bool uniform = false) {
        out_ = pts;
        outW_.fill(wavg, pts.size());
        stable_ = out_.size();
        shape_ = s;
        snapPts_ = pts;
        grab_ = b;
        adjusting_ = false;
        QRectF bb = QPolygonF(pts).boundingRect();
        uniform_ = uniform || (s != Shape::Line && std::fabs(bb.width() - bb.height()) < 1e-6);
        anchor_ = bb.topLeft();      // the corner farthest from the pen stays fixed
        for (const QPointF &c : {bb.topRight(), bb.bottomRight(), bb.bottomLeft()})
            if (dist(c, b) > dist(anchor_, b)) anchor_ = c;
        return true;
    };

    bool closed = chord < 0.25 * len && len * scale_ > 60;
    if (!closed) {
        qreal maxDev = 0;
        QLineF l(a, b);
        for (const QPointF &p : f_) {
            qreal cross = std::fabs((p.x() - a.x()) * l.dy() - (p.y() - a.y()) * l.dx());
            maxDev = qMax(maxDev, cross / qMax(chord, 1e-6));
        }
        if (maxDev > 0.10 * chord + 4 / scale_) return false;
        return finish(Shape::Line, {a, snapAngle(a, b)});
    }

    // Rectangle and ellipse are both fitted at small rotations and compared by their mean distance to
    // the samples, in page units: relative errors made a long thin rectangle (a box around a line of
    // text) almost impossible to draw. A rectangle's edges are robust quantiles of the samples, so an
    // overshoot at a corner or past the starting point does not move them; an ellipse touches the extremes.
    const int n = f_.size();
    const QPointF centroid = std::accumulate(f_.begin(), f_.end(), QPointF()) / n;
    QVector<qreal> xs(n), ys(n);
    struct Fit { qreal angle = 0, dist = 1e300; QRectF box; };
    auto quantile = [n](QVector<qreal> v, qreal q) {
        const int k = qBound(0, int(std::lround(q * (n - 1))), n - 1);
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    };
    auto fitAt = [&](qreal deg, Fit &rect, Fit &ell) {
        const qreal t = deg * M_PI / 180, ct = std::cos(t), st = std::sin(t);
        for (int i = 0; i < n; ++i) {
            const QPointF d = f_[i] - centroid;
            xs[i] = d.x() * ct + d.y() * st;      // turned by -angle
            ys[i] = -d.x() * st + d.y() * ct;
        }
        const QRectF rb(QPointF(quantile(xs, 0.03), quantile(ys, 0.03)), QPointF(quantile(xs, 0.97), quantile(ys, 0.97)));
        const QRectF eb(QPointF(*std::min_element(xs.begin(), xs.end()), *std::min_element(ys.begin(), ys.end())),
                        QPointF(*std::max_element(xs.begin(), xs.end()), *std::max_element(ys.begin(), ys.end())));
        const qreal erx = qMax(eb.width() / 2, 1e-6), ery = qMax(eb.height() / 2, 1e-6);
        const QPointF ec = eb.center();
        qreal rd = 0, ed = 0;
        for (int i = 0; i < n; ++i) {
            rd += qMin(qMin(std::fabs(xs[i] - rb.left()), std::fabs(xs[i] - rb.right())),
                       qMin(std::fabs(ys[i] - rb.top()), std::fabs(ys[i] - rb.bottom())));
            const qreal dx = xs[i] - ec.x(), dy = ys[i] - ec.y();   // first-order (Sampson) distance to the ellipse
            const qreal F = dx * dx / (erx * erx) + dy * dy / (ery * ery) - 1;
            ed += std::fabs(F) / qMax(2 * std::hypot(dx / (erx * erx), dy / (ery * ery)), 1e-9);
        }
        rect = Fit{deg, rd / n, rb};
        ell = Fit{deg, ed / n, eb};
    };
    Fit rect, ell, rect0, ell0;
    fitAt(0, rect0, ell0);
    rect = rect0;
    ell = ell0;
    for (int deg = -20; deg <= 20; ++deg) {
        if (deg == 0) continue;
        Fit r, e;
        fitAt(deg, r, e);
        if (r.dist < rect.dist) rect = r;
        if (e.dist < ell.dist) ell = e;
    }
    // Nearly level and the level fit hardly worse: meant to be level.
    const qreal levelSlack = 1.5 / scale_;
    if (rect0.dist <= rect.dist + levelSlack) rect = rect0;
    if (ell0.dist <= ell.dist + levelSlack) ell = ell0;

    auto turned = [&](const Fit &f, QPointF local) {
        const qreal t = f.angle * M_PI / 180;
        return centroid + QPointF(local.x() * std::cos(t) - local.y() * std::sin(t), local.x() * std::sin(t) + local.y() * std::cos(t));
    };
    const qreal shortHalf = qMin(rect.box.width(), rect.box.height()) / 2;
    if (rect.dist < ell.dist && shortHalf * scale_ >= 6 && rect.dist < 0.1 * shortHalf + 4 / scale_) {
        QRectF r = rect.box;
        if (std::fabs(r.width() - r.height()) < 0.12 * qMax(r.width(), r.height())) {   // square
            const qreal side = (r.width() + r.height()) / 2;
            r = QRectF(r.center() - QPointF(side / 2, side / 2), QSizeF(side, side));
        }
        return finish(Shape::Rect, {turned(rect, r.topLeft()), turned(rect, r.topRight()), turned(rect, r.bottomRight()),
                                    turned(rect, r.bottomLeft()), turned(rect, r.topLeft())},
                      rect.angle != 0);   // a turned shape is resized uniformly, so it keeps its form
    }
    qreal erx = ell.box.width() / 2, ery = ell.box.height() / 2;
    if (qMin(erx, ery) * scale_ < 6) return false;
    if (std::fabs(erx - ery) < 0.12 * qMax(erx, ery)) erx = ery = (erx + ery) / 2;   // circle
    if (ell.dist < 0.14 * qMin(erx, ery) + 2 / scale_) {
        QVector<QPointF> pts;
        for (int i = 0; i <= 72; ++i) {
            qreal t = 2 * M_PI * i / 72;
            pts.push_back(turned(ell, ell.box.center() + QPointF(erx * std::cos(t), ery * std::sin(t))));
        }
        return finish(Shape::Ellipse, pts, ell.angle != 0);
    }
    return false;
}

InkStroke StrokeBuilder::result() const
{
    InkStroke s;
    s.pts = out_;
    if (pressure_ && shape_ == Shape::None) s.w = outW_;
    return s;
}

void paintInk(QPainter &p, const InkStroke &s, const QColor &c, float width, bool highlighter, int from)
{
    int n = s.pts.size();
    if (n == 0) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (highlighter) p.setCompositionMode(QPainter::CompositionMode_Multiply);
    QPen pen(c, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (s.w.size() == n && n > 1) {
        for (int i = qMax(1, from); i < n; ++i) {
            pen.setWidthF((s.w[i - 1] + s.w[i]) / 2);
            p.setPen(pen);
            p.drawLine(s.pts[i - 1], s.pts[i]);
        }
    } else {
        if (s.w.size() == n) pen.setWidthF(s.w[0]);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        if (n == 1) p.drawPoint(s.pts[0]);
        else {
            QPainterPath path(s.pts[0]);
            for (int i = 1; i < n; ++i) path.lineTo(s.pts[i]);
            p.drawPath(path);
        }
    }
    p.restore();
}

void paintInk(QPainter &p, const InkData &d)
{
    for (const InkStroke &s : d.strokes) paintInk(p, s, d.color, d.width, d.highlighter);
}

bool inkHit(const InkData &d, QPointF pt, qreal radius)
{
    for (const InkStroke &s : d.strokes) {
        int n = s.pts.size();
        for (int i = 0; i < n; ++i) {
            qreal w = (s.w.size() == n ? s.w[i] : d.width) / 2 + radius;
            const QPointF a = s.pts[i], b = s.pts[qMin(i + 1, n - 1)];
            QPointF ab = b - a;
            qreal l2 = ab.x() * ab.x() + ab.y() * ab.y();
            qreal t = l2 > 0 ? qBound(0.0, ((pt.x() - a.x()) * ab.x() + (pt.y() - a.y()) * ab.y()) / l2, 1.0) : 0;
            if (dist(pt, a + ab * t) <= w) return true;
        }
    }
    return false;
}

QImage inkImage(const InkData &d, int pixelWidth, const QColor &background)
{
    QRectF b = d.bounds();
    if (b.isEmpty() || pixelWidth <= 0) return QImage();
    qreal s = pixelWidth / b.width();
    QImage img(pixelWidth, qMax(1, int(std::ceil(b.height() * s))), QImage::Format_ARGB32_Premultiplied);
    img.fill(background);
    QPainter p(&img);
    p.scale(s, s);
    p.translate(-b.topLeft());
    InkData copy = d;
    copy.highlighter = false;
    paintInk(p, copy);
    return img;
}

QJsonObject inkToJson(const InkData &d)
{
    QJsonArray strokes;
    for (const InkStroke &s : d.strokes) {
        QJsonArray pts, w;
        for (const QPointF &p : s.pts) { pts.append(std::round(p.x() * 100) / 100); pts.append(std::round(p.y() * 100) / 100); }
        for (float v : s.w) w.append(std::round(v * 100) / 100);
        strokes.append(QJsonObject{{"p", pts}, {"w", w}});
    }
    return QJsonObject{{"color", d.color.name()}, {"width", d.width}, {"strokes", strokes}};
}

InkData inkFromJson(const QJsonObject &o)
{
    InkData d;
    d.color = QColor(o.value("color").toString("#000000"));
    d.width = float(o.value("width").toDouble(1.5));
    for (const QJsonValue &sv : o.value("strokes").toArray()) {
        InkStroke s;
        QJsonArray pts = sv.toObject().value("p").toArray();
        for (int i = 0; i + 1 < pts.size(); i += 2) s.pts.push_back(QPointF(pts[i].toDouble(), pts[i + 1].toDouble()));
        for (const QJsonValue &w : sv.toObject().value("w").toArray()) s.w.push_back(float(w.toDouble()));
        if (s.w.size() != s.pts.size()) s.w.clear();
        d.strokes.push_back(s);
    }
    return d;
}

} // namespace ink
