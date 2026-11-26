#include "Document.h"
#ifdef PDFABLE_SIGNING
#include "Signer.h"
#endif

#include <QFile>
#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace ink {

// ---------------------------------------------------------------- context
static std::mutex g_locks[FZ_LOCK_MAX];
static void lockCb(void *, int i) { g_locks[i].lock(); }
static void unlockCb(void *, int i) { g_locks[i].unlock(); }
static void quietCb(void *, const char *) {}

static fz_context *baseCtx()
{
    static fz_context *base = [] {
        static fz_locks_context locks{nullptr, lockCb, unlockCb};
        fz_context *c = fz_new_context(nullptr, &locks, 256u << 20);
        fz_register_document_handlers(c);
        fz_set_warning_callback(c, quietCb, nullptr);
        fz_set_error_callback(c, quietCb, nullptr);
        return c;
    }();
    return base;
}

fz_context *fzctx()
{
    struct Holder {
        fz_context *c = fz_clone_context(baseCtx());
        ~Holder() { fz_drop_context(c); }
    };
    thread_local Holder h;
    return h.c;
}

// Run MuPDF code, converting MuPDF errors into C++ exceptions.
template <class F> static void fzrun(F &&f)
{
    fz_context *c = fzctx();
    bool failed = false;
    char msg[512] = "";
    fz_try(c) { f(c); }
    fz_catch(c)
    {
        failed = true;
        std::strncpy(msg, fz_caught_message(c), sizeof(msg) - 1);
    }
    if (failed)
        throw PdfError(QString::fromUtf8(msg));
}

static inline fz_point fp(const QPointF &p) { return fz_make_point(float(p.x()), float(p.y())); }
static inline QPointF qp(fz_point p) { return QPointF(p.x, p.y); }
static inline fz_rect fr(const QRectF &r) { return fz_make_rect(float(r.left()), float(r.top()), float(r.right()), float(r.bottom())); }
static inline QRectF qr(fz_rect r) { return QRectF(QPointF(r.x0, r.y0), QPointF(r.x1, r.y1)); }
static QPolygonF qquad(const fz_quad &q) { return QPolygonF({qp(q.ul), qp(q.ur), qp(q.lr), qp(q.ll)}); }
static inline QPointF qp_point(fz_point p) { return QPointF(p.x, p.y); }

// ---------------------------------------------------------------- InkData
QRectF InkData::bounds() const
{
    QRectF r;
    float maxw = width;
    bool first = true;
    for (const InkStroke &s : strokes) {
        for (float w : s.w) maxw = std::max(maxw, w);
        for (const QPointF &p : s.pts) {
            if (first) { r = QRectF(p, QSizeF(0, 0)); first = false; continue; }
            r.setLeft(std::min(r.left(), p.x()));
            r.setRight(std::max(r.right(), p.x()));
            r.setTop(std::min(r.top(), p.y()));
            r.setBottom(std::max(r.bottom(), p.y()));
        }
    }
    qreal m = maxw / 2 + 1;
    return r.adjusted(-m, -m, m, m);
}

void InkData::transform(const QTransform &t, qreal ws)
{
    width *= ws;
    for (InkStroke &s : strokes) {
        for (QPointF &p : s.pts) p = t.map(p);
        for (float &w : s.w) w *= ws;
    }
}

// ---------------------------------------------------------------- lifecycle
Document::Document(QObject *parent) : QObject(parent) {}
Document::~Document() { close(); }

void Document::close()
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    dropStext();
    if (doc_) pdf_drop_document(fzctx(), doc_);
    doc_ = nullptr;
}

void Document::open(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw PdfError(tr("Cannot open %1: %2").arg(path, f.errorString()));
    QByteArray bytes = f.readAll();
    f.close();
    loadBytes(bytes);
    path_ = QFileInfo(path).absoluteFilePath();
}

void Document::createEmpty()
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    close();
    data_.clear();
    fzrun([&](fz_context *c) { doc_ = pdf_create_document(c); pdf_enable_journal(c, doc_); });
    loadSizes();
}

void Document::loadBytes(const QByteArray &bytes)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    close();
    data_ = bytes;   // MuPDF reads lazily from this buffer; it must outlive doc_
    needsPw_ = false;
    fzrun([&](fz_context *c) {
        fz_stream *stm = fz_open_memory(c, reinterpret_cast<const unsigned char *>(data_.constData()), size_t(data_.size()));
        fz_try(c) doc_ = pdf_open_document_with_stream(c, stm);
        fz_always(c) fz_drop_stream(c, stm);
        fz_catch(c) fz_rethrow(c);
        needsPw_ = pdf_needs_password(c, doc_) != 0;
        if (needsPw_ && !password_.isEmpty())
            needsPw_ = !pdf_authenticate_password(c, doc_, password_.toUtf8().constData());
        if (!needsPw_) pdf_enable_journal(c, doc_);
    });
    if (!needsPw_) loadSizes();
    ++gen_;
    canUndo_ = canRedo_ = false;        // a freshly opened document has no history
    setModified(false);
}

bool Document::authenticate(const QString &password)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    bool ok = false;
    fzrun([&](fz_context *c) {
        ok = pdf_authenticate_password(c, doc_, password.toUtf8().constData()) != 0;
        if (ok) pdf_enable_journal(c, doc_);
    });
    if (ok) {
        password_ = password;
        needsPw_ = false;
        loadSizes();
        emit structureChanged();
    }
    return ok;
}

bool Document::reloadIfChanged()
{
    if (path_.isEmpty()) return false;
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray bytes = f.readAll();
    f.close();
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (bytes == data_) return false;
        if (!bytes.right(2048).contains("%%EOF")) throw PdfError(tr("The file is still being written"));
        QByteArray old = data_;
        try {
            loadBytes(bytes);
        } catch (const PdfError &) {
            loadBytes(old);
            throw;
        }
    }
    emit structureChanged();
    return true;
}

void Document::loadSizes()
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<QSizeF> s;
    QVector<int> keys;
    fzrun([&](fz_context *c) {
        int n = pdf_count_pages(c, doc_);
        s.reserve(n);
        keys.reserve(2 * n);
        for (int i = 0; i < n; ++i) {
            // Compute the visible size from the page dictionary: much cheaper than loading the page.
            pdf_obj *po = pdf_lookup_page_obj(c, doc_, i);
            fz_rect mb = pdf_to_rect(c, pdf_dict_get_inheritable(c, po, PDF_NAME(CropBox)));
            if (fz_is_empty_rect(mb)) mb = pdf_to_rect(c, pdf_dict_get_inheritable(c, po, PDF_NAME(MediaBox)));
            if (fz_is_empty_rect(mb)) mb = fz_make_rect(0, 0, 612, 792);
            float uu = pdf_dict_get_real_default(c, po, PDF_NAME(UserUnit), 1);
            int rot = pdf_to_int(c, pdf_dict_get_inheritable(c, po, PDF_NAME(Rotate)));
            float w = std::fabs(mb.x1 - mb.x0) * uu, h = std::fabs(mb.y1 - mb.y0) * uu;
            if (((rot % 360) + 360) % 180 == 90) std::swap(w, h);
            s.push_back(QSizeF(w, h));
            keys << pdf_to_num(c, po) << rot;
        }
    });
    sizes_ = s;
    pageKeys_ = keys;
}

void Document::setModified(bool m)
{
    if (m == modified_) return;
    modified_ = m;
    emit modifiedChanged(m);
}

void Document::touched(int page, bool structure)
{
    ++gen_;
    dropStext(structure ? -1 : page);
    setModified(true);
    if (structure) {
        // Undo and redo, and some page operations, leave every page where it was, as big and as turned as before
        // (undoing an ink stroke, say). Then only the content changed, which listeners show without starting over;
        // a structure change would make them drop the pictures, search hits and selection.
        const QVector<QSizeF> sizes = sizes_;
        const QVector<int> keys = pageKeys_;
        loadSizes();
        structure = sizes_ != sizes || pageKeys_ != keys;
    }
    if (structure)
        emit structureChanged();
    else
        emit pageChanged(page);
}

template <class F> void Document::pageOp(int page, const char *name, F &&f)
{
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        fzrun([&](fz_context *c) {
            pdf_page *pg = nullptr;
            fz_var(pg);
            pdf_begin_operation(c, doc_, name);
            fz_try(c)
            {
                pg = pdf_load_page(c, doc_, page);
                f(c, pg);
                pdf_update_page(c, pg);
                pdf_end_operation(c, doc_);
                noteHistory(c);
            }
            fz_always(c) fz_drop_page(c, reinterpret_cast<fz_page *>(pg));
            fz_catch(c)
            {
                pdf_abandon_operation(c, doc_);
                fz_rethrow(c);
            }
        });
    }
    touched(page);
}

template <class F> void Document::docOp(const char *name, F &&f)
{
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        fzrun([&](fz_context *c) {
            pdf_begin_operation(c, doc_, name);
            fz_try(c)
            {
                f(c);
                pdf_end_operation(c, doc_);
                noteHistory(c);
            }
            fz_catch(c)
            {
                pdf_abandon_operation(c, doc_);
                fz_rethrow(c);
            }
        });
    }
    touched(-1, true);
}

