#include "PdfView.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include "Theme.h"
#include <QVBoxLayout>
#include <QScreen>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QToolButton>
#include <QTouchEvent>
#include <QStyle>
#include <QTabletEvent>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <QLineF>

using namespace ink;

static const qreal kGap = 12, kMargin = 12;
static double s_scrollSpeed = 0.8;
static bool s_noteAfterMarking = true;
void PdfView::setNoteAfterMarking(bool on) { s_noteAfterMarking = on; }
bool PdfView::noteAfterMarking() { return s_noteAfterMarking; }
static QList<PdfView *> s_views;
static QElapsedTimer s_penClock;        // time since any pen was seen (hovering or touching)
static qint64 s_lastPenMs = -100000;

void PdfView::setScrollSpeed(double f) { s_scrollSpeed = qBound(0.1, f, 4.0); }
double PdfView::scrollSpeed() { return s_scrollSpeed; }

void PdfView::setPageTheme(PageTheme t)
{
    PageColors::setTheme(t);
    for (PdfView *v : std::as_const(s_views)) {
        v->tiles_.clear();
        v->requested_.clear();
        v->rebuildOverlay();
        v->viewport()->update();
    }
}

PdfView::PdfView(std::shared_ptr<Document> doc, QWidget *parent)
    : QAbstractScrollArea(parent), doc_(std::move(doc)), worker_(new RenderWorker(doc_, this))
{
    clock_.start();
    if (!s_penClock.isValid()) s_penClock.start();
    s_views.push_back(this);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
    viewport()->setMouseTracking(true);
    viewport()->setTabletTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    verticalScrollBar()->setSingleStep(48);
    horizontalScrollBar()->setSingleStep(48);

    connect(worker_, &RenderWorker::rendered, this, &PdfView::onRendered);
    connect(doc_.get(), &Document::pageChanged, this, &PdfView::onPageChanged);
    connect(doc_.get(), &Document::structureChanged, this, &PdfView::onStructureChanged);

    commitTimer_.setInterval(12);
    connect(&commitTimer_, &QTimer::timeout, this, [this] { tryCommit(false); });
    holdTimer_.setSingleShot(true);
    holdTimer_.setInterval(480);
    connect(&holdTimer_, &QTimer::timeout, this, &PdfView::onHold);
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(350);
    connect(&previewTimer_, &QTimer::timeout, this, &PdfView::showPreview);
    connect(&searchTimer_, &QTimer::timeout, this, &PdfView::runSearchChunk);
    flingTimer_.setInterval(16);
    connect(&flingTimer_, &QTimer::timeout, this, [this] {
        scrollBy(flingV_ * 16);
        flingV_ *= 0.94;
        if (std::hypot(flingV_.x(), flingV_.y()) < 0.04) flingTimer_.stop();
    });
    settleTimer_.setSingleShot(true);
    settleTimer_.setInterval(1500);
    connect(&settleTimer_, &QTimer::timeout, this, &PdfView::onSettle);
    previewPoll_.setInterval(250);
    connect(&previewPoll_, &QTimer::timeout, this, &PdfView::pollPreview);
    relayout();
}

PdfView::~PdfView()
{
    s_views.removeAll(this);
    try { flushInk(); } catch (...) {}
    delete preview_;
    delete previewFrame_;
}

// ------------------------------------------------------------------ geometry
QPointF PdfView::offset() const { return QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value()); }

void PdfView::relayout()
{
    int n = doc_->pageCount();
    pageRects_.resize(n);
    qreal s = ppp(), maxW = 0;
    for (int i = 0; i < n; ++i) maxW = qMax(maxW, doc_->pageSize(i).width() * s);
    qreal W = qMax<qreal>(viewport()->width(), maxW + 2 * kMargin), y = kMargin;
    for (int i = 0; i < n; ++i) {
        QSizeF sz = doc_->pageSize(i) * s;
        pageRects_[i] = QRectF(std::round((W - sz.width()) / 2), std::round(y), sz.width(), sz.height());
        y += sz.height() + kGap;
    }
    docSize_ = QSizeF(W, y - kGap + kMargin);
    updateScrollBars();
    emit viewChanged();
}

void PdfView::updateScrollBars()
{
    QSize vp = viewport()->size();
    horizontalScrollBar()->setRange(0, qMax(0, int(std::ceil(docSize_.width())) - vp.width()));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(docSize_.height())) - vp.height()));
    verticalScrollBar()->setPageStep(vp.height());
}

int PdfView::pageAt(QPointF viewPos, bool nearest) const
{
    QPointF d = viewPos + offset();
    int best = -1;
    qreal bestDist = 1e18;
    for (int i = 0; i < pageRects_.size(); ++i) {
        const QRectF &r = pageRects_[i];
        if (r.contains(d)) return i;
        if (nearest) {
            qreal dy = d.y() < r.top() ? r.top() - d.y() : d.y() - r.bottom();
            if (dy < bestDist) { bestDist = dy; best = i; }
        }
        if (r.top() > d.y() && !nearest) break;
    }
    return best;
}

QPointF PdfView::toPage(int page, QPointF viewPos) const { return (viewPos + offset() - pageRects_[page].topLeft()) / ppp(); }
QRectF PdfView::pageViewRect(int page) const { return pageRects_[page].translated(-offset()); }
QRectF PdfView::toView(int page, const QRectF &r) const
{
    QPointF o = pageViewRect(page).topLeft();
    qreal s = ppp();
    return QRectF(o + r.topLeft() * s, r.size() * s);
}
void PdfView::pagePainter(QPainter &p, int page) const
{
    p.translate(pageViewRect(page).topLeft());
    p.scale(ppp(), ppp());
}

int PdfView::currentPage() const
{
    if (pageRects_.isEmpty()) return 0;
    return qMax(0, pageAt(QPointF(viewport()->width() / 2.0, viewport()->height() / 3.0), true));
}

// ---- navigation history: jumps and settled scroll positions, so "back" returns to where you read before
QPair<int, double> PdfView::currentPos() const
{
    if (pageRects_.isEmpty()) return {0, 0};
    int pg = qMax(0, pageAt(QPointF(viewport()->width() / 2.0, 0), true));
    return {pg, (verticalScrollBar()->value() - pageRects_[pg].top()) / ppp()};
}

double PdfView::docY(const QPair<int, double> &p) const
{
    if (pageRects_.isEmpty()) return 0;
    return pageRects_[qBound(0, p.first, int(pageRects_.size()) - 1)].top() + p.second * ppp();
}

void PdfView::record(const QPair<int, double> &p)
{
    if (!back_.isEmpty() && std::fabs(docY(back_.last()) - docY(p)) < viewport()->height() * 0.3) return;
    back_.push_back(p);
    while (back_.size() > 100) back_.removeFirst();
    fwd_.clear();
    emit historyChanged();
}

void PdfView::onSettle()
{
    settleTimer_.stop();
    QPair<int, double> cur = currentPos();
    if (std::fabs(docY(cur) - docY(settled_)) > viewport()->height()) record(settled_);
    settled_ = cur;
}

void PdfView::pushHistory()
{
    if (pageRects_.isEmpty()) return;
    onSettle();
    record(settled_);
}

void PdfView::jumpTo(const QPair<int, double> &p)
{
    if (pageRects_.isEmpty()) return;
    verticalScrollBar()->setValue(int(std::round(docY(p))));
    settleTimer_.stop();
    settled_ = currentPos();
    emit historyChanged();
}

void PdfView::gotoPage(int page, double y, bool history)
{
    if (page < 0 || page >= pageRects_.size()) return;
    if (history) pushHistory();
    verticalScrollBar()->setValue(int(pageRects_[page].top() + y * ppp() - 8));
    if (history) {
        settleTimer_.stop();
        settled_ = currentPos();
    }
}

ViewState PdfView::viewState() const
{
    ViewState s;
    s.zoom = zoom_;
    if (pageRects_.isEmpty()) return s;
    s.page = qMax(0, pageAt(QPointF(viewport()->width() / 2.0, 0), true));
    s.y = (verticalScrollBar()->value() - pageRects_[s.page].top()) / ppp();
    return s;
}

void PdfView::setViewState(const ViewState &s)
{
    if (s.zoom > 0) setZoom(s.zoom);
    if (s.page >= 0 && s.page < pageRects_.size())
        verticalScrollBar()->setValue(int(std::round(pageRects_[s.page].top() + s.y * ppp())));
    settleTimer_.stop();
    settled_ = currentPos();
}

void PdfView::copyAreaAsImage()
{
    startRectPick([this](int page, QRectF r) {
        if (r.width() < 2 || r.height() < 2) return;
        const double s = 3.0;       // 216 dpi
        QImage img;
        try {
            flushInk();
            img = doc_->render(page, s, QRectF(r.topLeft() * s, r.size() * s).toAlignedRect());
        } catch (const PdfError &err) { emit statusMessage(err.message()); }
        if (img.isNull()) return;
        QApplication::clipboard()->setImage(img);
        emit statusMessage(tr("Copied %1 × %2 px image to the clipboard").arg(img.width()).arg(img.height()));
        updateCursor(tool_);
    }, tr("Drag a rectangle around the area to copy. Esc cancels."));
}

void PdfView::goBack()
{
    onSettle();
    if (back_.isEmpty()) return;
    QPair<int, double> target = back_.takeLast();
    fwd_.push_back(currentPos());
    jumpTo(target);
}

void PdfView::goForward()
{
    onSettle();
    if (fwd_.isEmpty()) return;
    QPair<int, double> target = fwd_.takeLast();
    back_.push_back(currentPos());
    jumpTo(target);
}

