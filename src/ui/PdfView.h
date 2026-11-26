// PdfView: continuous page view with pen input, text selection, links with hover
// previews, annotation editing and search highlighting.
#pragma once
#include "Document.h"
#include "RenderWorker.h"
#include "Stroke.h"
#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

class QLabel;
class QFrame;
class QPlainTextEdit;
class QTouchEvent;

enum class Tool { Select, Hand, Pen, Highlighter, Eraser, Text };

struct PenStyle {
    QColor color = QColor(0x1a, 0x1a, 0x1a);
    float width = 1.6f;   // points
};

// Line from a marking to its note in the Notes sidebar (drawn from the marking to the left edge).
struct NoteLink {
    int page = 0;
    QPointF anchor;      // page coordinates: bottom-left corner of the marking's first line
    QColor color;
};

struct ViewState {
    double zoom = 0;   // 0 = unknown
    int page = 0;
    double y = 0;      // offset inside the page, in points
};

class PdfView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PdfView(std::shared_ptr<ink::Document> doc, QWidget *parent = nullptr);
    ~PdfView() override;

    ink::Document *document() const { return doc_.get(); }
    RenderWorker *worker() const { return worker_; }

    // The pen and the mouse each have their own tool; a finger only pans and zooms.
    void setTools(Tool pen, Tool mouse);
    void setTool(Tool t) { setTools(t, t); }
    Tool tool() const { return tool_; }
    Tool penTool() const { return toolPen_; }
    Tool mouseTool() const { return toolMouse_; }
    void setHighlightPalette(const QVector<QColor> &c) { hlPalette_ = c; }
    void setPenStyle(const PenStyle &s) { pen_ = s; }
    void setHighlighterStyle(const PenStyle &s) { hl_ = s; }
    PenStyle penStyle() const { return pen_; }
    PenStyle highlighterStyle() const { return hl_; }
    void setTextSize(float pt) { textSize_ = pt; }
    bool isEditingText() const { return textEdit_ != nullptr; }
    bool isEditingField() const { return fieldEdit_ != nullptr; }
    QPointF pageToView(int page, QPointF pt) const { return pageViewRect(page).topLeft() + pt * ppp(); }   // viewport coordinates
    QWidget *fieldEditor() const { return fieldEdit_; }
    void commitTextEdit();

    double zoom() const { return zoom_; }
    void setZoom(double z, QPointF anchor = QPointF(-1, -1));
    void fitWidth();
    void fitPage();
    int currentPage() const;
    void gotoPage(int page, double y = 0, bool history = true);
    void goBack();
    void goForward();
    ViewState viewState() const;
    void setViewState(const ViewState &s);
    bool canGoBack() const { return !back_.isEmpty(); }
    bool canGoForward() const { return !fwd_.isEmpty(); }
    static void setPageTheme(PageTheme t);       // applies to every open view
    static void setScrollSpeed(double factor);   // 1.0 = system default wheel step
    static double scrollSpeed();
    void copyAreaAsImage();                // drag a rectangle, get it on the clipboard

    // search
    void setSearch(const QString &text);
    void findNext(bool forward);
    int searchCount() const { return searchTotal_; }
    bool searching() const { return searchTimer_.isActive(); }

    // selection
    bool hasTextSelection() const { return selPage_ >= 0 && !selQuads_.isEmpty(); }
    QString selectedText();
    void copySelection();
    int markupSelection(ink::AnnotKind kind, const QColor &c);   // returns the new annotation id (0 = none)
    static void setNoteAfterMarking(bool on);    // open the note editor right after marking text
    static bool noteAfterMarking();
    void setNoteLinks(const QVector<NoteLink> &links) { noteLinks_ = links; viewport()->update(); }
    void setNoteLinksVisible(bool on) { if (on != noteLinksVisible_) { noteLinksVisible_ = on; viewport()->update(); } }
    void deleteSelectedAnnot();

    // modes started from dialogs
    void startPlacingInk(const ink::InkData &d, double widthPt);
    void startPlacingImage(const QImage &img);
    void startRectPick(std::function<void(int, QRectF)> cb, const QString &hint = QString());
    void cancelModes();

    // Commit queued pen strokes to the document (call before save/undo/page edits).
    void flushInk();

    // test hooks: feed synthetic pointer input in viewport coordinates
    void testPointer(int phase, QPointF pos, float pressure, qint64 t, bool pen = false);
    bool testHold();

