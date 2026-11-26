// Document: thread-safe wrapper around a MuPDF pdf_document.
// All coordinates are in PDF points in MuPDF "page space" (origin top-left, y down).
#pragma once
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVector>
#include <memory>
#include <mutex>
#include <stdexcept>

struct fz_context;
struct pdf_document;
struct pdf_page;
struct pdf_annot;

namespace ink {

struct DigitalId;

class PdfError : public std::runtime_error {
public:
    explicit PdfError(const QString &m) : std::runtime_error(m.toStdString()) {}
    QString message() const { return QString::fromStdString(what()); }
};

struct OutlineItem {
    QString title;
    int page = -1;
    float y = 0;
    QVector<OutlineItem> children;
};

struct LinkInfo {
    QRectF rect;
    QString uri;
    bool external = false;
    int page = -1;      // destination page (internal links)
    QPointF dest;       // destination point on that page
};

struct InkStroke {
    QVector<QPointF> pts;
    QVector<float> w;   // per-point width in points; empty = uniform
};

struct InkData {
    QVector<InkStroke> strokes;
    QColor color = Qt::black;
    float width = 1.5f;      // nominal width in points
    bool highlighter = false;
    QRectF bounds() const;
    void transform(const QTransform &t, qreal widthScale);
};

enum class AnnotKind { Ink, FreeText, Stamp, Highlight, Underline, StrikeOut, Other };

struct AnnotInfo {
    int id = 0;
    AnnotKind kind = AnnotKind::Other;
    QRectF rect;
    QString text;        // FreeText: its text; markups: the attached note
    float fontSize = 0;  // FreeText only
    QColor color;        // FreeText: text colour; markups: highlight colour
    QVector<QPolygonF> quads;   // markups (highlight, underline, strike-out)
};

// A text marking (highlight, underline, strike-out) and the note attached to it.
struct MarkupNote {
    int page = 0;
    int id = 0;
    AnnotKind kind = AnnotKind::Highlight;
    QRectF rect;
    QVector<QPolygonF> quads;
    QColor color;
    QString text;
};

// Interactive form field (AcroForm widget).
enum class FieldKind { Text, CheckBox, Radio, Combo, List, Button, Signature, Other };
struct FieldInfo {
    int id = 0;
    FieldKind kind = FieldKind::Other;
    QRectF rect;
    QString value;
    QStringList options;     // combo / list boxes
    bool multiline = false, readonly = false, checked = false;
    int maxLen = 0;
    float fontSize = 0;      // 0 = auto
};

struct InkAnnot {
    int id = 0;
    InkData data;
};

// One output page of the PDF creator: a page of a PDF, or an image file that becomes a page.
struct AssemblyItem {
    QString path;
    int page = 0;        // ignored for images
    int rotate = 0;      // extra clockwise rotation in degrees (multiple of 90)
    bool image = false;
};

struct SignParams {
    std::shared_ptr<DigitalId> id;
    int page = 0;
    QRectF rect;            // empty = invisible signature
    QString reason, location, contact;
    QString tsaUrl;         // RFC 3161 timestamp authority (optional)
    int certifyLevel = 0;   // 0 = approval signature, 1..3 = DocMDP permission level
    QImage graphic;         // optional handwritten signature shown in the box
};

struct SignatureInfo {
    QString fieldName, subFilter, name, reason, location;
    QDateTime claimedTime;
    int page = -1;
    QRectF rect;
    int docMdp = 0;
    bool coversWholeFile = false;
    bool isDocTimestamp = false;
    // crypto result
    bool parsed = false, digestOk = false, trusted = false, selfSigned = false, hasTimestamp = false;
    QString signerCn, issuerCn, error, trustError;
    QDateTime timestampTime, notBefore, notAfter;
    QByteArray certDer;
};

fz_context *fzctx();   // per-thread MuPDF context

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject *parent = nullptr);
    ~Document() override;

    // Opening. Throws PdfError. If needsPassword() is true afterwards call authenticate().
    void open(const QString &path);
    void createEmpty();
    bool needsPassword() const { return needsPw_; }
    bool authenticate(const QString &password);
    // Re-read the file if another program rewrote it (e.g. LaTeX). Returns true if reloaded.
    bool reloadIfChanged();

    QString path() const { return path_; }
    bool isModified() const { return modified_; }
    int pageCount() const { return sizes_.size(); }
    QSizeF pageSize(int page) const { return sizes_.value(page); }
    quint64 generation() const { return gen_; }
    // Non-blocking access for the pen path: never stall the GUI behind a long render.
    bool tryLock() { return mu_.try_lock(); }
    void unlock() { mu_.unlock(); }

    // Rendering / reading (callable from any thread)
    QImage render(int page, double scale, const QRect &clipPx);
    QVector<OutlineItem> outline();
    QVector<LinkInfo> links(int page);
    QVector<QPolygonF> selectionQuads(int page, QPointF a, QPointF b);
    QString selectionText(int page, QPointF a, QPointF b);
    QVector<QPolygonF> search(int page, const QString &needle);
    QVector<AnnotInfo> annots(int page);
    QVector<MarkupNote> markups();                       // whole document, quick (no page loading)
    QString textInQuads(int page, const QVector<QPolygonF> &quads);
    QVector<FieldInfo> fields(int page);
    QVector<InkAnnot> inkAnnots(int page);

    // Annotation editing (GUI thread). Each call is one undo step.
    int addInk(int page, const InkData &d);
    void setInk(int page, int id, const InkData &d);
    int addMarkup(int page, AnnotKind kind, const QVector<QPolygonF> &quads, const QColor &c);
    int addFreeText(int page, const QRectF &r, const QString &text, float size, const QColor &c);
    void setFreeText(int page, int id, const QString &text, const QRectF &rect = QRectF());
    int addStamp(int page, const QRectF &r, const QImage &img);
    void setAnnotRect(int page, int id, const QRectF &r);
    void scaleFreeText(int page, int id, const QRectF &r, float fontSize);   // keeps font and colour
    void removeAnnots(int page, const QVector<int> &ids);
    void setAnnotContents(int page, int id, const QString &text);

    // Form filling
    void setFieldText(int page, int id, const QString &text);
    void toggleField(int page, int id);
    void setFieldChoice(int page, int id, const QString &option);

    // Page editing
    void rearrange(const QVector<int> &order);          // subset/permutation of current pages
    void rotatePages(const QVector<int> &pages, int delta);
    void insertBlank(int at, QSizeF size);
    int insertPdf(const QString &path, int at);         // returns number of pages inserted
    void extractPages(const QVector<int> &pages, const QString &outPath);
    static void mergeFiles(const QStringList &paths, const QString &outPath);
    static void assemble(const QVector<AssemblyItem> &items, const QString &outPath);

    bool canUndo() const { return canUndo_; }
    bool canRedo() const { return canRedo_; }
    void undo();
    void redo();

    // Saving. Signed documents are saved incrementally so existing signatures stay valid.
    void save(const QString &path, bool optimize = false);
    bool hasSignatures();
    void sign(const SignParams &p, const QString &outPath);
    QVector<SignatureInfo> signatures(const QList<QByteArray> &extraTrusted);