void PdfView::setZoom(double z, QPointF anchor)
{
    z = qBound(0.1, z, 8.0);
    if (qFuzzyCompare(z, zoom_) || pageRects_.isEmpty()) return;
    commitTextEdit();
    commitFieldEdit();
    hideSelBar();
    if (anchor.x() < 0) anchor = QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0);
    int pg = pageAt(anchor, true);
    QPointF pt = toPage(pg, anchor);
    zoom_ = z;
    relayout();
    QPointF target = pageRects_[pg].topLeft() + pt * ppp() - anchor;
    horizontalScrollBar()->setValue(int(std::round(target.x())));
    verticalScrollBar()->setValue(int(std::round(target.y())));
    rebuildOverlay();
    hidePreview();
    viewport()->update();
    emit zoomChanged(zoom_);
}

void PdfView::fitWidth()
{
    qreal maxW = 1;
    for (int i = 0; i < doc_->pageCount(); ++i) maxW = qMax(maxW, doc_->pageSize(i).width());
    int sb = verticalScrollBar()->isVisible() ? 0 : style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    setZoom((viewport()->width() - sb - 2 * kMargin) / (maxW * 96.0 / 72.0));
}

void PdfView::fitPage()
{
    if (pageRects_.isEmpty()) return;
    int pg = currentPage();
    QSizeF s = doc_->pageSize(pg) * (96.0 / 72.0);
    setZoom(qMin((viewport()->width() - 2 * kMargin) / s.width(), (viewport()->height() - 2 * kMargin) / s.height()));
    gotoPage(pg, 0, false);
}

void PdfView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    relayout();
    rebuildOverlay();
}

void PdfView::scrollContentsBy(int, int)
{
    if (textEdit_) layoutTextEdit();
    if (fieldEdit_) layoutFieldEdit();
    hideSelBar();
    settleTimer_.start();
    emit viewChanged();
    rebuildOverlay();
    hidePreview();
    viewport()->update();
    int cur = currentPage();
    if (cur != lastCurrent_) {
        lastCurrent_ = cur;
        emit currentPageChanged(cur);
    }
}

// ------------------------------------------------------------------ rendering
void PdfView::requestTiles()
{
    if (pageRects_.isEmpty()) return;
    const qreal dpr = viewport()->devicePixelRatioF();
    const double s = ppp() * dpr;
    const quint64 gen = doc_->generation();
    QRectF vp(offset(), QSizeF(viewport()->size()));
    QRectF wide = vp.adjusted(0, -vp.height() * 0.6, 0, vp.height() * 0.6);
    for (int i = 0; i < pageRects_.size(); ++i) {
        const QRectF &r = pageRects_[i];
        if (r.bottom() < wide.top()) continue;
        if (r.top() > wide.bottom()) break;
        QSizeF ps = doc_->pageSize(i);
        QRect full(0, 0, int(std::ceil(ps.width() * s)), int(std::ceil(ps.height() * s)));
        QRect need = full, clip = full;
        if (qint64(full.width()) * full.height() > 16000000) {   // deep zoom: render the visible part only
            QRectF vis = vp.intersected(r).translated(-r.topLeft());
            need = QRectF(vis.topLeft() * dpr, vis.size() * dpr).toAlignedRect().intersected(full);
            if (need.isEmpty()) continue;
            int mx = int(vp.width() * dpr * 0.35), my = int(vp.height() * dpr * 0.35);
            clip = need.adjusted(-mx, -my, mx, my).intersected(full);
        }
        auto covers = [&](quint64 g, double sc, const QRect &c) { return g == gen && qFuzzyCompare(sc, s) && c.contains(need); };
        auto t = tiles_.constFind(i);
        if (t != tiles_.constEnd() && covers(t->gen, t->scale, t->clip)) continue;
        auto rq = requested_.constFind(i);
        if (rq != requested_.constEnd() && covers(rq->gen, rq->scale, rq->clip)) continue;
        RenderJob job{0, i, s, clip, gen, int(PageColors::theme())};
        requested_.insert(i, job);
        worker_->request(job);
    }
}

void PdfView::onRendered(const RenderJob &job, const QImage &img)
{
    if (job.kind != 0) return;
    auto rq = requested_.find(job.page);
    if (rq != requested_.end() && rq->gen == job.gen && rq->clip == job.clip && qFuzzyCompare(rq->scale, job.scale)) requested_.erase(rq);
    if (job.page >= pageRects_.size() || img.isNull() || job.theme != int(PageColors::theme())) return;
    Tile &t = tiles_[job.page];
    if (job.gen < t.gen) return;
    t = Tile{img, job.scale, job.clip, job.gen};
    bool dropped = false;
    for (int i = pending_.size() - 1; i >= 0; --i)
        if (pending_[i].committed && pending_[i].page == job.page && pending_[i].gen <= job.gen) {
            pending_.removeAt(i);
            dropped = true;
        }
    if (dropped) rebuildOverlay();
    int cur = currentPage();
    for (auto it = tiles_.begin(); it != tiles_.end();)
        it = std::abs(it.key() - cur) > 6 ? tiles_.erase(it) : std::next(it);
    viewport()->update();
}

void PdfView::onPageChanged(int)
{
    linkCache_.clear();
    fieldCache_.clear();
    viewport()->update();
}

void PdfView::onStructureChanged()
{
    cancelFieldEdit();
    tiles_.clear();
    requested_.clear();
    linkCache_.clear();
    fieldCache_.clear();
    hits_.clear();
    searchTotal_ = 0;
    selPage_ = -1;
    selQuads_.clear();
    annPage_ = -1;
    erasePage_ = -1;
    for (int i = pending_.size() - 1; i >= 0; --i)
        if (pending_[i].committed) pending_.removeAt(i);
    relayout();
    rebuildOverlay();
    viewport()->update();
    emit currentPageChanged(currentPage());
}