signals:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    void searchChanged(int total, int current);
    void statusMessage(const QString &msg);
    void toolChangeRequested(Tool t);
    void deviceUsed(bool pen);       // a pen or the mouse started an interaction on the page
    void historyChanged();
    void viewChanged();                          // scrolled, zoomed or re-laid out
    void noteRequested(int page, int annotId);   // open the note of a marking

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    bool viewportEvent(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void leaveEvent(QEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    struct Ptr {
        QPointF pos;
        float pressure = -1;   // < 0: device without pressure
        bool eraser = false;
        Qt::MouseButton button = Qt::LeftButton;
        qint64 t = 0;
        bool pen = false;
    };
    struct Tile { QImage img; double scale = 0; QRect clip; quint64 gen = 0; };
    struct Pending { int page; ink::InkData d; quint64 gen; bool committed; };
    enum class Drag { None, Pan, Draw, Erase, TextSel, AnnotMove, AnnotResize, RectPick, LinkClick };

    double ppp() const { return zoom_ * 96.0 / 72.0; }   // logical pixels per PDF point
    QPointF offset() const;
    void relayout();
    void updateScrollBars();
    int pageAt(QPointF viewPos, bool nearest = false) const;
    QPointF toPage(int page, QPointF viewPos) const;
    QRectF pageViewRect(int page) const;
    QRectF toView(int page, const QRectF &r) const;
    void pagePainter(QPainter &p, int page) const;
    void requestTiles();
    void onRendered(const RenderJob &job, const QImage &img);
    void onPageChanged(int page);
    void onStructureChanged();

    void press(const Ptr &e);
    void move(const Ptr &e);
    void release(const Ptr &e);
    void hover(QPointF pos, bool pen = false);
    void handleTouch(QTouchEvent *e);
    void scrollBy(QPointF d);
    void updateCursor(Tool t);
    void showSelBar(QPointF viewPos);
    void hideSelBar();
    QPair<int, double> currentPos() const;
    double docY(const QPair<int, double> &p) const;
    void record(const QPair<int, double> &p);
    void onSettle();
    void jumpTo(const QPair<int, double> &p);
    void beginStroke(const Ptr &e, bool highlighter);
    void extendStroke(const Ptr &e);
    void endStroke();
    void rebuildOverlay();
    void paintLiveSegments(int from);
    void tryCommit(bool block);
    void eraseAt(int page, QPointF pt);
    void onHold();
    const QVector<ink::LinkInfo> &linksFor(int page);
    const ink::LinkInfo *linkAt(int page, QPointF pt);
    void showPreview();
    void hidePreview();
    bool annotAt(int page, QPointF pt, ink::AnnotInfo *out);
    void selectAnnot(int page, const ink::AnnotInfo &a);
    void clearAnnotSelection();
    void applyAnnotDrag(bool resized);
    QRectF handleRect(int corner, const QRectF &viewRect) const;
    void beginTextEdit(int page, QPointF pt, const ink::AnnotInfo *existing);
    void cancelTextEdit();
    void layoutTextEdit();
    void pollPreview();
    void runSearchChunk();
    bool searchPage(int page);     // false: it was searched already
    void showHit();
    void searchInView();
    void pushHistory();

    std::shared_ptr<ink::Document> doc_;
    RenderWorker *worker_;
    Tool tool_ = Tool::Select, toolPen_ = Tool::Pen, toolMouse_ = Tool::Select;
    bool lastWasPen_ = false;
    QVector<QColor> hlPalette_;
    QFrame *selBar_ = nullptr;
    QVector<NoteLink> noteLinks_;
    bool noteLinksVisible_ = false;
    int pressMarkupPage_ = -1, pressMarkupId_ = 0;
    PenStyle pen_, hl_{QColor(255, 235, 60), 12.f};
    double zoom_ = 1.0;
    QVector<QRectF> pageRects_;
    QSizeF docSize_;
    QHash<int, Tile> tiles_;
    QHash<int, RenderJob> requested_;
    int lastCurrent_ = -1;

    // pointer state
    Drag drag_ = Drag::None;
    QPointF pressPos_, lastPos_;
    QPoint panScroll_;
    bool penDown_ = false;
    QElapsedTimer clock_;

    // finger: pan, pinch-zoom and fling
    int touchCount_ = 0;
    QPointF touchLast_, flingV_;
    qreal pinchDist_ = 0;
    QVector<QPair<qint64, QPointF>> touchTrail_;
    QTimer flingTimer_;

    // inking
    ink::StrokeBuilder builder_;
    int drawPage_ = -1;
    bool drawHighlighter_ = false;
    int liveDrawn_ = 0;
    QImage overlay_;
    QVector<Pending> pending_;
    QTimer commitTimer_, holdTimer_;
    QPointF holdAnchor_;
    QRectF lastDirty_;
    int erasePage_ = -1;
    quint64 eraseGen_ = 0;
    QVector<ink::InkAnnot> eraseCache_;

    // text selection
    int selPage_ = -1;
    QPointF selA_, selB_;
    QVector<QPolygonF> selQuads_;

    // annotation selection
    int annPage_ = -1;
    ink::AnnotInfo ann_;
    QRectF annDragRect_;
    int annCorner_ = -1;
    QPointF annGrab_;

    // links
    QHash<int, QVector<ink::LinkInfo>> linkCache_;
    ink::LinkInfo hoverLink_;
    bool hasHoverLink_ = false;
    QPoint hoverGlobal_;
    QTimer previewTimer_;
    QLabel *preview_ = nullptr;          // small tip for external links
    QFrame *previewFrame_ = nullptr;     // interactive peek at the link target
    PdfView *previewView_ = nullptr;
    QTimer previewPoll_;
    int previewMiss_ = 0;
    bool isPreview_ = false;

    // in-place text box editor
    QPlainTextEdit *textEdit_ = nullptr;

    // form fields
    QHash<int, QVector<ink::FieldInfo>> fieldCache_;
    QWidget *fieldEdit_ = nullptr;
    int fieldPage_ = -1, fieldId_ = 0;
    QRectF fieldRect_;
    QString fieldOld_;
    const QVector<ink::FieldInfo> &fieldsFor(int page);
    const ink::FieldInfo *fieldAt(int page, QPointF pt);
    void activateField(int page, const ink::FieldInfo &f);
    void beginFieldEdit(int page, const ink::FieldInfo &f);
    void layoutFieldEdit();
    void cancelFieldEdit();
    void focusNextField(int step);
public:
    void commitFieldEdit();
private:
    int textPage_ = -1, textId_ = 0;
    QPointF textAt_, textDelta_;
    float textSize_ = 12, textEditSize_ = 12;
    QColor textEditColor_;
    ink::LinkInfo pressLink_;

    // placing / rect picking
    bool placing_ = false;
    ink::InkData placeInk_;
    QImage placeImage_;
    QSizeF placeSize_;
    QPointF placePos_;
    std::function<void(int, QRectF)> rectCb_;
    int rectPage_ = -1;
    QRectF rectPick_;

    // search
    QString needle_;
    QHash<int, QVector<QPolygonF>> hits_;
    QSet<int> searched_;   // pages searched for needle_ so far; those in view are searched out of turn
    QPair<int, double> searchAnchor_{0, 0};      // page and height where the search for the current term starts
    int searchNext_ = 0, searchTotal_ = 0, hitPage_ = -1, hitIndex_ = -1;
    QTimer searchTimer_;

    // history
    QVector<QPair<int, double>> back_, fwd_;
    QPair<int, double> settled_{0, 0};
    QTimer settleTimer_;
};