// ---------------------------------------------------------------- reading
QImage Document::render(int page, double scale, const QRect &clip)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QImage out;
    if (!doc_ || page < 0 || page >= sizes_.size() || clip.isEmpty()) return out;
    fzrun([&](fz_context *c) {
        fz_page *pg = nullptr;
        fz_pixmap *pix = nullptr;
        fz_device *dev = nullptr;
        fz_var(pg); fz_var(pix); fz_var(dev);
        fz_try(c)
        {
            pg = reinterpret_cast<fz_page *>(pdf_load_page(c, doc_, page));
            fz_irect bb = fz_make_irect(clip.left(), clip.top(), clip.left() + clip.width(), clip.top() + clip.height());
            pix = fz_new_pixmap_with_bbox(c, fz_device_bgr(c), bb, nullptr, 1);
            fz_clear_pixmap_with_value(c, pix, 255);
            dev = fz_new_draw_device(c, fz_scale(float(scale), float(scale)), pix);
            fz_run_page(c, pg, dev, fz_identity, nullptr);
            fz_close_device(c, dev);
            out = QImage(fz_pixmap_samples(c, pix), fz_pixmap_width(c, pix), fz_pixmap_height(c, pix),
                         int(fz_pixmap_stride(c, pix)), QImage::Format_RGB32).copy();
        }
        fz_always(c)
        {
            fz_drop_device(c, dev);
            fz_drop_pixmap(c, pix);
            fz_drop_page(c, pg);
        }
        fz_catch(c) fz_rethrow(c);
    });
    return out;
}

static void walkOutline(fz_context *c, fz_outline *o, QVector<OutlineItem> &out)
{
    for (; o; o = o->next) {
        OutlineItem it;
        it.title = QString::fromUtf8(o->title ? o->title : "");
        it.page = o->page.page;
        it.y = o->y;
        if (o->down) walkOutline(c, o->down, it.children);
        out.push_back(it);
    }
}

QVector<OutlineItem> Document::outline()
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<OutlineItem> out;
    if (!doc_) return out;
    try {
        fzrun([&](fz_context *c) {
            fz_outline *o = fz_load_outline(c, reinterpret_cast<fz_document *>(doc_));
            walkOutline(c, o, out);
            fz_drop_outline(c, o);
        });
    } catch (const PdfError &) {}
    return out;
}

QVector<LinkInfo> Document::links(int page)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<LinkInfo> out;
    if (!doc_ || page < 0 || page >= sizes_.size()) return out;
    try {
        fzrun([&](fz_context *c) {
            fz_page *pg = reinterpret_cast<fz_page *>(pdf_load_page(c, doc_, page));
            fz_link *first = nullptr;
            fz_var(first);
            fz_try(c)
            {
                first = fz_load_links(c, pg);
                for (fz_link *l = first; l; l = l->next) {
                    LinkInfo li;
                    li.rect = qr(l->rect);
                    li.uri = QString::fromUtf8(l->uri ? l->uri : "");
                    li.external = fz_is_external_link(c, l->uri);
                    if (!li.external) {
                        float x = 0, y = 0;
                        fz_location loc = fz_resolve_link(c, reinterpret_cast<fz_document *>(doc_), l->uri, &x, &y);
                        li.page = loc.page;
                        li.dest = QPointF(x, y);
                    }
                    out.push_back(li);
                }
            }
            fz_always(c)
            {
                fz_drop_link(c, first);
                fz_drop_page(c, pg);
            }
            fz_catch(c) fz_rethrow(c);
        });
    } catch (const PdfError &) {}
    return out;
}

void *Document::stext(fz_context *c, int page)
{
    auto it = stext_.find(page);
    if (it != stext_.end()) return it.value();
    if (stext_.size() > 12) dropStext();
    fz_page *pg = reinterpret_cast<fz_page *>(pdf_load_page(c, doc_, page));
    fz_stext_page *st = nullptr;
    fz_try(c)
    {
        fz_stext_options opts;
        std::memset(&opts, 0, sizeof opts);
        st = fz_new_stext_page_from_page(c, pg, &opts);
    }
    fz_always(c) fz_drop_page(c, pg);
    fz_catch(c) fz_rethrow(c);
    stext_.insert(page, st);
    return st;
}

void Document::dropStext(int page)
{
    fz_context *c = fzctx();
    if (page < 0) {
        for (void *s : std::as_const(stext_)) fz_drop_stext_page(c, static_cast<fz_stext_page *>(s));
        stext_.clear();
    } else if (stext_.contains(page))
        fz_drop_stext_page(c, static_cast<fz_stext_page *>(stext_.take(page)));
}

QVector<QPolygonF> Document::selectionQuads(int page, QPointF a, QPointF b)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<QPolygonF> out;
    if (!doc_ || page < 0 || page >= sizes_.size()) return out;
    try {
        fzrun([&](fz_context *c) {
            std::vector<fz_quad> q(4096);
            int n = fz_highlight_selection(c, static_cast<fz_stext_page *>(stext(c, page)), fp(a), fp(b), q.data(), int(q.size()));
            for (int i = 0; i < n; ++i) out.push_back(qquad(q[size_t(i)]));
        });
    } catch (const PdfError &) {}
    return out;
}

QString Document::selectionText(int page, QPointF a, QPointF b)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QString out;
    if (!doc_ || page < 0 || page >= sizes_.size()) return out;
    try {
        fzrun([&](fz_context *c) {
            char *s = fz_copy_selection(c, static_cast<fz_stext_page *>(stext(c, page)), fp(a), fp(b), 0);
            out = QString::fromUtf8(s);
            fz_free(c, s);
        });
    } catch (const PdfError &) {}
    return out;
}

QVector<QPolygonF> Document::search(int page, const QString &needle)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<QPolygonF> out;
    if (!doc_ || page < 0 || page >= sizes_.size() || needle.isEmpty()) return out;
    try {
        fzrun([&](fz_context *c) {
            fz_page *pg = reinterpret_cast<fz_page *>(pdf_load_page(c, doc_, page));
            fz_try(c)
            {
                std::vector<fz_quad> q(512);
                std::vector<int> marks(512);
                int n = fz_search_page(c, pg, needle.toUtf8().constData(), marks.data(), q.data(), int(q.size()));
                // merge the quads of one hit (hit_mark is set on the first quad of each hit)
                for (int i = 0; i < n; ++i) {
                    if (marks[size_t(i)] || out.isEmpty()) out.push_back(qquad(q[size_t(i)]));
                    else {
                        QRectF r = out.last().boundingRect().united(qquad(q[size_t(i)]).boundingRect());
                        out.last() = QPolygonF(r);
                    }
                }
            }
            fz_always(c) fz_drop_page(c, pg);
            fz_catch(c) fz_rethrow(c);
        });
    } catch (const PdfError &) {}
    return out;
}

static AnnotKind kindOf(enum pdf_annot_type t)
{
    switch (t) {
    case PDF_ANNOT_INK: return AnnotKind::Ink;
    case PDF_ANNOT_FREE_TEXT: return AnnotKind::FreeText;
    case PDF_ANNOT_STAMP: return AnnotKind::Stamp;
    case PDF_ANNOT_HIGHLIGHT: return AnnotKind::Highlight;
    case PDF_ANNOT_UNDERLINE: return AnnotKind::Underline;
    case PDF_ANNOT_STRIKE_OUT: return AnnotKind::StrikeOut;
    default: return AnnotKind::Other;
    }
}

static int annotId(fz_context *c, pdf_annot *a) { return pdf_to_num(c, pdf_annot_obj(c, a)); }

static pdf_annot *findAnnot(fz_context *c, pdf_page *pg, int id)
{
    for (pdf_annot *a = pdf_first_annot(c, pg); a; a = pdf_next_annot(c, a))
        if (annotId(c, a) == id) return a;
    fz_throw(c, FZ_ERROR_GENERIC, "annotation not found");
}

static InkData readInk(fz_context *c, pdf_annot *a)
{
    InkData d;
    int n = 0;
    float col[4] = {0, 0, 0, 0};
    pdf_annot_color(c, a, &n, col);
    if (n == 1) d.color = QColor::fromRgbF(col[0], col[0], col[0]);
    else if (n == 3) d.color = QColor::fromRgbF(col[0], col[1], col[2]);
    else if (n == 4) d.color = QColor::fromCmykF(col[0], col[1], col[2], col[3]);
    d.width = pdf_annot_border_width(c, a);
    if (d.width <= 0) d.width = 1;
    pdf_obj *obj = pdf_annot_obj(c, a);
    d.highlighter = pdf_to_bool(c, pdf_dict_gets(c, obj, "PDFableHighlighter"));
    pdf_obj *wl = pdf_dict_gets(c, obj, "PDFableWidths");
    int count = pdf_annot_ink_list_count(c, a);
    for (int i = 0; i < count; ++i) {
        InkStroke s;
        int k = pdf_annot_ink_list_stroke_count(c, a, i);
        s.pts.reserve(k);
        for (int j = 0; j < k; ++j) s.pts.push_back(qp(pdf_annot_ink_list_stroke_vertex(c, a, i, j)));
        pdf_obj *w = pdf_array_get(c, wl, i);
        if (pdf_array_len(c, w) == k) {
            s.w.reserve(k);
            for (int j = 0; j < k; ++j) s.w.push_back(pdf_array_get_real(c, w, j));
        }
        d.strokes.push_back(s);
    }
    return d;
}