static void drawSegments(QPainter &p, const QVector<QPointF> &pts, const QVector<float> &w, int from, int to, const QColor &c)
{
    QPen pen(c, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (to == 1 && from <= 0) {
        pen.setWidthF(w[0]);
        p.setPen(pen);
        p.drawPoint(pts[0]);
    }
    for (int i = qMax(1, from); i < to; ++i) {
        pen.setWidthF((w[i - 1] + w[i]) / 2);
        p.setPen(pen);
        p.drawLine(pts[i - 1], pts[i]);
    }
}

void PdfView::rebuildOverlay()
{
    const qreal dpr = viewport()->devicePixelRatioF();
    QSize want = viewport()->size() * dpr;
    if (overlay_.size() != want || overlay_.isNull()) {
        overlay_ = QImage(want.expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        overlay_.setDevicePixelRatio(dpr);
    }
    overlay_.fill(Qt::transparent);
    liveDrawn_ = 0;
    if (pending_.isEmpty() && drag_ != Drag::Draw) return;
    QPainter p(&overlay_);
    p.setRenderHint(QPainter::Antialiasing);
    for (const Pending &pd : std::as_const(pending_)) {
        if (pd.d.highlighter || pd.page >= pageRects_.size()) continue;
        p.save();
        pagePainter(p, pd.page);
        InkData themed = pd.d;
        themed.color = PageColors::map(themed.color);
        paintInk(p, themed);
        p.restore();
    }
    if (drag_ == Drag::Draw && !drawHighlighter_) {
        pagePainter(p, drawPage_);
        drawSegments(p, builder_.points(), builder_.widths(), 0, builder_.stableCount(), PageColors::map(pen_.color));
        liveDrawn_ = builder_.stableCount();
    }
}

void PdfView::paintLiveSegments(int from)
{
    QPainter p(&overlay_);
    p.setRenderHint(QPainter::Antialiasing);
    pagePainter(p, drawPage_);
    drawSegments(p, builder_.points(), builder_.widths(), from, builder_.stableCount(), PageColors::map(pen_.color));
    liveDrawn_ = builder_.stableCount();
}

QRectF PdfView::handleRect(int corner, const QRectF &r) const
{
    QPointF c = corner == 0 ? r.topLeft() : corner == 1 ? r.topRight() : corner == 2 ? r.bottomRight() : r.bottomLeft();
    return QRectF(c.x() - 6, c.y() - 6, 12, 12);
}

void PdfView::paintEvent(QPaintEvent *e)
{
    QPainter p(viewport());
    const QRect ev = e->rect();
    p.fillRect(ev, PageColors::backdrop());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal s = ppp();
    const QPointF off = offset();
    const bool dark = PageColors::theme() == PageTheme::Dark;
    for (int i = 0; i < pageRects_.size(); ++i) {
        QRectF r = pageRects_[i].translated(-off);
        if (r.bottom() < ev.top()) continue;
        if (r.top() > ev.bottom()) break;
        p.fillRect(r, PageColors::paper());
        auto t = tiles_.constFind(i);
        if (t != tiles_.constEnd()) {
            QRectF target(r.topLeft() + QPointF(t->clip.topLeft()) / t->scale * s, QSizeF(t->clip.size()) / t->scale * s);
            p.drawImage(target, t->img);
        }
        p.save();
        p.setClipRect(r.intersected(ev));
        p.translate(r.topLeft());
        p.scale(s, s);
        p.setPen(Qt::NoPen);
        auto h = hits_.constFind(i);
        if (h != hits_.constEnd())
            for (int k = 0; k < h->size(); ++k) {
                bool cur = i == hitPage_ && k == hitIndex_;
                p.setBrush(cur ? QColor(255, 120, 0, 130) : QColor(255, 210, 0, 90));
                p.drawPolygon(h->at(k));
            }
        // fillable form fields get a light tint, like "highlight fields" in Acrobat
        for (const FieldInfo &f : fieldsFor(i))
            if (!f.readonly && f.kind != FieldKind::Button && f.kind != FieldKind::Signature && f.kind != FieldKind::Other &&
                !(fieldEdit_ && i == fieldPage_ && f.id == fieldId_))
                p.fillRect(f.rect, QColor(40, 120, 230, 30));
        if (i == selPage_) {
            p.setBrush(QColor(0, 110, 230, 80));
            for (const QPolygonF &q : std::as_const(selQuads_)) p.drawPolygon(q);
        }
        p.setRenderHint(QPainter::Antialiasing);
        for (const Pending &pd : std::as_const(pending_))
            if (pd.page == i && pd.d.highlighter) {
                if (dark) {     // multiply would vanish on dark paper
                    p.setOpacity(0.45);
                    paintInk(p, pd.d.strokes.value(0), pd.d.color, pd.d.width, false);
                    p.setOpacity(1);
                } else
                    paintInk(p, pd.d);
            }
        if (drag_ == Drag::Draw && drawPage_ == i) {
            InkStroke st{builder_.points(), builder_.widths()};
            if (drawHighlighter_) {
                st.w.clear();
                if (dark) p.setOpacity(0.45);
                paintInk(p, st, hl_.color, builder_.baseWidth(), !dark);
                p.setOpacity(1);
            }
        }
        p.restore();
    }
    if (!pending_.isEmpty() || drag_ == Drag::Draw) p.drawImage(QPoint(0, 0), overlay_);
    if (drag_ == Drag::Draw && !drawHighlighter_ && drawPage_ >= 0) {   // provisional tail up to the pen tip
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        pagePainter(p, drawPage_);
        drawSegments(p, builder_.points(), builder_.widths(), qMax(1, builder_.stableCount()), builder_.points().size(), PageColors::map(pen_.color));
        p.restore();
    }

    if (noteLinksVisible_ && !noteLinks_.isEmpty()) {      // fine lines from markings to their notes
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        for (const NoteLink &l : std::as_const(noteLinks_)) {
            if (l.page < 0 || l.page >= pageRects_.size()) continue;
            QPointF a = pageToView(l.page, l.anchor);
            if (a.y() < ev.top() - 2 || a.y() > ev.bottom() + 2) continue;
            QColor c = l.color.darker(135);
            p.setPen(QPen(c, 1.2));
            p.drawLine(QPointF(0, a.y()), QPointF(a.x() - 1, a.y()));
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawEllipse(a, 2.5, 2.5);
        }
        p.restore();
    }
    if (annPage_ >= 0 && annPage_ < pageRects_.size()) {
        QRectF vr = toView(annPage_, (drag_ == Drag::AnnotMove || drag_ == Drag::AnnotResize) ? annDragRect_ : ann_.rect);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 120, 215), 1, Qt::DashLine));
        p.drawRect(vr);
        p.setPen(QPen(QColor(0, 120, 215), 1));
        p.setBrush(Qt::white);
        for (int c = 0; c < 4; ++c) p.drawRect(handleRect(c, vr).adjusted(2, 2, -2, -2));
        if (drag_ == Drag::AnnotResize && ann_.kind == AnnotKind::FreeText) {      // live preview of the new text size
            qreal k = annDragRect_.width() / qMax(1.0, ann_.rect.width());
            QFont f("Helvetica");
            f.setStyleHint(QFont::SansSerif);
            f.setPixelSize(qMax(4, int(std::round((ann_.fontSize > 0 ? ann_.fontSize : 12) * k * s))));
            p.setFont(f);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(PageColors::map(ann_.color.isValid() ? ann_.color : QColor(Qt::black)));
            p.fillRect(vr, QColor(PageColors::paper().red(), PageColors::paper().green(), PageColors::paper().blue(), 215));
            p.drawText(vr.adjusted(3, 1, 400, 400), Qt::AlignLeft | Qt::AlignTop, ann_.text);
        }
    }
    if (placing_) {
        int pg = pageAt(placePos_);
        if (pg >= 0) {
            QPointF c = toPage(pg, placePos_);
            QRectF r(c.x() - placeSize_.width() / 2, c.y() - placeSize_.height() / 2, placeSize_.width(), placeSize_.height());
            QRectF vr = toView(pg, r);
            p.setOpacity(0.75);
            if (!placeImage_.isNull()) p.drawImage(vr, placeImage_);
            else {
                p.save();
                p.setRenderHint(QPainter::Antialiasing);
                QRectF b = placeInk_.bounds();
                p.translate(vr.topLeft());
                p.scale(vr.width() / b.width(), vr.height() / b.height());
                p.translate(-b.topLeft());
                paintInk(p, placeInk_);
                p.restore();
            }
            p.setOpacity(1);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 120, 215), 1, Qt::DashLine));
            p.drawRect(vr);
        }
    }
    if (drag_ == Drag::RectPick && rectPage_ >= 0) {
        p.setPen(QPen(QColor(0, 120, 215), 1.5));
        p.setBrush(QColor(0, 120, 215, 40));
        p.drawRect(toView(rectPage_, rectPick_.normalized()));
    }
    p.end();
    requestTiles();
}

// ------------------------------------------------------------------ input plumbing
bool PdfView::viewportEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease: {
        auto *te = static_cast<QTabletEvent *>(e);
        Ptr p;
        p.pos = te->position();
        p.pressure = float(te->pressure());
        p.eraser = te->pointerType() == QPointingDevice::PointerType::Eraser || (te->buttons() & Qt::RightButton);
        p.t = clock_.elapsed();
        p.pen = true;
        s_lastPenMs = s_penClock.elapsed();
        if (e->type() == QEvent::TabletPress) {
            penDown_ = true;
            flingTimer_.stop();
            touchCount_ = 0;
            setFocus();
            press(p);
        } else if (e->type() == QEvent::TabletMove) {
            if (penDown_) move(p);
            else hover(p.pos, true);
        } else {
            if (penDown_) release(p);
            penDown_ = false;
        }
        te->accept();
        return true;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        handleTouch(static_cast<QTouchEvent *>(e));     // fingers only move the page, they never draw
        e->accept();
        return true;
    case QEvent::NativeGesture: {
        auto *ng = static_cast<QNativeGestureEvent *>(e);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(zoom_ * (1.0 + ng->value()), ng->position());
            return true;
        }
        break;
    }
    default: break;
    }
    return QAbstractScrollArea::viewportEvent(e);
}

// Finger input: one finger pans (with a short fling), two fingers zoom. Ignored while a pen is
// on or just above the screen, so a resting hand does nothing.
void PdfView::handleTouch(QTouchEvent *te)
{
    if (te->type() == QEvent::TouchCancel || penDown_ || s_penClock.elapsed() - s_lastPenMs < 400) {
        touchCount_ = 0;
        return;
    }
    QVector<QPointF> pts;
    for (const QEventPoint &p : te->points())
        if (p.state() != QEventPoint::Released) pts.push_back(p.position());
    if (te->type() == QEvent::TouchBegin) {
        flingTimer_.stop();
        hidePreview();
        touchCount_ = 0;
    }
    if (pts.size() == 1) {
        if (touchCount_ != 1) {
            touchCount_ = 1;
            touchTrail_.clear();
        } else
            scrollBy(pts[0] - touchLast_);
        touchLast_ = pts[0];
        touchTrail_.push_back({clock_.elapsed(), pts[0]});
        while (touchTrail_.size() > 8) touchTrail_.removeFirst();
    } else if (pts.size() >= 2) {
        QPointF mid = (pts[0] + pts[1]) / 2;
        qreal dist = QLineF(pts[0], pts[1]).length();
        if (touchCount_ == 2) {
            if (pinchDist_ > 1 && dist > 1) setZoom(zoom_ * dist / pinchDist_, mid);
            scrollBy(mid - touchLast_);
        }
        touchCount_ = 2;
        pinchDist_ = dist;
        touchLast_ = mid;
    }
    if (te->type() == QEvent::TouchEnd) {
        if (touchCount_ == 1 && touchTrail_.size() >= 2) {
            qint64 now = clock_.elapsed();
            int first = touchTrail_.size() - 1;
            while (first > 0 && now - touchTrail_[first - 1].first < 90) --first;
            qint64 dt = touchTrail_.last().first - touchTrail_[first].first;
            if (dt > 0 && now - touchTrail_.last().first < 60) {
                flingV_ = (touchTrail_.last().second - touchTrail_[first].second) / qreal(dt);
                if (std::hypot(flingV_.x(), flingV_.y()) > 0.3) flingTimer_.start();
            }
        }
        touchCount_ = 0;
    }
}

void PdfView::scrollBy(QPointF d)
{
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - int(std::round(d.x())));
    verticalScrollBar()->setValue(verticalScrollBar()->value() - int(std::round(d.y())));
}

static bool fromTouch(const QMouseEvent *e) { return e->deviceType() == QInputDevice::DeviceType::TouchScreen; }