signals:
    void pageChanged(int page);      // -1 = all pages
    void structureChanged();         // page count/size/order changed, or reloaded
    void saved();                    // written to path(), which may be new; the pages are unchanged
    void modifiedChanged(bool);

private:
    void close();
    void loadBytes(const QByteArray &bytes);
    void loadSizes();
    void touched(int page, bool structure = false);
    void setModified(bool m);
    template <class F> void pageOp(int page, const char *name, F &&f);
    template <class F> void docOp(const char *name, F &&f);
    void *stext(fz_context *c, int page);
    void dropStext(int page = -1);
    void replaceFile(const QString &tmp, const QString &path);

    mutable std::recursive_mutex mu_;
    pdf_document *doc_ = nullptr;
    QByteArray data_;
    QString path_, password_;
    QVector<QSizeF> sizes_;
    QVector<int> pageKeys_;          // object number and rotation of each page, in turn
    QHash<int, void *> stext_;
    bool modified_ = false, needsPw_ = false;
    // Noted while the document is locked anyway: asking MuPDF from the GUI, which greys Undo and Redo out
    // after every edit, could wait behind a long render.
    bool canUndo_ = false, canRedo_ = false;
    void noteHistory(fz_context *c);
    quint64 gen_ = 0;
};

} // namespace ink