static QByteArray num(double v) { return QByteArray::number(v, 'f', 2); }

// Writes geometry, our private width data and a hand-built appearance stream
// (variable-width, round-capped segments) so every viewer shows the same ink.
static void writeInk(fz_context *c, pdf_document *doc, pdf_page *pg, pdf_annot *a, const InkData &d)
{
    pdf_clear_annot_ink_list(c, a);
    bool anyW = false;
    for (const InkStroke &s : d.strokes) {
        std::vector<fz_point> pts;
        pts.reserve(size_t(s.pts.size()));
        for (const QPointF &p : s.pts) pts.push_back(fp(p));
        if (!pts.empty()) pdf_add_annot_ink_list(c, a, int(pts.size()), pts.data());
        anyW = anyW || !s.w.isEmpty();
    }
    float col[3] = {d.color.redF(), d.color.greenF(), d.color.blueF()};
    pdf_set_annot_color(c, a, 3, col);
    pdf_set_annot_border_width(c, a, d.width);

    pdf_obj *obj = pdf_annot_obj(c, a);
    pdf_dict_dels(c, obj, "PDFableWidths");
    pdf_dict_dels(c, obj, "PDFableHighlighter");
    if (d.highlighter) pdf_dict_puts_drop(c, obj, "PDFableHighlighter", PDF_TRUE);
    if (anyW) {
        pdf_obj *wl = pdf_new_array(c, doc, int(d.strokes.size()));
        pdf_dict_puts_drop(c, obj, "PDFableWidths", wl);
        for (const InkStroke &s : d.strokes) {
            pdf_obj *w = pdf_new_array(c, doc, int(s.w.size()));
            pdf_array_push_drop(c, wl, w);
            for (float v : s.w) pdf_array_push_real(c, w, std::round(v * 100) / 100);
        }
    }

    fz_rect mediabox;
    fz_matrix ctm;
    pdf_page_transform(c, pg, &mediabox, &ctm);
    fz_matrix inv = fz_invert_matrix(ctm);
    fz_rect rectPdf = fz_transform_rect(fr(d.bounds()), inv);

    QByteArray s = "q " + num(inv.a) + " " + num(inv.b) + " " + num(inv.c) + " " + num(inv.d) + " " +
                   num(inv.e) + " " + num(inv.f) + " cm 1 J 1 j " + num(col[0]) + " " + num(col[1]) + " " +
                   num(col[2]) + " RG\n";
    if (d.highlighter) s += "/GS0 gs\n";
    for (const InkStroke &st : d.strokes) {
        int n = int(st.pts.size());
        if (n == 0) continue;
        if (st.w.size() == n && n > 1) {
            for (int i = 1; i < n; ++i)
                s += num((st.w[i - 1] + st.w[i]) / 2) + " w " + num(st.pts[i - 1].x()) + " " + num(st.pts[i - 1].y()) +
                     " m " + num(st.pts[i].x()) + " " + num(st.pts[i].y()) + " l S\n";
        } else {
            s += num(st.w.size() == n ? st.w[0] : d.width) + " w " + num(st.pts[0].x()) + " " + num(st.pts[0].y()) + " m\n";
            for (int i = 1; i < n; ++i) s += num(st.pts[i].x()) + " " + num(st.pts[i].y()) + " l\n";
            if (n == 1) s += num(st.pts[0].x()) + " " + num(st.pts[0].y()) + " l\n";
            s += "S\n";
        }
    }
    s += "Q\n";

    pdf_obj *res = nullptr;
    fz_buffer *buf = nullptr;
    fz_var(res); fz_var(buf);
    fz_try(c)
    {
        res = pdf_new_dict(c, doc, 1);
        if (d.highlighter) {
            pdf_obj *gs = pdf_dict_put_dict(c, pdf_dict_put_dict(c, res, PDF_NAME(ExtGState), 1), pdf_new_name(c, "GS0"), 2);
            pdf_dict_put(c, gs, PDF_NAME(BM), PDF_NAME(Multiply));
        }
        buf = fz_new_buffer_from_copied_data(c, reinterpret_cast<const unsigned char *>(s.constData()), size_t(s.size()));
        pdf_set_annot_appearance(c, a, "N", nullptr, fz_identity, rectPdf, res, buf);
        pdf_dict_put_rect(c, obj, PDF_NAME(Rect), rectPdf);
    }
    fz_always(c)
    {
        fz_drop_buffer(c, buf);
        pdf_drop_obj(c, res);
    }
    fz_catch(c) fz_rethrow(c);
}

QVector<AnnotInfo> Document::annots(int page)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<AnnotInfo> out;
    if (!doc_ || page < 0 || page >= sizes_.size()) return out;
    try {
        fzrun([&](fz_context *c) {
            pdf_page *pg = pdf_load_page(c, doc_, page);
            fz_try(c)
            {
                for (pdf_annot *a = pdf_first_annot(c, pg); a; a = pdf_next_annot(c, a)) {
                    AnnotInfo ai;
                    ai.id = annotId(c, a);
                    ai.kind = kindOf(pdf_annot_type(c, a));
                    ai.rect = qr(pdf_bound_annot(c, a));
                    if (ai.kind == AnnotKind::FreeText) {
                        ai.text = QString::fromUtf8(pdf_annot_contents(c, a));
                        const char *font = nullptr;
                        int n = 0;
                        float col[4] = {0, 0, 0, 0};
                        pdf_annot_default_appearance(c, a, &font, &ai.fontSize, &n, col);
                        ai.color = n == 3 ? QColor::fromRgbF(col[0], col[1], col[2]) : n == 1 ? QColor::fromRgbF(col[0], col[0], col[0]) : QColor(Qt::black);
                    } else if (ai.kind == AnnotKind::Highlight || ai.kind == AnnotKind::Underline || ai.kind == AnnotKind::StrikeOut) {
                        ai.text = QString::fromUtf8(pdf_annot_contents(c, a));
                        int n = 0;
                        float col[4] = {0, 0, 0, 0};
                        pdf_annot_color(c, a, &n, col);
                        ai.color = n == 3 ? QColor::fromRgbF(col[0], col[1], col[2]) : n == 1 ? QColor::fromRgbF(col[0], col[0], col[0]) : QColor(255, 235, 60);
                        int nq = pdf_annot_quad_point_count(c, a);
                        for (int q = 0; q < nq; ++q) ai.quads.push_back(qquad(pdf_annot_quad_point(c, a, q)));
                    }
                    out.push_back(ai);
                }
            }
            fz_always(c) fz_drop_page(c, reinterpret_cast<fz_page *>(pg));
            fz_catch(c) fz_rethrow(c);
        });
    } catch (const PdfError &) {}
    return out;
}

QVector<InkAnnot> Document::inkAnnots(int page)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<InkAnnot> out;
    if (!doc_ || page < 0 || page >= sizes_.size()) return out;
    try {
        fzrun([&](fz_context *c) {
            pdf_page *pg = pdf_load_page(c, doc_, page);
            fz_try(c)
            {
                for (pdf_annot *a = pdf_first_annot(c, pg); a; a = pdf_next_annot(c, a))
                    if (pdf_annot_type(c, a) == PDF_ANNOT_INK) out.push_back({annotId(c, a), readInk(c, a)});
            }
            fz_always(c) fz_drop_page(c, reinterpret_cast<fz_page *>(pg));
            fz_catch(c) fz_rethrow(c);
        });
    } catch (const PdfError &) {}
    return out;
}

// ---------------------------------------------------------------- annotation editing
int Document::addInk(int page, const InkData &d)
{
    int id = 0;
    pageOp(page, "Ink", [&](fz_context *c, pdf_page *pg) {
        pdf_annot *a = pdf_create_annot(c, pg, PDF_ANNOT_INK);
        fz_try(c) { writeInk(c, doc_, pg, a, d); id = annotId(c, a); }
        fz_always(c) pdf_drop_annot(c, a);
        fz_catch(c) fz_rethrow(c);
    });
    return id;
}

void Document::setInk(int page, int id, const InkData &d)
{
    pageOp(page, "Edit ink", [&](fz_context *c, pdf_page *pg) { writeInk(c, doc_, pg, findAnnot(c, pg, id), d); });
}

int Document::addMarkup(int page, AnnotKind kind, const QVector<QPolygonF> &quads, const QColor &col)
{
    int id = 0;
    pageOp(page, "Markup", [&](fz_context *c, pdf_page *pg) {
        enum pdf_annot_type t = kind == AnnotKind::Underline ? PDF_ANNOT_UNDERLINE
                              : kind == AnnotKind::StrikeOut ? PDF_ANNOT_STRIKE_OUT : PDF_ANNOT_HIGHLIGHT;
        pdf_annot *a = pdf_create_annot(c, pg, t);
        fz_try(c)
        {
            float rgb[3] = {col.redF(), col.greenF(), col.blueF()};
            pdf_set_annot_color(c, a, 3, rgb);
            for (const QPolygonF &q : quads) {
                if (q.size() < 4) continue;
                fz_quad fq;
                fq.ul = fp(q[0]); fq.ur = fp(q[1]); fq.lr = fp(q[2]); fq.ll = fp(q[3]);
                pdf_add_annot_quad_point(c, a, fq);
            }
            id = annotId(c, a);
        }
        fz_always(c) pdf_drop_annot(c, a);
        fz_catch(c) fz_rethrow(c);
    });
    return id;
}