void PdfView::mousePressEvent(QMouseEvent *e)
{
    if (penDown_ || fromTouch(e)) return;
    if (e->button() == Qt::BackButton) { goBack(); return; }
    if (e->button() == Qt::ForwardButton) { goForward(); return; }
    flingTimer_.stop();
    press(Ptr{e->position(), -1, false, e->button(), clock_.elapsed()});
}
void PdfView::mouseMoveEvent(QMouseEvent *e)
{
    if (penDown_ || fromTouch(e)) return;
    if (e->buttons() == Qt::NoButton) hover(e->position());
    else move(Ptr{e->position(), -1, false, Qt::LeftButton, clock_.elapsed()});
}
void PdfView::mouseReleaseEvent(QMouseEvent *e)
{
    if (penDown_ || fromTouch(e)) return;
    release(Ptr{e->position(), -1, false, e->button(), clock_.elapsed()});
}

void PdfView::mouseDoubleClickEvent(QMouseEvent *e)
{
    int pg = pageAt(e->position());
    AnnotInfo a;
    if (tool_ == Tool::Select && pg >= 0 && annotAt(pg, toPage(pg, e->position()), &a) && a.kind == AnnotKind::FreeText) {
        clearAnnotSelection();
        beginTextEdit(pg, toPage(pg, e->position()), &a);
    }
}

void PdfView::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        setZoom(zoom_ * std::pow(1.0015, e->angleDelta().y()), e->position());
        e->accept();
        return;
    }
    // own wheel handling so the step is adjustable (Preferences > scroll speed)
    QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 120.0 * 144.0 : QPointF(e->pixelDelta());
    if ((e->modifiers() & Qt::ShiftModifier) && qFuzzyIsNull(d.x())) d = QPointF(d.y(), 0);
    flingTimer_.stop();
    scrollBy(d * s_scrollSpeed);
    e->accept();
}

void PdfView::leaveEvent(QEvent *e)
{
    previewTimer_.stop();
    hasHoverLink_ = false;      // an open preview stays while the pointer moves into it (pollPreview)
    if (preview_) preview_->hide();
    QAbstractScrollArea::leaveEvent(e);
}

void PdfView::keyPressEvent(QKeyEvent *e)
{
    QScrollBar *v = verticalScrollBar();
    switch (e->key()) {
    case Qt::Key_Escape: cancelModes(); return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace: deleteSelectedAnnot(); return;
    case Qt::Key_PageDown:
    case Qt::Key_Space: v->setValue(v->value() + (e->modifiers() & Qt::ShiftModifier ? -1 : 1) * int(viewport()->height() * 0.9)); return;
    case Qt::Key_PageUp: v->setValue(v->value() - int(viewport()->height() * 0.9)); return;
    case Qt::Key_Home: v->setValue(0); return;
    case Qt::Key_End: v->setValue(v->maximum()); return;
    case Qt::Key_Down: scrollBy(QPointF(0, -60 * s_scrollSpeed)); return;
    case Qt::Key_Up: scrollBy(QPointF(0, 60 * s_scrollSpeed)); return;
    case Qt::Key_Left:
    case Qt::Key_Right:     // turn the page; a page wider than the view is moved sideways instead, as before
        if (horizontalScrollBar()->maximum() > 0 || (e->modifiers() & ~Qt::KeypadModifier)) break;
        gotoPage(qBound(0, currentPage() + (e->key() == Qt::Key_Right ? 1 : -1), doc_->pageCount() - 1), 0, false);
        return;
    default: break;
    }
    if (e->matches(QKeySequence::Copy)) { copySelection(); return; }
    QAbstractScrollArea::keyPressEvent(e);
}

void PdfView::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu m(this);
    int pg = pageAt(e->pos());
    if (hasTextSelection()) {
        m.addAction(tr("Copy"), this, &PdfView::copySelection);
        m.addAction(tr("Highlight"), this, [this] { markupSelection(AnnotKind::Highlight, QColor(255, 235, 60)); });
        m.addAction(tr("Highlight (green)"), this, [this] { markupSelection(AnnotKind::Highlight, QColor(140, 235, 130)); });
        m.addAction(tr("Underline"), this, [this] { markupSelection(AnnotKind::Underline, QColor(220, 40, 40)); });
        m.addAction(tr("Strike out"), this, [this] { markupSelection(AnnotKind::StrikeOut, QColor(220, 40, 40)); });
    }
    if (pg >= 0) {
        QPointF pt = toPage(pg, e->pos());
        flushInk();
        const QVector<AnnotInfo> as = doc_->annots(pg);
        for (int i = as.size() - 1; i >= 0; --i)
            if (as[i].rect.contains(pt)) {
                int id = as[i].id;
                m.addAction(tr("Delete annotation"), this, [this, pg, id] {
                    clearAnnotSelection();
                    try { doc_->removeAnnots(pg, {id}); } catch (const PdfError &err) { emit statusMessage(err.message()); }
                });
                break;
            }
    }
    if (!m.isEmpty()) m.exec(e->globalPos());
}

// ------------------------------------------------------------------ tools
void PdfView::updateCursor(Tool t)
{
    viewport()->setCursor(t == Tool::Hand ? Qt::OpenHandCursor : t == Tool::Select ? Qt::IBeamCursor : Qt::CrossCursor);
}

void PdfView::setTools(Tool pen, Tool mouse)
{
    toolPen_ = pen;
    toolMouse_ = mouse;
    Tool now = lastWasPen_ ? pen : mouse;
    if (now == tool_) return;
    commitTextEdit();       // only when the tool of the device in use really changes
    tool_ = now;
    cancelModes();
    hideSelBar();
    if (now != Tool::Select) clearAnnotSelection();
    updateCursor(now);
}

void PdfView::cancelModes()
{
    placing_ = false;
    rectCb_ = nullptr;
    rectPage_ = -1;
    hidePreview();
    viewport()->update();
}

void PdfView::press(const Ptr &e)
{
    if (textEdit_) {            // a click outside the text box finishes it
        commitTextEdit();
        return;
    }
    commitFieldEdit();
    hidePreview();
    hideSelBar();
    previewTimer_.stop();
    // each device brings its own tool
    lastWasPen_ = e.pen;
    Tool want = e.pen ? toolPen_ : toolMouse_;
    if (want != tool_) {
        tool_ = want;
        if (tool_ != Tool::Select) clearAnnotSelection();
    }
    emit deviceUsed(e.pen);
    pressPos_ = lastPos_ = e.pos;
    int pg = pageAt(e.pos);
    // form fields react to the select and hand tools (and to the pen when it has one of them)
    if (e.button == Qt::LeftButton && pg >= 0 && !placing_ && !rectCb_ && !e.eraser &&
        (tool_ == Tool::Select || tool_ == Tool::Hand)) {
        if (const FieldInfo *f = fieldAt(pg, toPage(pg, e.pos))) {
            FieldInfo copy = *f;
            activateField(pg, copy);
            return;
        }
    }
    if (e.button == Qt::MiddleButton || (tool_ == Tool::Hand && !e.eraser && !placing_ && !rectCb_)) {
        drag_ = Drag::Pan;
        panScroll_ = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e.button != Qt::LeftButton || pg < 0) return;
    QPointF pt = toPage(pg, e.pos);

    if (rectCb_) {
        drag_ = Drag::RectPick;
        rectPage_ = pg;
        rectPick_ = QRectF(pt, pt);
        return;
    }
    if (placing_) {
        placing_ = false;
        QRectF r(pt.x() - placeSize_.width() / 2, pt.y() - placeSize_.height() / 2, placeSize_.width(), placeSize_.height());
        try {
            flushInk();
            int id;
            if (!placeImage_.isNull()) id = doc_->addStamp(pg, r, placeImage_);
            else {
                InkData d = placeInk_;
                QRectF b = d.bounds();
                qreal sc = r.width() / b.width();
                QTransform t;
                t.translate(r.left(), r.top());
                t.scale(sc, sc);
                t.translate(-b.left(), -b.top());
                d.transform(t, sc);
                id = doc_->addInk(pg, d);
            }
            emit toolChangeRequested(Tool::Select);
            for (const AnnotInfo &a : doc_->annots(pg))
                if (a.id == id) selectAnnot(pg, a);
        } catch (const PdfError &err) { emit statusMessage(err.message()); }
        viewport()->update();
        return;
    }

    Tool t = e.eraser ? Tool::Eraser : tool_;
    switch (t) {
    case Tool::Pen: beginStroke(e, false); break;
    case Tool::Highlighter: beginStroke(e, true); break;
    case Tool::Eraser:
        drag_ = Drag::Erase;
        eraseAt(pg, pt);
        break;
    case Tool::Text: {
        AnnotInfo a;
        bool existing = annotAt(pg, pt, &a) && a.kind == AnnotKind::FreeText;
        beginTextEdit(pg, pt, existing ? &a : nullptr);
        break;
    }
    case Tool::Select: {
        if (annPage_ >= 0) {
            QRectF vr = toView(annPage_, ann_.rect);
            for (int c = 0; c < 4; ++c)
                if (handleRect(c, vr).contains(e.pos)) {
                    drag_ = Drag::AnnotResize;
                    annCorner_ = c;
                    annDragRect_ = ann_.rect;
                    return;
                }
        }
        AnnotInfo a;
        if (annotAt(pg, pt, &a)) {
            selectAnnot(pg, a);
            drag_ = Drag::AnnotMove;
            annDragRect_ = a.rect;
            annGrab_ = pt;
            selPage_ = -1;
            selQuads_.clear();
            return;
        }
        clearAnnotSelection();
        if (const LinkInfo *l = linkAt(pg, pt)) {
            pressLink_ = *l;
            drag_ = Drag::LinkClick;
            return;
        }
        drag_ = Drag::TextSel;
        selPage_ = pg;
        selA_ = selB_ = pt;
        selQuads_.clear();
        pressMarkupPage_ = -1;
        pressMarkupId_ = 0;
        for (const AnnotInfo &a : doc_->annots(pg))
            for (const QPolygonF &q : a.quads)
                if (q.containsPoint(pt, Qt::OddEvenFill)) {
                    pressMarkupPage_ = pg;
                    pressMarkupId_ = a.id;
                }
        viewport()->update();
        break;
    }
    default: break;
    }
}