int Document::addFreeText(int page, const QRectF &r, const QString &text, float size, const QColor &col)
{
    int id = 0;
    pageOp(page, "Text", [&](fz_context *c, pdf_page *pg) {
        pdf_annot *a = pdf_create_annot(c, pg, PDF_ANNOT_FREE_TEXT);
        fz_try(c)
        {
            float rgb[3] = {col.redF(), col.greenF(), col.blueF()};
            pdf_set_annot_rect(c, a, fr(r));
            pdf_set_annot_border_width(c, a, 0);
            pdf_set_annot_default_appearance(c, a, "Helv", size, 3, rgb);
            pdf_set_annot_contents(c, a, text.toUtf8().constData());
            id = annotId(c, a);
        }
        fz_always(c) pdf_drop_annot(c, a);
        fz_catch(c) fz_rethrow(c);
    });
    return id;
}

void Document::setFreeText(int page, int id, const QString &text, const QRectF &rect)
{
    pageOp(page, "Edit text", [&](fz_context *c, pdf_page *pg) {
        pdf_annot *a = findAnnot(c, pg, id);
        if (rect.isValid()) pdf_set_annot_rect(c, a, fr(rect));
        pdf_set_annot_contents(c, a, text.toUtf8().constData());
    });
}

int Document::addStamp(int page, const QRectF &r, const QImage &src)
{
    int id = 0;
    QImage img = src.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    pageOp(page, "Image", [&](fz_context *c, pdf_page *pg) {
        pdf_annot *a = pdf_create_annot(c, pg, PDF_ANNOT_STAMP);
        fz_pixmap *pix = nullptr;
        fz_image *fi = nullptr;
        fz_var(pix); fz_var(fi);
        fz_try(c)
        {
            pix = fz_new_pixmap(c, fz_device_rgb(c), img.width(), img.height(), nullptr, 1);
            unsigned char *dst = fz_pixmap_samples(c, pix);
            ptrdiff_t stride = fz_pixmap_stride(c, pix);
            for (int y = 0; y < img.height(); ++y)
                std::memcpy(dst + y * stride, img.constScanLine(y), size_t(img.width()) * 4);
            fi = fz_new_image_from_pixmap(c, pix, nullptr);
            pdf_set_annot_rect(c, a, fr(r));
            pdf_set_annot_stamp_image(c, a, fi);
            pdf_set_annot_rect(c, a, fr(r));
            id = annotId(c, a);
        }
        fz_always(c)
        {
            fz_drop_image(c, fi);
            fz_drop_pixmap(c, pix);
            pdf_drop_annot(c, a);
        }
        fz_catch(c) fz_rethrow(c);
    });
    return id;
}

void Document::setAnnotRect(int page, int id, const QRectF &r)
{
    pageOp(page, "Move", [&](fz_context *c, pdf_page *pg) { pdf_set_annot_rect(c, findAnnot(c, pg, id), fr(r)); });
}

void Document::scaleFreeText(int page, int id, const QRectF &r, float fontSize)
{
    pageOp(page, "Resize text", [&](fz_context *c, pdf_page *pg) {
        pdf_annot *a = findAnnot(c, pg, id);
        const char *font = nullptr;
        float size = 0, col[4] = {0, 0, 0, 0};
        int n = 0;
        pdf_annot_default_appearance(c, a, &font, &size, &n, col);
        QByteArray fontName = font ? QByteArray(font) : QByteArray("Helv");
        pdf_set_annot_rect(c, a, fr(r));
        pdf_set_annot_default_appearance(c, a, fontName.constData(), fontSize, n, col);
    });
}

void Document::setAnnotContents(int page, int id, const QString &text)
{
    pageOp(page, "Note", [&](fz_context *c, pdf_page *pg) { pdf_set_annot_contents(c, findAnnot(c, pg, id), text.toUtf8().constData()); });
}

// Reads the markings straight from the page dictionaries: much faster than loading every page,
// which matters for long documents because the notes sidebar needs all of them.
QVector<MarkupNote> Document::markups()
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<MarkupNote> out;
    if (!doc_) return out;
    try {
        fzrun([&](fz_context *c) {
            int n = pdf_count_pages(c, doc_);
            for (int i = 0; i < n; ++i) {
                pdf_obj *po = pdf_lookup_page_obj(c, doc_, i);
                pdf_obj *annots = pdf_dict_get(c, po, PDF_NAME(Annots));
                int na = pdf_array_len(c, annots);
                if (na == 0) continue;
                fz_rect box;
                fz_matrix ctm;
                pdf_page_obj_transform(c, po, &box, &ctm);
                for (int k = 0; k < na; ++k) {
                    pdf_obj *ref = pdf_array_get(c, annots, k);
                    pdf_obj *st = pdf_dict_get(c, ref, PDF_NAME(Subtype));
                    MarkupNote m;
                    if (pdf_name_eq(c, st, PDF_NAME(Highlight))) m.kind = AnnotKind::Highlight;
                    else if (pdf_name_eq(c, st, PDF_NAME(Underline))) m.kind = AnnotKind::Underline;
                    else if (pdf_name_eq(c, st, PDF_NAME(StrikeOut))) m.kind = AnnotKind::StrikeOut;
                    else continue;
                    m.page = i;
                    m.id = pdf_to_num(c, ref);
                    if (m.id <= 0) continue;
                    m.rect = qr(fz_transform_rect(pdf_to_rect(c, pdf_dict_get(c, ref, PDF_NAME(Rect))), ctm));
                    m.text = QString::fromUtf8(pdf_dict_get_text_string(c, ref, PDF_NAME(Contents)));
                    pdf_obj *col = pdf_dict_get(c, ref, PDF_NAME(C));
                    int cn = pdf_array_len(c, col);
                    if (cn == 3) m.color = QColor::fromRgbF(pdf_array_get_real(c, col, 0), pdf_array_get_real(c, col, 1), pdf_array_get_real(c, col, 2));
                    else if (cn == 1) m.color = QColor::fromRgbF(pdf_array_get_real(c, col, 0), pdf_array_get_real(c, col, 0), pdf_array_get_real(c, col, 0));
                    else m.color = QColor(255, 235, 60);
                    pdf_obj *qp = pdf_dict_get(c, ref, PDF_NAME(QuadPoints));
                    int nq = pdf_array_len(c, qp) / 8;
                    for (int q = 0; q < nq; ++q) {
                        auto pt = [&](int j) {
                            return qp_point(fz_transform_point(fz_make_point(pdf_array_get_real(c, qp, q * 8 + j * 2), pdf_array_get_real(c, qp, q * 8 + j * 2 + 1)), ctm));
                        };
                        // PDF order: upper-left, upper-right, lower-left, lower-right
                        m.quads.push_back(QPolygonF({pt(0), pt(1), pt(3), pt(2)}));
                    }
                    if (m.quads.isEmpty()) m.quads.push_back(QPolygonF(m.rect));
                    out.push_back(m);
                }
            }
        });
    } catch (const PdfError &) {}
    return out;
}

QString Document::textInQuads(int page, const QVector<QPolygonF> &quads)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QStringList parts;
    if (!doc_ || page < 0 || page >= sizes_.size()) return QString();
    try {
        fzrun([&](fz_context *c) {
            auto *st = static_cast<fz_stext_page *>(stext(c, page));
            for (const QPolygonF &q : quads) {
                QRectF r = q.boundingRect();
                // shrink vertically a little so the line above/below is not picked up
                r.adjust(0, r.height() * 0.25, 0, -r.height() * 0.25);
                char *t = fz_copy_rectangle(c, st, fr(r), 0);
                parts << QString::fromUtf8(t).simplified();
                fz_free(c, t);
            }
        });
    } catch (const PdfError &) {}
    return parts.join(' ').simplified();
}

// ---------------------------------------------------------------- form fields
static pdf_annot *findWidget(fz_context *c, pdf_page *pg, int id)
{
    for (pdf_annot *w = pdf_first_widget(c, pg); w; w = pdf_next_widget(c, w))
        if (annotId(c, w) == id) return w;
    fz_throw(c, FZ_ERROR_GENERIC, "form field not found");
}