void PdfView::move(const Ptr &e)
{
    switch (drag_) {
    case Drag::Pan: {
        QPointF d = e.pos - pressPos_;
        horizontalScrollBar()->setValue(panScroll_.x() - int(d.x()));
        verticalScrollBar()->setValue(panScroll_.y() - int(d.y()));
        break;
    }
    case Drag::Draw: extendStroke(e); break;
    case Drag::Erase: {
        int pg = pageAt(e.pos);
        qreal len = QLineF(lastPos_, e.pos).length();
        int steps = qMax(1, int(len / 4));
        for (int i = 1; i <= steps && pg >= 0; ++i) {
            QPointF v = lastPos_ + (e.pos - lastPos_) * (qreal(i) / steps);
            if (pageAt(v) == pg) eraseAt(pg, toPage(pg, v));
        }
        break;
    }
    case Drag::LinkClick:
        if (QLineF(pressPos_, e.pos).length() < 5) break;
        drag_ = Drag::TextSel;       // dragging over a link selects its text instead
        selPage_ = pageAt(pressPos_);
        selA_ = toPage(selPage_, pressPos_);
        [[fallthrough]];
    case Drag::TextSel:
        if (selPage_ >= 0) {
            selB_ = toPage(selPage_, e.pos);
            selQuads_ = doc_->selectionQuads(selPage_, selA_, selB_);
            viewport()->update();
        }
        break;
    case Drag::AnnotMove: {
        QPointF pt = toPage(annPage_, e.pos);
        annDragRect_ = ann_.rect.translated(pt - annGrab_);
        viewport()->update();
        break;
    }
    case Drag::AnnotResize: {
        QPointF pt = toPage(annPage_, e.pos);
        const QRectF &r = ann_.rect;
        QPointF fixed = annCorner_ == 0 ? r.bottomRight() : annCorner_ == 1 ? r.bottomLeft() : annCorner_ == 2 ? r.topLeft() : r.topRight();
        qreal w = qMax(8.0, std::fabs(pt.x() - fixed.x())), h = qMax(8.0, std::fabs(pt.y() - fixed.y()));
        {       // proportional: ink and images scale, text boxes change their font size
            qreal k = qMax(w / r.width(), h / r.height());
            w = r.width() * k;
            h = r.height() * k;
        }
        bool left = annCorner_ == 0 || annCorner_ == 3, top = annCorner_ == 0 || annCorner_ == 1;
        annDragRect_ = QRectF(left ? fixed.x() - w : fixed.x(), top ? fixed.y() - h : fixed.y(), w, h);
        viewport()->update();
        break;
    }
    case Drag::RectPick:
        if (pageAt(e.pos) == rectPage_) rectPick_.setBottomRight(toPage(rectPage_, e.pos));
        viewport()->update();
        break;
    default: break;
    }
    lastPos_ = e.pos;
}

void PdfView::release(const Ptr &e)
{
    Drag d = drag_;
    switch (d) {
    case Drag::Draw: endStroke(); break;
    case Drag::Pan: viewport()->setCursor(tool_ == Tool::Hand ? Qt::OpenHandCursor : Qt::ArrowCursor); break;
    case Drag::LinkClick:
        if (pressLink_.external) {
            QUrl u(pressLink_.uri);
            if (u.scheme() == "http" || u.scheme() == "https" || u.scheme() == "mailto") QDesktopServices::openUrl(u);
        } else if (pressLink_.page >= 0)
            gotoPage(pressLink_.page, pressLink_.dest.y());
        break;
    case Drag::TextSel:
        if (selQuads_.isEmpty()) {
            selPage_ = -1;
            if (pressMarkupId_ && QLineF(pressPos_, e.pos).length() < 5 && !isPreview_) emit noteRequested(pressMarkupPage_, pressMarkupId_);
        } else
            showSelBar(e.pos);
        pressMarkupId_ = 0;
        break;
    case Drag::AnnotMove: applyAnnotDrag(false); break;
    case Drag::AnnotResize: applyAnnotDrag(true); break;
    case Drag::RectPick: {
        auto cb = rectCb_;
        rectCb_ = nullptr;
        QRectF r = rectPick_.normalized();
        int pg = rectPage_;
        rectPage_ = -1;
        drag_ = Drag::None;
        viewport()->update();
        if (cb) cb(pg, r);
        return;
    }
    default: break;
    }
    drag_ = Drag::None;
    viewport()->update();
}

void PdfView::hover(QPointF pos, bool pen)
{
    if (placing_) {
        placePos_ = pos;
        viewport()->update();
        return;
    }
    Tool ht = pen ? toolPen_ : toolMouse_;
    if ((ht == Tool::Select || ht == Tool::Hand) && !rectCb_) {
        int fp = pageAt(pos);
        if (const FieldInfo *f = fp >= 0 ? fieldAt(fp, toPage(fp, pos)) : nullptr) {
            viewport()->setCursor(f->kind == FieldKind::Text ? Qt::IBeamCursor : Qt::PointingHandCursor);
            if (!f->value.isEmpty() && (f->kind == FieldKind::List || f->kind == FieldKind::Combo)) viewport()->setToolTip(f->value);
            return;
        }
        viewport()->setToolTip(QString());
    }
    if (ht != Tool::Select || rectCb_) {
        updateCursor(rectCb_ ? Tool::Pen : ht);
        return;
    }
    int pg = pageAt(pos);
    Qt::CursorShape cur = pg >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor;
    const LinkInfo *l = pg >= 0 ? linkAt(pg, toPage(pg, pos)) : nullptr;
    if (annPage_ >= 0) {
        QRectF vr = toView(annPage_, ann_.rect);
        for (int c = 0; c < 4; ++c)
            if (handleRect(c, vr).contains(pos)) cur = (c % 2 == 0) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
        if (cur == Qt::IBeamCursor && vr.contains(pos)) cur = Qt::SizeAllCursor;
    }
    if (l && cur == Qt::IBeamCursor) cur = Qt::PointingHandCursor;
    viewport()->setCursor(cur);
    if (isPreview_) return;     // no previews inside a preview
    if (l) {
        if (!hasHoverLink_ || hoverLink_.rect != l->rect || hoverLink_.uri != l->uri) {
            hidePreview();
            hoverLink_ = *l;
            hasHoverLink_ = true;
            QRectF vr = toView(pg, l->rect);
            hoverGlobal_ = viewport()->mapToGlobal(QPoint(int(vr.left()), int(vr.bottom()) + 6));
            previewTimer_.start();
        }
    } else if (hasHoverLink_) {
        hasHoverLink_ = false;
        previewTimer_.stop();
        if (preview_) preview_->hide();
    }
}

// ------------------------------------------------------------------ inking
void PdfView::beginStroke(const Ptr &e, bool highlighter)
{
    drawPage_ = pageAt(e.pos);
    drawHighlighter_ = highlighter;
    drag_ = Drag::Draw;
    const PenStyle &st = highlighter ? hl_ : pen_;
    builder_.begin(toPage(drawPage_, e.pos), e.pressure, e.t, st.width, !highlighter, ppp());
    if (overlay_.isNull()) rebuildOverlay();
    liveDrawn_ = qMin(liveDrawn_, 0);
    if (pending_.isEmpty()) rebuildOverlay();
    holdAnchor_ = e.pos;
    holdTimer_.start();
    lastDirty_ = QRectF();
    viewport()->update(QRectF(e.pos.x() - 20, e.pos.y() - 20, 40, 40).toAlignedRect());
}

void PdfView::extendStroke(const Ptr &e)
{
    if (QLineF(holdAnchor_, e.pos).length() > 4) {     // still moving: restart the hold-to-shape timer
        holdAnchor_ = e.pos;
        holdTimer_.start();
    }
    QPointF pt = toPage(drawPage_, e.pos);
    QSizeF ps = doc_->pageSize(drawPage_);
    pt = QPointF(qBound(0.0, pt.x(), ps.width()), qBound(0.0, pt.y(), ps.height()));
    int oldCount = builder_.points().size();
    int first = builder_.add(pt, e.pressure, e.t);
    const QVector<QPointF> &pts = builder_.points();
    bool shape = builder_.shape() != StrokeBuilder::Shape::None;
    if (!drawHighlighter_) {
        if (shape || first < liveDrawn_ - 1) rebuildOverlay();
        else paintLiveSegments(liveDrawn_);
    }
    // repaint only what changed: from the first modified point to the pen tip (old and new)
    int from = shape || drawHighlighter_ ? 0 : qMax(0, qMin(first, oldCount - 2) - 1);
    QRectF dirty(pts[from], QSizeF(0, 0));
    for (int i = from + 1; i < pts.size(); ++i) {
        dirty.setLeft(qMin(dirty.left(), pts[i].x()));
        dirty.setRight(qMax(dirty.right(), pts[i].x()));
        dirty.setTop(qMin(dirty.top(), pts[i].y()));
        dirty.setBottom(qMax(dirty.bottom(), pts[i].y()));
    }
    qreal m = builder_.baseWidth() * 1.8 + 3 / ppp();
    QRectF vd = toView(drawPage_, dirty.adjusted(-m, -m, m, m));
    viewport()->update(vd.united(lastDirty_).toAlignedRect().adjusted(-2, -2, 2, 2));
    lastDirty_ = vd;
}