QVector<FieldInfo> Document::fields(int page)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<FieldInfo> out;
    if (!doc_ || page < 0 || page >= sizes_.size()) return out;
    try {
        fzrun([&](fz_context *c) {
            // cheap test first: most pages have no widgets at all
            pdf_obj *annots = pdf_dict_get(c, pdf_lookup_page_obj(c, doc_, page), PDF_NAME(Annots));
            bool any = false;
            for (int k = 0; k < pdf_array_len(c, annots) && !any; ++k)
                any = pdf_name_eq(c, pdf_dict_get(c, pdf_array_get(c, annots, k), PDF_NAME(Subtype)), PDF_NAME(Widget));
            if (!any) return;
            pdf_page *pg = pdf_load_page(c, doc_, page);
            fz_try(c)
            {
                for (pdf_annot *w = pdf_first_widget(c, pg); w; w = pdf_next_widget(c, w)) {
                    FieldInfo f;
                    f.id = annotId(c, w);
                    f.rect = qr(pdf_bound_annot(c, w));
                    pdf_obj *obj = pdf_annot_obj(c, w);
                    int flags = pdf_field_flags(c, obj);
                    f.readonly = pdf_widget_is_readonly(c, w) != 0;
                    switch (pdf_widget_type(c, w)) {
                    case PDF_WIDGET_TYPE_TEXT: f.kind = FieldKind::Text; break;
                    case PDF_WIDGET_TYPE_CHECKBOX: f.kind = FieldKind::CheckBox; break;
                    case PDF_WIDGET_TYPE_RADIOBUTTON: f.kind = FieldKind::Radio; break;
                    case PDF_WIDGET_TYPE_COMBOBOX: f.kind = FieldKind::Combo; break;
                    case PDF_WIDGET_TYPE_LISTBOX: f.kind = FieldKind::List; break;
                    case PDF_WIDGET_TYPE_BUTTON: f.kind = FieldKind::Button; break;
                    case PDF_WIDGET_TYPE_SIGNATURE: f.kind = FieldKind::Signature; break;
                    default: f.kind = FieldKind::Other; break;
                    }
                    f.value = QString::fromUtf8(pdf_field_value(c, obj));
                    if (f.kind == FieldKind::CheckBox || f.kind == FieldKind::Radio) {
                        // the widget is "on" when its appearance state is not Off
                        pdf_obj *as = pdf_dict_get(c, obj, PDF_NAME(AS));
                        f.checked = as && !pdf_name_eq(c, as, PDF_NAME(Off));
                    }
                    if (f.kind == FieldKind::Text) {
                        f.multiline = (flags & PDF_TX_FIELD_IS_MULTILINE) != 0;
                        f.maxLen = pdf_text_widget_max_len(c, w);
                    }
                    if (f.kind == FieldKind::Combo || f.kind == FieldKind::List) {
                        int n = pdf_choice_widget_options(c, w, 0, nullptr);
                        std::vector<const char *> opts(size_t(qMax(n, 0)));
                        if (n > 0) pdf_choice_widget_options(c, w, 0, opts.data());
                        for (const char *o : opts) f.options << QString::fromUtf8(o ? o : "");
                    }
                    if (f.kind == FieldKind::Text || f.kind == FieldKind::Combo) {
                        const char *font = nullptr;
                        int n = 0;
                        float col[4] = {0, 0, 0, 0};
                        fz_try(c) pdf_annot_default_appearance(c, w, &font, &f.fontSize, &n, col);
                        fz_catch(c) f.fontSize = 0;
                    }
                    out.push_back(f);
                }
            }
            fz_always(c) fz_drop_page(c, reinterpret_cast<fz_page *>(pg));
            fz_catch(c) fz_rethrow(c);
        });
    } catch (const PdfError &) {}
    return out;
}

void Document::setFieldText(int page, int id, const QString &text)
{
    pageOp(page, "Fill field", [&](fz_context *c, pdf_page *pg) {
        if (!pdf_set_text_field_value(c, findWidget(c, pg, id), text.toUtf8().constData()))
            fz_throw(c, FZ_ERROR_GENERIC, "the form rejected this value");
    });
}

void Document::toggleField(int page, int id)
{
    pageOp(page, "Toggle field", [&](fz_context *c, pdf_page *pg) { pdf_toggle_widget(c, findWidget(c, pg, id)); });
}

void Document::setFieldChoice(int page, int id, const QString &option)
{
    pageOp(page, "Choose", [&](fz_context *c, pdf_page *pg) {
        QByteArray o = option.toUtf8();
        const char *opts[1] = {o.constData()};
        pdf_choice_widget_set_value(c, findWidget(c, pg, id), 1, opts);
    });
}

void Document::removeAnnots(int page, const QVector<int> &ids)
{
    if (ids.isEmpty()) return;
    pageOp(page, "Delete", [&](fz_context *c, pdf_page *pg) {
        for (int id : ids) pdf_delete_annot(c, pg, findAnnot(c, pg, id));
    });
}

// ---------------------------------------------------------------- page editing
void Document::rearrange(const QVector<int> &order)
{
    docOp("Arrange pages", [&](fz_context *c) {
        int n = pdf_count_pages(c, doc_);
        std::vector<pdf_obj *> all, cur, want;
        for (int i = 0; i < n; ++i) all.push_back(pdf_keep_obj(c, pdf_lookup_page_obj(c, doc_, i)));
        fz_try(c)
        {
            std::vector<char> keep(size_t(n), 0);
            for (int o : order) {
                if (o < 0 || o >= n || keep[size_t(o)]) fz_throw(c, FZ_ERROR_GENERIC, "invalid page order");
                keep[size_t(o)] = 1;
                want.push_back(all[size_t(o)]);
            }
            if (want.empty()) fz_throw(c, FZ_ERROR_GENERIC, "a document needs at least one page");
            cur = all;
            for (int i = n - 1; i >= 0; --i)
                if (!keep[size_t(i)]) {
                    pdf_delete_page(c, doc_, i);
                    cur.erase(cur.begin() + i);
                }
            for (size_t k = 0; k < want.size(); ++k) {
                size_t j = size_t(std::find(cur.begin(), cur.end(), want[k]) - cur.begin());
                if (j == k) continue;
                pdf_delete_page(c, doc_, int(j));
                pdf_insert_page(c, doc_, int(k), want[k]);
                cur.erase(cur.begin() + long(j));
                cur.insert(cur.begin() + long(k), want[k]);
            }
        }
        fz_always(c) { for (pdf_obj *o : all) pdf_drop_obj(c, o); }
        fz_catch(c) fz_rethrow(c);
    });
}

void Document::rotatePages(const QVector<int> &pages, int delta)
{
    docOp("Rotate pages", [&](fz_context *c) {
        for (int p : pages) {
            pdf_obj *po = pdf_lookup_page_obj(c, doc_, p);
            int rot = pdf_to_int(c, pdf_dict_get_inheritable(c, po, PDF_NAME(Rotate)));
            pdf_dict_put_int(c, po, PDF_NAME(Rotate), (((rot + delta) % 360) + 360) % 360);
        }
    });
}

void Document::insertBlank(int at, QSizeF size)
{
    docOp("Insert page", [&](fz_context *c) {
        fz_buffer *buf = fz_new_buffer(c, 1);
        pdf_obj *po = nullptr;
        fz_var(po);
        fz_try(c)
        {
            po = pdf_add_page(c, doc_, fz_make_rect(0, 0, float(size.width()), float(size.height())), 0, nullptr, buf);
            pdf_insert_page(c, doc_, at, po);
        }
        fz_always(c)
        {
            pdf_drop_obj(c, po);
            fz_drop_buffer(c, buf);
        }
        fz_catch(c) fz_rethrow(c);
    });
}

// pdf_graft_mapped_page copies the page content only; bring ink, text boxes, highlights and
// stamps along as well. Links, pop-ups and form fields are left behind.
static void graftPage(fz_context *c, pdf_graft_map *map, pdf_document *dst, int at, pdf_document *src, int from)
{
    pdf_graft_mapped_page(c, map, at, src, from);
    pdf_obj *dpage = pdf_lookup_page_obj(c, dst, at < 0 ? pdf_count_pages(c, dst) - 1 : at);
    pdf_obj *sann = pdf_dict_get(c, pdf_lookup_page_obj(c, src, from), PDF_NAME(Annots));
    int n = pdf_array_len(c, sann);
    pdf_obj *dann = nullptr;
    for (int i = 0; i < n; ++i) {
        pdf_obj *a = pdf_array_get(c, sann, i);
        pdf_obj *st = pdf_dict_get(c, a, PDF_NAME(Subtype));
        if (pdf_name_eq(c, st, PDF_NAME(Link)) || pdf_name_eq(c, st, PDF_NAME(Popup)) || pdf_name_eq(c, st, PDF_NAME(Widget)) ||
            pdf_dict_get(c, a, PDF_NAME(IRT)))
            continue;
        pdf_obj *copy = pdf_copy_dict(c, a);     // detach from the source page before grafting
        pdf_obj *grafted = nullptr, *ind = nullptr;
        fz_var(grafted); fz_var(ind);
        fz_try(c)
        {
            pdf_dict_del(c, copy, PDF_NAME(P));
            pdf_dict_del(c, copy, PDF_NAME(Popup));
            pdf_dict_del(c, copy, PDF_NAME(Parent));
            grafted = pdf_graft_mapped_object(c, map, copy);
            ind = pdf_add_object(c, dst, grafted);
            pdf_dict_put(c, ind, PDF_NAME(P), dpage);
            if (!dann) dann = pdf_dict_put_array(c, dpage, PDF_NAME(Annots), n);
            pdf_array_push(c, dann, ind);
        }
        fz_always(c)
        {
            pdf_drop_obj(c, ind);
            pdf_drop_obj(c, grafted);
            pdf_drop_obj(c, copy);
        }
        fz_catch(c) fz_rethrow(c);
    }
}

static pdf_document *openSource(fz_context *c, const QString &path)
{
    pdf_document *src = pdf_open_document(c, path.toUtf8().constData());
    if (pdf_needs_password(c, src)) {
        pdf_drop_document(c, src);
        fz_throw(c, FZ_ERROR_GENERIC, "password protected files cannot be inserted");
    }
    return src;
}

int Document::insertPdf(const QString &path, int at)
{
    int n = 0;
    docOp("Insert pages", [&](fz_context *c) {
        pdf_document *src = openSource(c, path);
        pdf_graft_map *map = nullptr;
        fz_var(map);
        fz_try(c)
        {
            map = pdf_new_graft_map(c, doc_);
            n = pdf_count_pages(c, src);
            for (int i = 0; i < n; ++i) graftPage(c, map, doc_, at + i, src, i);
        }
        fz_always(c)
        {
            pdf_drop_graft_map(c, map);
            pdf_drop_document(c, src);
        }
        fz_catch(c) fz_rethrow(c);
    });
    return n;
}

static void saveNew(fz_context *c, pdf_document *d, const QString &outPath)
{
    pdf_write_options o = pdf_default_write_options;
    o.do_garbage = 3;
    o.do_compress = 1;
    o.do_compress_images = 1;
    o.do_compress_fonts = 1;
    pdf_save_document(c, d, outPath.toUtf8().constData(), &o);
}

void Document::extractPages(const QVector<int> &pages, const QString &outPath)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    fzrun([&](fz_context *c) {
        pdf_document *dst = pdf_create_document(c);
        pdf_graft_map *map = nullptr;
        fz_var(map);
        fz_try(c)
        {
            map = pdf_new_graft_map(c, dst);
            for (int p : pages) graftPage(c, map, dst, -1, doc_, p);
            saveNew(c, dst, outPath);
        }
        fz_always(c)
        {
            pdf_drop_graft_map(c, map);
            pdf_drop_document(c, dst);
        }
        fz_catch(c) fz_rethrow(c);
    });
}

void Document::mergeFiles(const QStringList &paths, const QString &outPath)
{
    fzrun([&](fz_context *c) {
        pdf_document *dst = pdf_create_document(c);
        fz_try(c)
        {
            for (const QString &p : paths) {
                pdf_document *src = openSource(c, p);
                pdf_graft_map *map = nullptr;
                fz_var(map);
                fz_try(c)
                {
                    map = pdf_new_graft_map(c, dst);
                    int n = pdf_count_pages(c, src);
                    for (int i = 0; i < n; ++i) graftPage(c, map, dst, -1, src, i);
                }
                fz_always(c)
                {
                    pdf_drop_graft_map(c, map);
                    pdf_drop_document(c, src);
                }
                fz_catch(c) fz_rethrow(c);
            }
            saveNew(c, dst, outPath);
        }
        fz_always(c) pdf_drop_document(c, dst);
        fz_catch(c) fz_rethrow(c);
    });
}

void Document::assemble(const QVector<AssemblyItem> &items, const QString &outPath)
{
    if (items.isEmpty()) throw PdfError(tr("Nothing to put into the PDF"));
    // images are prepared outside MuPDF: JPEG files are embedded as they are, everything else as PNG
    QHash<QString, QByteArray> imageBytes;
    for (const AssemblyItem &it : items) {
        if (!it.image || imageBytes.contains(it.path)) continue;
        QImageReader rd(it.path);
        rd.setAutoTransform(true);
        QByteArray bytes;
        if (rd.format() == "jpeg" && rd.transformation() == QImageIOHandler::TransformationNone) {
            QFile f(it.path);
            if (f.open(QIODevice::ReadOnly)) bytes = f.readAll();
        } else {
            QImage img = rd.read();
            if (img.isNull()) throw PdfError(tr("Cannot read image %1").arg(it.path));
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, img.hasAlphaChannel() || rd.format() != "jpeg" ? "PNG" : "JPEG", 90);
        }
        if (bytes.isEmpty()) throw PdfError(tr("Cannot read image %1").arg(it.path));
        imageBytes.insert(it.path, bytes);
    }
    fzrun([&](fz_context *c) {
        pdf_document *dst = pdf_create_document(c);
        struct Src { pdf_document *doc; pdf_graft_map *map; };
        QHash<QString, Src> sources;
        fz_try(c)
        {
            for (const AssemblyItem &it : items) {
                if (it.image) {
                    const QByteArray &bytes = imageBytes[it.path];
                    fz_buffer *ibuf = nullptr, *content = nullptr;
                    fz_image *img = nullptr;
                    pdf_obj *res = nullptr, *page = nullptr;
                    fz_var(ibuf); fz_var(content); fz_var(img); fz_var(res); fz_var(page);
                    fz_try(c)
                    {
                        ibuf = fz_new_buffer_from_copied_data(c, reinterpret_cast<const unsigned char *>(bytes.constData()), size_t(bytes.size()));
                        img = fz_new_image_from_buffer(c, ibuf);
                        int xres = 96, yres = 96;
                        fz_image_resolution(img, &xres, &yres);
                        float w = img->w * 72.f / float(xres > 0 ? xres : 96), h = img->h * 72.f / float(yres > 0 ? yres : 96);
                        if (w > 842 || h > 1191) {            // larger than A3: scale down to A4 width
                            float k = 595.f / w;
                            w *= k;
                            h *= k;
                        }
                        res = pdf_new_dict(c, dst, 1);
                        pdf_obj *xo = pdf_dict_put_dict(c, res, PDF_NAME(XObject), 1);
                        pdf_dict_puts_drop(c, xo, "Im0", pdf_add_image(c, dst, img));
                        QByteArray cs = "q " + QByteArray::number(w, 'f', 2) + " 0 0 " + QByteArray::number(h, 'f', 2) + " 0 0 cm /Im0 Do Q\n";
                        content = fz_new_buffer_from_copied_data(c, reinterpret_cast<const unsigned char *>(cs.constData()), size_t(cs.size()));
                        page = pdf_add_page(c, dst, fz_make_rect(0, 0, w, h), ((it.rotate % 360) + 360) % 360, res, content);
                        pdf_insert_page(c, dst, pdf_count_pages(c, dst), page);
                    }
                    fz_always(c)
                    {
                        pdf_drop_obj(c, page);
                        pdf_drop_obj(c, res);
                        fz_drop_image(c, img);
                        fz_drop_buffer(c, content);
                        fz_drop_buffer(c, ibuf);
                    }
                    fz_catch(c) fz_rethrow(c);
                    continue;
                }
                auto s = sources.find(it.path);
                if (s == sources.end()) {
                    Src ns{openSource(c, it.path), nullptr};
                    s = sources.insert(it.path, ns);
                    s->map = pdf_new_graft_map(c, dst);
                }
                if (it.page < 0 || it.page >= pdf_count_pages(c, s->doc)) fz_throw(c, FZ_ERROR_GENERIC, "page out of range");
                graftPage(c, s->map, dst, -1, s->doc, it.page);
                if (it.rotate % 360) {
                    pdf_obj *po = pdf_lookup_page_obj(c, dst, pdf_count_pages(c, dst) - 1);
                    int rot = pdf_to_int(c, pdf_dict_get_inheritable(c, po, PDF_NAME(Rotate)));
                    pdf_dict_put_int(c, po, PDF_NAME(Rotate), (((rot + it.rotate) % 360) + 360) % 360);
                }
            }
            saveNew(c, dst, outPath);
        }
        fz_always(c)
        {
            for (const Src &s : std::as_const(sources)) {
                pdf_drop_graft_map(c, s.map);
                pdf_drop_document(c, s.doc);
            }
            pdf_drop_document(c, dst);
        }
        fz_catch(c) fz_rethrow(c);
    });
}

void Document::noteHistory(fz_context *c)
{
    canUndo_ = pdf_can_undo(c, doc_);
    canRedo_ = pdf_can_redo(c, doc_);
}
void Document::undo()
{
    bool clean = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        fzrun([&](fz_context *c) {
            pdf_undo(c, doc_);
            noteHistory(c);
            int steps = 0;
            clean = pdf_undoredo_state(c, doc_, &steps) == 0;
        });
    }
    touched(-1, true);
    // The undo history starts afresh whenever the file is loaded or saved, and every edit is a step in it (MuPDF
    // refuses changes outside an operation), so its start is the file on disk: back there, there is nothing to save.
    if (clean) setModified(false);
}
void Document::redo()
{
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        fzrun([&](fz_context *c) { pdf_redo(c, doc_); noteHistory(c); });
    }
    touched(-1, true);
}

// ---------------------------------------------------------------- saving
void Document::replaceFile(const QString &tmp, const QString &path)
{
    QString bak = path + QStringLiteral(".bak~");
    QFile::remove(bak);
    bool had = QFile::exists(path);
    if (had && !QFile::rename(path, bak)) {
        QFile::remove(tmp);
        throw PdfError(tr("Cannot replace %1 (is it open in another program?)").arg(path));
    }
    if (!QFile::rename(tmp, path)) {
        if (had) QFile::rename(bak, path);
        QFile::remove(tmp);
        throw PdfError(tr("Cannot write %1").arg(path));
    }
    QFile::remove(bak);
}