void PdfView::endStroke()
{
    holdTimer_.stop();
    InkData d;
    d.strokes.push_back(builder_.result());
    d.color = drawHighlighter_ ? hl_.color : pen_.color;
    d.width = builder_.baseWidth();
    d.highlighter = drawHighlighter_;
    if (builder_.shape() != StrokeBuilder::Shape::None && !builder_.widths().isEmpty()) d.width = builder_.widths().first();
    drag_ = Drag::None;
    pending_.push_back(Pending{drawPage_, d, 0, false});
    rebuildOverlay();
    tryCommit(false);
    viewport()->update();
}

void PdfView::tryCommit(bool block)
{
    bool left = false;
    for (int i = 0; i < pending_.size(); ++i) {
        Pending &pd = pending_[i];
        if (pd.committed) continue;
        if (!block && !doc_->tryLock()) { left = true; break; }
        try {
            doc_->addInk(pd.page, pd.d);
            pd.committed = true;
            pd.gen = doc_->generation();
        } catch (const PdfError &err) {
            emit statusMessage(err.message());
            pending_.removeAt(i--);
        }
        if (!block) doc_->unlock();
    }
    if (left) { if (!commitTimer_.isActive()) commitTimer_.start(); }
    else commitTimer_.stop();
}

void PdfView::flushInk()
{
    commitTextEdit();
    commitFieldEdit();
    if (drag_ == Drag::Draw) endStroke();
    tryCommit(true);
}

void PdfView::onHold()
{
    if (drag_ != Drag::Draw) return;
    if (builder_.snapToShape()) {
        rebuildOverlay();
        viewport()->update();
    }
}

void PdfView::eraseAt(int page, QPointF pt)
{
    if (erasePage_ != page || eraseGen_ != doc_->generation()) {
        flushInk();
        eraseCache_ = doc_->inkAnnots(page);
        erasePage_ = page;
        eraseGen_ = doc_->generation();
    }
    QVector<int> ids;
    qreal radius = 6 / ppp();
    for (const InkAnnot &a : std::as_const(eraseCache_))
        if (a.data.bounds().adjusted(-radius, -radius, radius, radius).contains(pt) && inkHit(a.data, pt, radius)) ids.push_back(a.id);
    if (ids.isEmpty()) return;
    try {
        doc_->removeAnnots(page, ids);
    } catch (const PdfError &err) { emit statusMessage(err.message()); }
    for (int i = pending_.size() - 1; i >= 0; --i)
        if (pending_[i].page == page && inkHit(pending_[i].d, pt, radius)) pending_.removeAt(i);
    rebuildOverlay();
}

// ------------------------------------------------------------------ links
const QVector<LinkInfo> &PdfView::linksFor(int page)
{
    auto it = linkCache_.find(page);
    if (it == linkCache_.end()) it = linkCache_.insert(page, doc_->links(page));
    return it.value();
}

const LinkInfo *PdfView::linkAt(int page, QPointF pt)
{
    for (const LinkInfo &l : linksFor(page))
        if (l.rect.contains(pt)) return &l;
    return nullptr;
}

void PdfView::showPreview()
{
    if (!hasHoverLink_ || drag_ != Drag::None || isPreview_) return;
    QPoint pos = hoverGlobal_;
    QScreen *sc = QGuiApplication::screenAt(pos);
    QRect g = sc ? sc->availableGeometry() : QRect(pos, QSize(4000, 4000));
    auto place = [&](QWidget *w) {
        if (pos.y() + w->height() > g.bottom()) pos.setY(pos.y() - w->height() - int(hoverLink_.rect.height() * ppp()) - 12);
        pos.setX(qBound(g.left(), pos.x(), qMax(g.left(), g.right() - w->width())));
        w->move(pos);
    };
    if (hoverLink_.external || hoverLink_.page < 0 || hoverLink_.page >= doc_->pageCount()) {
        if (!preview_) {
            preview_ = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
            preview_->setAttribute(Qt::WA_ShowWithoutActivating);
            preview_->setStyleSheet("QLabel{background:white;color:#202020;border:1px solid #707070;padding:3px;}");
        }
        preview_->setText(hoverLink_.uri.left(160));
        preview_->adjustSize();
        place(preview_);
        preview_->show();
        return;
    }
    // Zotero-style peek: a small live view of the document at the link target. The pointer can
    // move into it to scroll, select, copy and highlight; it closes when the pointer leaves.
    if (!previewFrame_) {
        previewFrame_ = new QFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        previewFrame_->setObjectName("linkPreview");
        previewFrame_->setAttribute(Qt::WA_ShowWithoutActivating);
        previewFrame_->setStyleSheet("#linkPreview{border:1px solid #6b7280;}");
        auto *lay = new QVBoxLayout(previewFrame_);
        lay->setContentsMargins(1, 1, 1, 1);
        previewView_ = new PdfView(doc_, previewFrame_);
        previewView_->isPreview_ = true;
        previewView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        lay->addWidget(previewView_);
        previewFrame_->resize(660, 320);
    }
    place(previewFrame_);
    previewFrame_->show();
    previewView_->setHighlighterStyle(hl_);
    int page = hoverLink_.page;
    double y = qMax(0.0, hoverLink_.dest.y() - 20);
    QSizeF ps = doc_->pageSize(page);
    int sb = previewView_->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    previewView_->setZoom((previewFrame_->width() - 2 - sb - 2 * kMargin) / (ps.width() * 96.0 / 72.0));
    QTimer::singleShot(0, previewView_, [v = previewView_, page, y] { v->gotoPage(page, y, false); });
    previewMiss_ = 0;
    previewPoll_.start();
}

void PdfView::pollPreview()
{
    if (!previewFrame_ || !previewFrame_->isVisible()) {
        previewPoll_.stop();
        return;
    }
    bool inside = previewFrame_->geometry().adjusted(-10, -10, 10, 10).contains(QCursor::pos());
    bool busy = QApplication::activePopupWidget() != nullptr || QGuiApplication::mouseButtons() != Qt::NoButton;
    if (inside || busy || hasHoverLink_) previewMiss_ = 0;
    else if (++previewMiss_ >= 2) hidePreview();
}

void PdfView::hidePreview()
{
    if (preview_) preview_->hide();
    if (previewFrame_) previewFrame_->hide();
    previewPoll_.stop();
}

// ------------------------------------------------------------------ annotation selection
bool PdfView::annotAt(int page, QPointF pt, AnnotInfo *out)
{
    flushInk();
    const QVector<AnnotInfo> as = doc_->annots(page);
    QVector<InkAnnot> inks;
    bool haveInks = false;
    for (int i = as.size() - 1; i >= 0; --i) {
        const AnnotInfo &a = as[i];
        if (!a.rect.contains(pt)) continue;
        if (a.kind == AnnotKind::Ink) {
            if (!haveInks) { inks = doc_->inkAnnots(page); haveInks = true; }
            bool hit = false;
            for (const InkAnnot &ia : std::as_const(inks))
                if (ia.id == a.id) hit = inkHit(ia.data, pt, 5 / ppp());
            if (!hit) continue;
        } else if (a.kind != AnnotKind::FreeText && a.kind != AnnotKind::Stamp)
            continue;
        *out = a;
        return true;
    }
    return false;
}

void PdfView::selectAnnot(int page, const AnnotInfo &a)
{
    annPage_ = page;
    ann_ = a;
    viewport()->update();
}

void PdfView::clearAnnotSelection()
{
    if (annPage_ < 0) return;
    annPage_ = -1;
    viewport()->update();
}

void PdfView::applyAnnotDrag(bool resized)
{
    if (annPage_ < 0 || annDragRect_ == ann_.rect) return;
    try {
        if (ann_.kind == AnnotKind::Ink) {
            for (const InkAnnot &ia : doc_->inkAnnots(annPage_)) {
                if (ia.id != ann_.id) continue;
                InkData d = ia.data;
                qreal sc = annDragRect_.width() / ann_.rect.width();
                QTransform t;
                t.translate(annDragRect_.left(), annDragRect_.top());
                t.scale(sc, sc);
                t.translate(-ann_.rect.left(), -ann_.rect.top());
                d.transform(t, sc);
                doc_->setInk(annPage_, ann_.id, d);
            }
        } else if (ann_.kind == AnnotKind::FreeText && resized) {
            float size = (ann_.fontSize > 0 ? ann_.fontSize : 12) * float(annDragRect_.width() / qMax(1.0, ann_.rect.width()));
            doc_->scaleFreeText(annPage_, ann_.id, annDragRect_, qBound(4.f, size, 200.f));
        } else
            doc_->setAnnotRect(annPage_, ann_.id, annDragRect_);
        for (const AnnotInfo &a : doc_->annots(annPage_))
            if (a.id == ann_.id) ann_ = a;
    } catch (const PdfError &err) { emit statusMessage(err.message()); }
}

void PdfView::deleteSelectedAnnot()
{
    if (annPage_ < 0) return;
    int pg = annPage_, id = ann_.id;
    clearAnnotSelection();
    try { doc_->removeAnnots(pg, {id}); } catch (const PdfError &err) { emit statusMessage(err.message()); }
}

// In-place text box: typing happens on the page itself; the box grows with the text.
void PdfView::beginTextEdit(int page, QPointF pt, const AnnotInfo *existing)
{
    commitTextEdit();
    textPage_ = page;
    textId_ = existing ? existing->id : 0;
    textEditSize_ = existing && existing->fontSize > 0 ? existing->fontSize : textSize_;
    textEditColor_ = existing && existing->color.isValid() ? existing->color : pen_.color;
    auto *ed = new QPlainTextEdit(viewport());
    ed->setFrameShape(QFrame::NoFrame);
    ed->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ed->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ed->setLineWrapMode(QPlainTextEdit::NoWrap);
    ed->document()->setDocumentMargin(2);
    QFont f("Helvetica");
    f.setStyleHint(QFont::SansSerif);
    f.setPixelSize(qMax(5, int(std::round(textEditSize_ * ppp()))));
    ed->setFont(f);
    // Offset between the editor's corner and the PDF text box, so the committed text lands
    // exactly where it was typed (the PDF places the first baseline ~0.72 em below the box top).
    textDelta_ = QPointF(3 / ppp(), (3 + QFontMetricsF(f).ascent()) / ppp() - 0.72 * textEditSize_);
    textAt_ = existing ? existing->rect.topLeft() - textDelta_ : QPointF(pt.x() - 3 / ppp(), pt.y() - textEditSize_ * 0.5) - textDelta_;
    ed->setStyleSheet(QString("QPlainTextEdit{background:%1;color:%2;border:1px dashed #0a78d7;border-radius:0;padding:0;}")
                          .arg(existing ? "white" : "rgba(255,255,255,215)", textEditColor_.name()));
    if (existing) ed->setPlainText(existing->text);
    ed->installEventFilter(this);
    connect(ed, &QPlainTextEdit::textChanged, this, &PdfView::layoutTextEdit);
    textEdit_ = ed;
    layoutTextEdit();
    ed->show();
    ed->setFocus();
    ed->moveCursor(QTextCursor::End);
    emit statusMessage(tr("Type the text. Click outside the box to finish, Esc to cancel."));
}

void PdfView::layoutTextEdit()
{
    if (!textEdit_ || textPage_ >= pageRects_.size()) return;
    QFontMetricsF fm(textEdit_->font());
    qreal w = fm.horizontalAdvance(QStringLiteral("MMM"));
    const QStringList lines = textEdit_->toPlainText().split('\n');
    for (const QString &l : lines) w = qMax(w, fm.horizontalAdvance(l));
    QPointF tl = pageViewRect(textPage_).topLeft() + textAt_ * ppp();
    textEdit_->setGeometry(int(tl.x()), int(tl.y()), int(w + fm.averageCharWidth() * 2 + 10), int(lines.size() * fm.lineSpacing() + 10));
}

void PdfView::cancelTextEdit()
{
    if (!textEdit_) return;
    QPlainTextEdit *ed = textEdit_;
    textEdit_ = nullptr;
    ed->removeEventFilter(this);
    ed->hide();
    ed->deleteLater();
    setFocus();
}

void PdfView::commitTextEdit()
{
    if (!textEdit_) return;
    QPlainTextEdit *ed = textEdit_;
    textEdit_ = nullptr;        // first: committing can re-enter through focus changes
    ed->removeEventFilter(this);
    QString text = ed->toPlainText();
    while (text.endsWith('\n') || text.endsWith(' ')) text.chop(1);
    QRectF r(textAt_ + textDelta_, QSizeF(ed->width() / ppp() + 4, ed->height() / ppp() + 2));
    ed->hide();
    ed->deleteLater();
    try {
        flushInk();
        if (textId_) {
            if (text.isEmpty()) doc_->removeAnnots(textPage_, {textId_});
            else doc_->setFreeText(textPage_, textId_, text, r);
        } else if (!text.isEmpty())
            doc_->addFreeText(textPage_, r, text, textEditSize_, textEditColor_);
    } catch (const PdfError &err) { emit statusMessage(err.message()); }
    viewport()->update();
}

// ------------------------------------------------------------------ form fields
const QVector<FieldInfo> &PdfView::fieldsFor(int page)
{
    auto it = fieldCache_.find(page);
    if (it == fieldCache_.end()) it = fieldCache_.insert(page, doc_->fields(page));
    return it.value();
}

const FieldInfo *PdfView::fieldAt(int page, QPointF pt)
{
    for (const FieldInfo &f : fieldsFor(page))
        if (f.rect.contains(pt) && !f.readonly && f.kind != FieldKind::Button && f.kind != FieldKind::Signature && f.kind != FieldKind::Other)
            return &f;
    return nullptr;
}

void PdfView::activateField(int page, const FieldInfo &f)
{
    try {
        switch (f.kind) {
        case FieldKind::Text: beginFieldEdit(page, f); break;
        case FieldKind::CheckBox:
        case FieldKind::Radio:
            flushInk();
            doc_->toggleField(page, f.id);
            break;
        case FieldKind::Combo:
        case FieldKind::List: {
            QMenu m(this);
            for (const QString &o : f.options) {
                QAction *a = m.addAction(o);
                a->setCheckable(true);
                a->setChecked(o == f.value);
            }
            QRectF vr = toView(page, f.rect);
            QAction *chosen = m.exec(viewport()->mapToGlobal(QPoint(int(vr.left()), int(vr.bottom()) + 2)));
            if (chosen && chosen->text() != f.value) {
                flushInk();
                doc_->setFieldChoice(page, f.id, chosen->text());
            }
            break;
        }
        default: break;
        }
    } catch (const PdfError &err) { emit statusMessage(err.message()); }
    viewport()->update();
}

void PdfView::beginFieldEdit(int page, const FieldInfo &f)
{
    cancelFieldEdit();
    fieldPage_ = page;
    fieldId_ = f.id;
    fieldRect_ = f.rect;
    fieldOld_ = f.value;
    float size = f.fontSize > 0 ? f.fontSize : float(qBound(6.0, f.rect.height() * 0.62, 14.0));
    QFont font("Helvetica");
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(qMax(5, int(std::round(size * ppp()))));
    const QString css = QString("border:2px solid %1;border-radius:2px;background:white;color:black;padding:0 1px;").arg(Theme::accent().name());
    if (f.multiline) {
        auto *ed = new QPlainTextEdit(viewport());
        ed->setPlainText(f.value);
        ed->setStyleSheet("QPlainTextEdit{" + css + "}");
        ed->document()->setDocumentMargin(1);
        ed->moveCursor(QTextCursor::End);
        fieldEdit_ = ed;
    } else {
        auto *ed = new QLineEdit(viewport());
        ed->setText(f.value);
        if (f.maxLen > 0) ed->setMaxLength(f.maxLen);
        ed->setStyleSheet("QLineEdit{" + css + "}");
        ed->selectAll();
        fieldEdit_ = ed;
    }
    fieldEdit_->setFont(font);
    fieldEdit_->installEventFilter(this);
    layoutFieldEdit();
    fieldEdit_->show();
    fieldEdit_->setFocus();
    viewport()->update();
}

void PdfView::layoutFieldEdit()
{
    if (!fieldEdit_ || fieldPage_ < 0 || fieldPage_ >= pageRects_.size()) return;
    fieldEdit_->setGeometry(toView(fieldPage_, fieldRect_).toAlignedRect().adjusted(-2, -2, 2, 2));
}

void PdfView::cancelFieldEdit()
{
    if (!fieldEdit_) return;
    QWidget *ed = fieldEdit_;
    fieldEdit_ = nullptr;
    ed->removeEventFilter(this);
    ed->hide();
    ed->deleteLater();
    viewport()->update();
}

void PdfView::commitFieldEdit()
{
    if (!fieldEdit_) return;
    QWidget *ed = fieldEdit_;
    QString text = qobject_cast<QLineEdit *>(ed) ? static_cast<QLineEdit *>(ed)->text() : static_cast<QPlainTextEdit *>(ed)->toPlainText();
    int page = fieldPage_, id = fieldId_;
    cancelFieldEdit();
    setFocus();
    if (text == fieldOld_) return;
    try {
        flushInk();
        doc_->setFieldText(page, id, text);
    } catch (const PdfError &err) { emit statusMessage(err.message()); }
}

// Tab / Shift+Tab: next text field in reading order (page, then top to bottom, left to right)
void PdfView::focusNextField(int step)
{
    int page = fieldPage_;
    QRectF from = fieldRect_;
    commitFieldEdit();
    struct Cand { int page; FieldInfo f; };
    QVector<Cand> all;
    for (int p = 0; p < pageRects_.size(); ++p)
        for (const FieldInfo &f : fieldsFor(p))
            if (f.kind == FieldKind::Text && !f.readonly) all.push_back({p, f});
    if (all.isEmpty()) return;
    auto before = [](const Cand &a, int p, const QRectF &r) {
        if (a.page != p) return a.page < p;
        if (std::fabs(a.f.rect.top() - r.top()) > 4) return a.f.rect.top() < r.top();
        return a.f.rect.left() < r.left() - 1;
    };
    std::sort(all.begin(), all.end(), [&](const Cand &a, const Cand &b) { return before(a, b.page, b.f.rect); });
    int cur = -1;
    for (int i = 0; i < all.size(); ++i)
        if (all[i].page == page && all[i].f.rect == from) cur = i;
    int next = cur < 0 ? 0 : (cur + step + all.size()) % all.size();
    const Cand &c = all[next];
    QRectF vr = toView(c.page, c.f.rect);
    if (vr.top() < 0 || vr.bottom() > viewport()->height()) gotoPage(c.page, qMax(0.0, c.f.rect.top() - 60), false);
    beginFieldEdit(c.page, c.f);
}