bool Document::hasSignatures()
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    bool r = false;
    try {
        fzrun([&](fz_context *c) { r = pdf_count_signatures(c, doc_) > 0 && !data_.isEmpty(); });
    } catch (const PdfError &) {}
    return r;
}

void Document::save(const QString &path, bool optimize)
{
    bool same = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        QString tmp = path + QStringLiteral(".tmp~");
        bool incremental = hasSignatures();
        fzrun([&](fz_context *c) {
            pdf_write_options o = pdf_default_write_options;
            if (incremental) {
                if (!pdf_can_be_saved_incrementally(c, doc_))
                    fz_throw(c, FZ_ERROR_GENERIC, "this signed file is damaged and cannot be saved without invalidating its signatures");
                QFile f(tmp);
                if (!f.open(QIODevice::WriteOnly) || f.write(data_) != data_.size())
                    fz_throw(c, FZ_ERROR_GENERIC, "cannot write temporary file");
                f.close();
                o.do_incremental = 1;
            } else {
                o.do_garbage = optimize ? 4 : 1;
                o.do_compress = 1;
                o.do_compress_images = 1;
                o.do_compress_fonts = 1;
                o.do_use_objstms = optimize ? 1 : 0;
            }
            pdf_save_document(c, doc_, tmp.toUtf8().constData(), &o);
        });
        replaceFile(tmp, path);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) throw PdfError(tr("Cannot re-open %1").arg(path));
        const QVector<QSizeF> sizes = sizes_;
        loadBytes(f.readAll());
        path_ = QFileInfo(path).absoluteFilePath();
        same = sizes_ == sizes;
    }
    // The file just written is the document on screen, so its pages are the same: say that every page may have
    // changed rather than the structure, so the view keeps its pictures, search hits and selection. Every page,
    // because annotation and field ids are object numbers, which an optimised save renumbers (so the page keys
    // may differ too, and are not compared). Should the file read back differ after all, say so.
    if (same)
        emit pageChanged(-1);
    else
        emit structureChanged();
    emit saved();
}

#ifdef PDFABLE_SIGNING
// ---------------------------------------------------------------- digital signatures
namespace {
struct BridgeSigner {
    pdf_pkcs7_signer base;
    int refs;
    const DigitalId *id;
    QString tsa;
    QString error;
};

pdf_pkcs7_signer *bsKeep(fz_context *, pdf_pkcs7_signer *s) { ++reinterpret_cast<BridgeSigner *>(s)->refs; return s; }
void bsDrop(fz_context *, pdf_pkcs7_signer *s)
{
    BridgeSigner *b = reinterpret_cast<BridgeSigner *>(s);
    if (b && --b->refs == 0) delete b;
}
char *dupStr(fz_context *c, const QString &s) { return s.isEmpty() ? nullptr : fz_strdup(c, s.toUtf8().constData()); }
pdf_pkcs7_distinguished_name *bsName(fz_context *c, pdf_pkcs7_signer *s)
{
    const DigitalId *id = reinterpret_cast<BridgeSigner *>(s)->id;
    auto *dn = static_cast<pdf_pkcs7_distinguished_name *>(fz_calloc(c, 1, sizeof(pdf_pkcs7_distinguished_name)));
    dn->cn = dupStr(c, id->cn);
    dn->o = dupStr(c, id->o);
    dn->ou = dupStr(c, id->ou);
    dn->email = dupStr(c, id->email);
    dn->c = dupStr(c, id->c);
    return dn;
}
size_t bsMax(fz_context *, pdf_pkcs7_signer *s)
{
    BridgeSigner *b = reinterpret_cast<BridgeSigner *>(s);
    return size_t(cmsMaxSize(*b->id, !b->tsa.isEmpty()));
}
int bsDigest(fz_context *c, pdf_pkcs7_signer *s, fz_stream *in, unsigned char *digest, size_t len)
{
    BridgeSigner *b = reinterpret_cast<BridgeSigner *>(s);
    if (!in) return int(bsMax(c, s));
    fz_buffer *buf = fz_read_all(c, in, 1 << 16);
    unsigned char *data = nullptr;
    size_t n = fz_buffer_storage(c, buf, &data);
    QByteArray content(reinterpret_cast<const char *>(data), qsizetype(n));
    fz_drop_buffer(c, buf);
    QByteArray der = cmsSign(*b->id, content, b->tsa, &b->error);
    if (der.isEmpty() || size_t(der.size()) > len) {
        if (b->error.isEmpty()) b->error = QStringLiteral("signature does not fit the reserved space");
        fz_throw(c, FZ_ERROR_GENERIC, "%s", b->error.toUtf8().constData());
    }
    std::memcpy(digest, der.constData(), size_t(der.size()));
    return int(der.size());
}
} // namespace

void Document::sign(const SignParams &p, const QString &outPath)
{
    if (!p.id) throw PdfError(tr("No digital ID selected"));
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        // 1. make sure the on-disk bytes are the current document and allow an incremental update
        bool clean = false;
        fzrun([&](fz_context *c) { clean = pdf_can_be_saved_incrementally(c, doc_) != 0; });
        if (modified_ || data_.isEmpty() || !clean) {
            if (!clean && hasSignatures())
                throw PdfError(tr("This file is damaged; signing it would invalidate its existing signatures."));
            save(outPath);
        }
        if (p.page < 0 || p.page >= sizes_.size()) throw PdfError(tr("Invalid page"));

        // 2. add the signature field and value
        BridgeSigner *bs = new BridgeSigner{{bsKeep, bsDrop, bsName, bsMax, bsDigest}, 1, p.id.get(), p.tsaUrl, QString()};
        QString tmp = outPath + QStringLiteral(".tmp~");
        QImage gimg = p.graphic.isNull() ? QImage() : p.graphic.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        QString failure;
        try {
            fzrun([&](fz_context *c) {
                pdf_page *pg = nullptr;
                pdf_annot *w = nullptr;
                fz_pixmap *pix = nullptr;
                fz_image *gi = nullptr;
                fz_var(pg); fz_var(w); fz_var(pix); fz_var(gi);
                fz_try(c)
                {
                    pg = pdf_load_page(c, doc_, p.page);
                    pdf_begin_operation(c, doc_, "Sign");
                    QByteArray fname = "Signature_" + QByteArray::number(QRandomGenerator::global()->generate(), 16);
                    w = pdf_create_signature_widget(c, pg, fname.data());
                    bool visible = p.rect.isValid() && p.rect.width() > 2 && p.rect.height() > 2;
                    pdf_set_annot_rect(c, w, visible ? fr(p.rect) : fz_make_rect(0, 0, 0, 0));
                    if (visible && !gimg.isNull()) {
                        pix = fz_new_pixmap(c, fz_device_rgb(c), gimg.width(), gimg.height(), nullptr, 1);
                        for (int y = 0; y < gimg.height(); ++y)
                            std::memcpy(fz_pixmap_samples(c, pix) + y * fz_pixmap_stride(c, pix), gimg.constScanLine(y),
                                        size_t(gimg.width()) * 4);
                        gi = fz_new_image_from_pixmap(c, pix, nullptr);
                    }
                    QByteArray reason = p.reason.toUtf8(), location = p.location.toUtf8(), cn = p.id->cn.toUtf8();
                    int64_t now = int64_t(QDateTime::currentSecsSinceEpoch());
                    if (visible) {
                        pdf_pkcs7_distinguished_name *dn = bsName(c, &bs->base);
                        char *info = nullptr;
                        fz_display_list *dl = nullptr;
                        fz_var(info); fz_var(dl);
                        fz_try(c)
                        {
                            info = pdf_signature_info(c, cn.constData(), dn, reason.isEmpty() ? nullptr : reason.constData(),
                                                      location.isEmpty() ? nullptr : location.constData(), now, 1);
                            dl = pdf_signature_appearance_signed(c, fr(p.rect), pdf_annot_language(c, w), gi,
                                                                 gi ? nullptr : cn.constData(), info, 0);
                            pdf_set_annot_appearance_from_display_list(c, w, "N", nullptr, fz_identity, dl);
                        }
                        fz_always(c)
                        {
                            fz_drop_display_list(c, dl);
                            fz_free(c, info);
                            pdf_signature_drop_distinguished_name(c, dn);
                        }
                        fz_catch(c) fz_rethrow(c);
                    }

                    // Signature dictionary (PAdES: SubFilter ETSI.CAdES.detached). It must be complete
                    // before it is registered: later edits would land in a newer revision.
                    pdf_obj *field = pdf_annot_obj(c, w);
                    pdf_obj *v = pdf_add_new_dict(c, doc_, 12);
                    pdf_dict_put_drop(c, field, PDF_NAME(V), v);
                    pdf_dict_put(c, v, PDF_NAME(Type), PDF_NAME(Sig));
                    // MuPDF's writer requires the key order ByteRange, Contents, Filter.
                    pdf_dict_put_array(c, v, PDF_NAME(ByteRange), 4);
                    QByteArray zeros(qsizetype(bsMax(c, &bs->base)), '\0');
                    pdf_dict_put_string(c, v, PDF_NAME(Contents), zeros.constData(), size_t(zeros.size()));
                    pdf_dict_put_name(c, v, PDF_NAME(Filter), "Adobe.PPKLite");
                    pdf_dict_put_name(c, v, PDF_NAME(SubFilter), "ETSI.CAdES.detached");
                    pdf_dict_put_date(c, v, PDF_NAME(M), now);
                    pdf_dict_put_text_string(c, v, PDF_NAME(Name), cn.constData());
                    if (!reason.isEmpty()) pdf_dict_put_text_string(c, v, PDF_NAME(Reason), reason.constData());
                    if (!location.isEmpty()) pdf_dict_put_text_string(c, v, PDF_NAME(Location), location.constData());
                    if (!p.contact.isEmpty()) pdf_dict_put_text_string(c, v, PDF_NAME(ContactInfo), p.contact.toUtf8().constData());
                    if (p.certifyLevel >= 1 && p.certifyLevel <= 3) {   // certification signature (DocMDP)
                        pdf_obj *refs = pdf_dict_put_array(c, v, PDF_NAME(Reference), 1);
                        pdf_obj *ref = pdf_new_dict(c, doc_, 3);
                        pdf_array_push_drop(c, refs, ref);
                        pdf_dict_put(c, ref, PDF_NAME(Type), PDF_NAME(SigRef));
                        pdf_dict_put(c, ref, PDF_NAME(TransformMethod), PDF_NAME(DocMDP));
                        pdf_obj *tp = pdf_dict_put_dict(c, ref, PDF_NAME(TransformParams), 3);
                        pdf_dict_put(c, tp, PDF_NAME(Type), PDF_NAME(TransformParams));
                        pdf_dict_put_int(c, tp, PDF_NAME(P), p.certifyLevel);
                        pdf_dict_put_name(c, tp, PDF_NAME(V), "1.2");
                        pdf_obj *root = pdf_dict_get(c, pdf_trailer(c, doc_), PDF_NAME(Root));
                        pdf_dict_put(c, pdf_dict_put_dict(c, root, PDF_NAME(Perms), 1), PDF_NAME(DocMDP), v);
                    }
                    pdf_obj *acro = pdf_dict_get(c, pdf_dict_get(c, pdf_trailer(c, doc_), PDF_NAME(Root)), PDF_NAME(AcroForm));
                    if (acro) pdf_dict_put_int(c, acro, PDF_NAME(SigFlags), 3);
                    pdf_xref_store_unsaved_signature(c, doc_, field, &bs->base);

                    pdf_end_operation(c, doc_);

                    // 3. incremental save: original bytes + appended revision containing the signature
                    QFile f(tmp);
                    if (!f.open(QIODevice::WriteOnly) || f.write(data_) != data_.size())
                        fz_throw(c, FZ_ERROR_GENERIC, "cannot write temporary file");
                    f.close();
                    pdf_write_options o = pdf_default_write_options;
                    o.do_incremental = 1;
                    pdf_save_document(c, doc_, tmp.toUtf8().constData(), &o);
                }
                fz_always(c)
                {
                    fz_drop_image(c, gi);
                    fz_drop_pixmap(c, pix);
                    pdf_drop_annot(c, w);
                    fz_drop_page(c, reinterpret_cast<fz_page *>(pg));
                }
                fz_catch(c) fz_rethrow(c);
            });
        } catch (const PdfError &e) {
            failure = bs->error.isEmpty() ? e.message() : bs->error;
        }
        bsDrop(fzctx(), &bs->base);
        if (!failure.isEmpty()) {
            QFile::remove(tmp);
            // drop the half-made signature field by reloading the last saved state
            QByteArray keep = data_;
            QString pth = path_;
            loadBytes(keep);
            path_ = pth;
            emit structureChanged();
            throw PdfError(failure);
        }
        replaceFile(tmp, outPath);
        QFile f(outPath);
        if (!f.open(QIODevice::ReadOnly)) throw PdfError(tr("Cannot re-open %1").arg(outPath));
        loadBytes(f.readAll());
        path_ = QFileInfo(outPath).absoluteFilePath();
    }
    emit structureChanged();
}

QVector<SignatureInfo> Document::signatures(const QList<QByteArray> &extraTrusted)
{
    std::lock_guard<std::recursive_mutex> lk(mu_);
    QVector<SignatureInfo> out;
    if (!doc_ || data_.isEmpty()) return out;
    struct Raw { SignatureInfo si; QByteArray cms, content; };
    QVector<Raw> raws;
    try {
        fzrun([&](fz_context *c) {
            int n = pdf_count_pages(c, doc_);
            for (int i = 0; i < n; ++i) {
                pdf_obj *annots = pdf_dict_get(c, pdf_lookup_page_obj(c, doc_, i), PDF_NAME(Annots));
                bool any = false;
                for (int k = 0; k < pdf_array_len(c, annots) && !any; ++k)
                    any = pdf_name_eq(c, pdf_dict_get(c, pdf_array_get(c, annots, k), PDF_NAME(Subtype)), PDF_NAME(Widget));
                if (!any) continue;
                pdf_page *pg = pdf_load_page(c, doc_, i);
                fz_try(c)
                {
                    for (pdf_annot *w = pdf_first_widget(c, pg); w; w = pdf_next_widget(c, w)) {
                        if (pdf_widget_type(c, w) != PDF_WIDGET_TYPE_SIGNATURE || !pdf_widget_is_signed(c, w)) continue;
                        pdf_obj *field = pdf_annot_obj(c, w);
                        pdf_obj *v = pdf_dict_get_inheritable(c, field, PDF_NAME(V));
                        Raw r;
                        r.si.page = i;
                        r.si.rect = qr(pdf_bound_annot(c, w));
                        r.si.fieldName = QString::fromUtf8(pdf_dict_get_text_string(c, field, PDF_NAME(T)));
                        r.si.subFilter = QString::fromUtf8(pdf_to_name(c, pdf_dict_get(c, v, PDF_NAME(SubFilter))));
                        r.si.name = QString::fromUtf8(pdf_dict_get_text_string(c, v, PDF_NAME(Name)));
                        r.si.reason = QString::fromUtf8(pdf_dict_get_text_string(c, v, PDF_NAME(Reason)));
                        r.si.location = QString::fromUtf8(pdf_dict_get_text_string(c, v, PDF_NAME(Location)));
                        int64_t t = pdf_dict_get_date(c, v, PDF_NAME(M));
                        if (t > 0) r.si.claimedTime = QDateTime::fromSecsSinceEpoch(t);
                        pdf_obj *tp = pdf_dict_get(c, pdf_array_get(c, pdf_dict_get(c, v, PDF_NAME(Reference)), 0), PDF_NAME(TransformParams));
                        if (tp) r.si.docMdp = pdf_dict_get_int(c, tp, PDF_NAME(P));
                        r.si.isDocTimestamp = r.si.subFilter == QLatin1String("ETSI.RFC3161");

                        fz_range ranges[8];
                        int nr = pdf_signature_byte_range(c, doc_, field, nullptr);
                        if (nr > 0 && nr <= 8) {
                            pdf_signature_byte_range(c, doc_, field, ranges);
                            int64_t end = 0;
                            for (int k = 0; k < nr; ++k) {
                                int64_t off = ranges[k].offset, len = int64_t(ranges[k].length);
                                if (off < 0 || len < 0 || off + len > data_.size()) { r.content.clear(); end = -1; break; }
                                r.content += data_.mid(qsizetype(off), qsizetype(len));
                                end = off + len;
                            }
                            r.si.coversWholeFile = end == data_.size();
                        }
                        char *contents = nullptr;
                        size_t cl = pdf_signature_contents(c, doc_, field, &contents);
                        r.cms = QByteArray(contents, qsizetype(cl));
                        fz_free(c, contents);
                        raws.push_back(r);
                    }
                }
                fz_always(c) fz_drop_page(c, reinterpret_cast<fz_page *>(pg));
                fz_catch(c) fz_rethrow(c);
            }
        });
    } catch (const PdfError &) {}

    for (Raw &r : raws) {
        VerifyResult vr = r.si.isDocTimestamp ? tsTokenInfo(r.cms) : cmsVerify(r.cms, r.content, r.si.claimedTime, extraTrusted);
        r.si.parsed = vr.parsed;
        r.si.digestOk = vr.digestOk;
        r.si.trusted = vr.trusted;
        r.si.selfSigned = vr.selfSigned;
        r.si.hasTimestamp = vr.hasTimestamp;
        r.si.signerCn = vr.signerCn;
        r.si.issuerCn = vr.issuerCn;
        r.si.error = vr.error;
        r.si.trustError = vr.trustError;
        r.si.timestampTime = vr.timestampTime;
        r.si.notBefore = vr.notBefore;
        r.si.notAfter = vr.notAfter;
        r.si.certDer = vr.certDer;
        out.push_back(r.si);
    }
    return out;
}

#else
void Document::sign(const SignParams &, const QString &) { throw PdfError(tr("This build has no certificate signing.")); }
QVector<SignatureInfo> Document::signatures(const QList<QByteArray> &) { return {}; }
#endif

} // namespace ink