bool PdfView::eventFilter(QObject *o, QEvent *e)
{
    if (o == fieldEdit_ && fieldEdit_) {
        if (e->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (ke->key() == Qt::Key_Escape) { cancelFieldEdit(); setFocus(); return true; }
            if (ke->key() == Qt::Key_Tab) { focusNextField(1); return true; }
            if (ke->key() == Qt::Key_Backtab) { focusNextField(-1); return true; }
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && qobject_cast<QLineEdit *>(fieldEdit_)) {
                commitFieldEdit();
                return true;
            }
        }
        if (e->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(e)->reason() != Qt::PopupFocusReason &&
            static_cast<QFocusEvent *>(e)->reason() != Qt::TabFocusReason && static_cast<QFocusEvent *>(e)->reason() != Qt::BacktabFocusReason) {
            QPointer<QWidget> guard = fieldEdit_;
            QTimer::singleShot(0, this, [this, guard] { if (guard && guard == fieldEdit_) commitFieldEdit(); });
        }
    }
    if (o == textEdit_ && textEdit_) {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            cancelTextEdit();
            return true;
        }
        if (e->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(e)->reason() != Qt::PopupFocusReason)
            QTimer::singleShot(0, this, &PdfView::commitTextEdit);
    }
    return QAbstractScrollArea::eventFilter(o, e);
}

// ------------------------------------------------------------------ text selection
QString PdfView::selectedText() { return hasTextSelection() ? doc_->selectionText(selPage_, selA_, selB_) : QString(); }

void PdfView::copySelection()
{
    QString t = selectedText();
    if (!t.isEmpty()) QApplication::clipboard()->setText(t);
    hideSelBar();
}

// Small bar next to a fresh text selection: one click highlights, underlines, strikes out or copies.
void PdfView::showSelBar(QPointF pos)
{
    hideSelBar();
    selBar_ = new QFrame(viewport());
    selBar_->setObjectName("selBar");
    selBar_->setStyleSheet("#selBar{background:palette(base);border:1px solid palette(mid);border-radius:9px;}"
                           "#selBar QToolButton{border:none;border-radius:6px;padding:3px 5px;}"
                           "#selBar QToolButton:hover{background:rgba(128,128,128,0.25);}");
    auto *lay = new QHBoxLayout(selBar_);
    lay->setContentsMargins(5, 4, 5, 4);
    lay->setSpacing(1);
    QVector<QColor> cols = hlPalette_;
    if (cols.isEmpty()) cols = {QColor(255, 235, 60), QColor(140, 235, 130), QColor(255, 160, 205)};
    for (const QColor &c : std::as_const(cols)) {
        auto *b = new QToolButton(selBar_);
        QPixmap pm(36, 36);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        pp.setRenderHint(QPainter::Antialiasing);
        pp.setPen(QPen(QColor(0, 0, 0, 70), 2));
        pp.setBrush(c);
        pp.drawEllipse(QRectF(4, 4, 28, 28));
        pp.end();
        pm.setDevicePixelRatio(2);
        b->setIcon(QIcon(pm));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tr("Highlight"));
        connect(b, &QToolButton::clicked, this, [this, c] { markupSelection(AnnotKind::Highlight, c); });
        lay->addWidget(b);
    }
    auto text = [&](const QString &label, const QString &tip, bool underline, bool strike, std::function<void()> fn) {
        auto *b = new QToolButton(selBar_);
        QFont f = b->font();
        f.setUnderline(underline);
        f.setStrikeOut(strike);
        f.setBold(underline || strike);
        b->setFont(f);
        b->setText(label);
        b->setToolTip(tip);
        connect(b, &QToolButton::clicked, this, [fn] { fn(); });
        lay->addWidget(b);
    };
    text("U", tr("Underline"), true, false, [this] { markupSelection(AnnotKind::Underline, QColor(220, 40, 40)); });
    text("S", tr("Strike out"), false, true, [this] { markupSelection(AnnotKind::StrikeOut, QColor(220, 40, 40)); });
    text(tr("Copy"), tr("Copy text"), false, false, [this] { copySelection(); });
    selBar_->adjustSize();
    QPoint p(int(pos.x()) + 10, int(pos.y()) + 16);
    p.setX(qBound(4, p.x(), qMax(4, viewport()->width() - selBar_->width() - 4)));
    if (p.y() + selBar_->height() > viewport()->height() - 4) p.setY(int(pos.y()) - selBar_->height() - 16);
    selBar_->move(p);
    selBar_->show();
    selBar_->raise();
}

void PdfView::hideSelBar()
{
    if (!selBar_) return;
    selBar_->hide();
    selBar_->deleteLater();
    selBar_ = nullptr;
}

int PdfView::markupSelection(AnnotKind kind, const QColor &c)
{
    if (!hasTextSelection()) return 0;
    int id = 0, page = selPage_;
    try {
        flushInk();
        id = doc_->addMarkup(selPage_, kind, selQuads_, c);
    } catch (const PdfError &err) { emit statusMessage(err.message()); }
    selPage_ = -1;
    selQuads_.clear();
    hideSelBar();
    viewport()->update();
    if (id && s_noteAfterMarking && !isPreview_) emit noteRequested(page, id);
    return id;
}

// ------------------------------------------------------------------ placing / picking
void PdfView::startPlacingInk(const InkData &d, double widthPt)
{
    cancelModes();
    QRectF b = d.bounds();
    if (b.isEmpty()) return;
    placeInk_ = d;
    placeImage_ = QImage();
    placeSize_ = QSizeF(widthPt, widthPt * b.height() / b.width());
    placing_ = true;
    placePos_ = viewport()->mapFromGlobal(QCursor::pos());
    viewport()->setCursor(Qt::CrossCursor);
    emit statusMessage(tr("Click where the signature should go. Esc cancels."));
}

void PdfView::startPlacingImage(const QImage &img)
{
    cancelModes();
    if (img.isNull()) return;
    placeImage_ = img;
    double w = qMin(240.0, img.width() * 0.75);
    placeSize_ = QSizeF(w, w * img.height() / img.width());
    placing_ = true;
    placePos_ = viewport()->mapFromGlobal(QCursor::pos());
    viewport()->setCursor(Qt::CrossCursor);
    emit statusMessage(tr("Click where the image should go. Esc cancels."));
}

void PdfView::startRectPick(std::function<void(int, QRectF)> cb, const QString &hint)
{
    cancelModes();
    rectCb_ = std::move(cb);
    viewport()->setCursor(Qt::CrossCursor);
    emit statusMessage(hint.isEmpty() ? tr("Drag a rectangle where the signature should appear. Esc cancels.") : hint);
}

// ------------------------------------------------------------------ search
void PdfView::setSearch(const QString &text)
{
    needle_ = text;
    hits_.clear();
    searchTotal_ = 0;
    searchNext_ = 0;
    hitPage_ = hitIndex_ = -1;
    searchTimer_.stop();
    if (!text.isEmpty()) searchTimer_.start(0);
    emit searchChanged(0, 0);
    viewport()->update();
}

void PdfView::runSearchChunk()
{
    int n = doc_->pageCount();
    for (int k = 0; k < 6 && searchNext_ < n; ++k, ++searchNext_) {
        QVector<QPolygonF> h = doc_->search(searchNext_, needle_);
        if (h.isEmpty()) continue;
        searchTotal_ += h.size();
        hits_.insert(searchNext_, h);
        if (hitPage_ < 0 && searchNext_ >= currentPage()) {
            hitPage_ = searchNext_;
            hitIndex_ = 0;
            gotoPage(hitPage_, qMax(0.0, h[0].boundingRect().top() - 80), false);
        }
    }
    if (searchNext_ >= n) {
        searchTimer_.stop();
        if (hitPage_ < 0 && searchTotal_ > 0) findNext(true);
    }
    emit searchChanged(searchTotal_, 0);
    viewport()->update();
}

void PdfView::findNext(bool forward)
{
    if (hits_.isEmpty()) return;
    QList<int> pages = hits_.keys();
    std::sort(pages.begin(), pages.end());
    int pi = pages.indexOf(hitPage_);
    if (pi < 0) {
        pi = forward ? 0 : pages.size() - 1;
        hitIndex_ = forward ? 0 : hits_[pages[pi]].size() - 1;
    } else {
        hitIndex_ += forward ? 1 : -1;
        if (hitIndex_ >= hits_[pages[pi]].size()) { pi = (pi + 1) % pages.size(); hitIndex_ = 0; }
        else if (hitIndex_ < 0) { pi = (pi + pages.size() - 1) % pages.size(); hitIndex_ = hits_[pages[pi]].size() - 1; }
    }
    hitPage_ = pages[pi];
    gotoPage(hitPage_, qMax(0.0, hits_[hitPage_][hitIndex_].boundingRect().top() - 80), false);
    int ordinal = hitIndex_ + 1;
    for (int i = 0; i < pi; ++i) ordinal += hits_[pages[i]].size();
    emit searchChanged(searchTotal_, ordinal);
    viewport()->update();
}

// ------------------------------------------------------------------ test hooks
void PdfView::testPointer(int phase, QPointF pos, float pressure, qint64 t, bool pen)
{
    Ptr p{pos, pressure, false, Qt::LeftButton, t, pen};
    if (phase == 0) press(p);
    else if (phase == 1) move(p);
    else release(p);
}

bool PdfView::testHold()
{
    onHold();
    return builder_.shape() != StrokeBuilder::Shape::None;
}
