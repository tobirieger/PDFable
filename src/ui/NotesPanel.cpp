#include "NotesPanel.h"
#include "PdfView.h"

#include <QContextMenuEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using namespace ink;

static const int kSide = 10;    // card margin left/right inside the panel
static const int kGapY = 8;     // minimum gap between cards
static const int kMaxLines = 8; // a longer note scrolls inside its card

// Lines of notes that are not lit: a grey that reads on light, sepia and dark paper.
static const QColor kIdleLine(128, 128, 128, 110);

// Height of plain text wrapped to a width, laid out the way QPlainTextEdit does it (which does
// not report a height in pixels itself).
static int textHeight(const QString &text, const QFont &font, int width)
{
    qreal h = 0;
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    const QStringList paragraphs = text.split('\n');
    for (const QString &para : paragraphs) {
        QTextLayout tl(para, font);
        tl.setTextOption(opt);
        tl.beginLayout();
        for (QTextLine line = tl.createLine(); line.isValid(); line = tl.createLine()) {
            line.setLeadingIncluded(true);
            line.setLineWidth(width);
            h += line.height();
        }
        tl.endLayout();
    }
    return int(std::ceil(h));
}

// ------------------------------------------------------------------ NoteCard
NoteCard::NoteCard(const MarkupNote &n, const QString &excerpt, QWidget *parent) : QFrame(parent), note_(n)
{
    setCursor(Qt::PointingHandCursor);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 7, 9, 8);
    lay->setSpacing(4);
    excerpt_ = new QLabel(this);
    excerpt_->setWordWrap(true);
    QFont f = excerpt_->font();
    f.setItalic(true);
    f.setPointSizeF(f.pointSizeF() * 0.9);
    excerpt_->setFont(f);
    excerpt_->setForegroundRole(QPalette::PlaceholderText);
    QString ex = excerpt.size() > 140 ? excerpt.left(137) + QStringLiteral("…") : excerpt;
    excerpt_->setText(ex.isEmpty() ? QString() : QStringLiteral("“%1”").arg(ex));
    excerpt_->setVisible(!ex.isEmpty());
    edit_ = new QPlainTextEdit(this);
    edit_->setPlaceholderText(tr("Type a note…  (Ctrl+Enter to finish, Esc to cancel)"));
    edit_->setFrameShape(QFrame::NoFrame);
    edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit_->setStyleSheet("QPlainTextEdit{background:transparent;border:none;padding:0;}");
    edit_->document()->setDocumentMargin(0);
    edit_->installEventFilter(this);
    meta_ = new QLabel(this);
    f.setItalic(false);
    meta_->setFont(f);
    meta_->setForegroundRole(QPalette::PlaceholderText);
    meta_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);      // a long name is cut off, the card keeps its width
    lay->addWidget(excerpt_);
    lay->addWidget(edit_);
    lay->addWidget(meta_);
    connect(edit_, &QPlainTextEdit::textChanged, this, [this] {
        if (!editing_) return;
        fitted_ = 0;
        fitWidth(width());
        emit heightChanged();
    });
    setEditing(false);
    setNote(n);
}

void NoteCard::setNote(const MarkupNote &n)
{
    note_ = n;
    if (!editing_ && edit_->toPlainText() != n.text) {      // reload() passes every note, changed or not
        edit_->setPlainText(n.text);
        fitted_ = 0;
    }
    QStringList meta;
    if (!n.author.isEmpty()) meta << n.author;
    if (n.modified.isValid()) meta << QLocale().toString(n.modified.toLocalTime().date(), QLocale::ShortFormat);
    const QString line = meta.join(QStringLiteral(" · "));
    if (line != meta_->text() || meta_->isHidden() != line.isEmpty()) {
        meta_->setText(line);
        meta_->setToolTip(n.modified.isValid() ? QLocale().toString(n.modified.toLocalTime(), QLocale::LongFormat) : QString());
        meta_->setVisible(!line.isEmpty());
        fitted_ = 0;
    }
    update();
}

void NoteCard::fitWidth(int w)
{
    if (w == fitted_) return;       // called for every card whenever the view scrolls
    fitted_ = w;
    setFixedWidth(w);
    const QMargins m = layout()->contentsMargins();
    const int textW = w - m.left() - m.right(), line = edit_->fontMetrics().lineSpacing();
    // an empty note shows the placeholder, which needs its room too
    const QString text = edit_->document()->isEmpty() ? edit_->placeholderText() : edit_->toPlainText();
    const int h = textHeight(text, edit_->font(), textW);
    const bool scroll = h > kMaxLines * line;
    edit_->setVerticalScrollBarPolicy(scroll ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    edit_->setFixedHeight(scroll ? kMaxLines * line : h + 1);    // + 1: QPlainTextEdit scrolls unless a pixel is left over
    // without an excerpt nothing in the card wraps, and the layout has no height for a width
    setFixedHeight(layout()->hasHeightForWidth() ? layout()->heightForWidth(w) : layout()->sizeHint().height());
}

void NoteCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(isLit() ? noteLineColor(note_.color) : palette().color(QPalette::Mid), isLit() ? 1.5 : 1));
    p.setBrush(palette().color(QPalette::Base));
    p.drawRoundedRect(r, 7, 7);
    p.setPen(Qt::NoPen);
    p.setBrush(note_.color);
    p.drawRoundedRect(QRectF(r.left() + 3, r.top() + 6, 4, r.height() - 12), 2, 2);   // colour of the marking
}

// Not editing: the text is only shown, and a click anywhere on the card starts editing.
void NoteCard::setEditing(bool on)
{
    editing_ = on;
    edit_->setReadOnly(!on);
    edit_->setTextInteractionFlags(on ? Qt::TextEditorInteraction : Qt::NoTextInteraction);
    edit_->setFocusPolicy(on ? Qt::StrongFocus : Qt::NoFocus);
    edit_->setContextMenuPolicy(on ? Qt::DefaultContextMenu : Qt::NoContextMenu);
    edit_->viewport()->setCursor(on ? Qt::IBeamCursor : Qt::PointingHandCursor);
    fitted_ = 0;
    update();
}

void NoteCard::setLit(bool on)
{
    if (on == lit_) return;
    lit_ = on;
    update();
}

void NoteCard::enterEvent(QEnterEvent *e)
{
    emit hovered(true);
    QFrame::enterEvent(e);
}

void NoteCard::leaveEvent(QEvent *e)
{
    emit hovered(false);
    QFrame::leaveEvent(e);
}

void NoteCard::startEditing()
{
    if (!editing_) {
        setEditing(true);
        fitWidth(width());
        emit heightChanged();
        emit editingStarted();
    }
    edit_->setFocus();
    edit_->moveCursor(QTextCursor::End);
}

void NoteCard::finish(bool keep)
{
    if (!editing_) return;
    QString text = edit_->toPlainText().trimmed();
    setEditing(false);
    edit_->clearFocus();
    edit_->setPlainText(note_.text);
    fitWidth(width());
    if (keep && text != note_.text) emit committed(note_.page, note_.id, text);
    else emit cancelled(note_.id);
}

bool NoteCard::eventFilter(QObject *o, QEvent *e)
{
    if (o == edit_ && editing_) {
        if (e->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (ke->key() == Qt::Key_Escape) { finish(false); return true; }
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && (ke->modifiers() & Qt::ControlModifier)) {
                finish(true);
                return true;
            }
        }
        if (e->type() == QEvent::FocusOut && static_cast<QFocusEvent *>(e)->reason() != Qt::PopupFocusReason) {
            QPointer<NoteCard> self(this);
            QTimer::singleShot(0, this, [self] { if (self) self->finish(true); });
        }
    }
    return QFrame::eventFilter(o, e);
}

void NoteCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) startEditing();
    QFrame::mousePressEvent(e);
}

void NoteCard::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu m(this);
    m.addAction(tr("Edit note"), this, &NoteCard::startEditing);
    m.addAction(tr("Delete note"), this, [this] {
        finish(false);
        emit committed(note_.page, note_.id, QString());
    });
    m.addAction(tr("Delete marking and note"), this, [this] { emit deleteMarking(note_.page, note_.id); });
    m.exec(e->globalPos());
}

// ------------------------------------------------------------------ NotesPanel
NotesPanel::NotesPanel(std::shared_ptr<Document> doc, PdfView *view, QWidget *parent)
    : QWidget(parent), doc_(std::move(doc)), view_(view)
{
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setMinimumWidth(170);
    connect(view_, &PdfView::viewChanged, this, &NotesPanel::relayout);
    for (QToolButton **hint : {&above_, &below_}) {
        auto *b = *hint = new QToolButton(this);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet("QToolButton{background:palette(base);border:1px solid palette(mid);border-radius:9px;padding:2px 10px;}"
                         "QToolButton:hover{border-color:palette(highlight);}");
        b->hide();
        connect(b, &QToolButton::clicked, this, [this, b] {
            for (const MarkupNote &m : std::as_const(notes_)) {
                if (m.id != (b == above_ ? aboveId_ : belowId_)) continue;
                view_->gotoPage(m.page, qMax(0.0, lineYOf(m) - 60));
                view_->selectAnnot(m.page, m.id);       // lit: which of the notes now in view it went to
                break;
            }
        });
    }
    connect(view_, &PdfView::noteHovered, this, [this](int id) { hoveredId_ = id; updateLit(); });
    connect(view_, &PdfView::noteSelected, this, [this](int id) { selectedId_ = id; updateLit(); });
    connect(view_, &PdfView::notePlaced, this, [this](int id) { placedId_ = id; });
}

int NotesPanel::noteCount() const
{
    int n = 0;
    for (const MarkupNote &m : notes_) n += m.text.isEmpty() ? 0 : 1;
    return n;
}

QString NotesPanel::excerptFor(const MarkupNote &n)
{
    if (n.kind == AnnotKind::Note || n.kind == AnnotKind::Ink) return QString();     // these mark no text
    auto it = excerpts_.find(n.id);
    if (it == excerpts_.end()) it = excerpts_.insert(n.id, doc_->textInQuads(n.page, n.quads));
    return it.value();
}

void NotesPanel::reload()
{
    QVector<MarkupNote> all = doc_->markups(editingId_);
    lineYs_.clear();        // the text of a page may have changed
    notes_.clear();
    QSet<int> keep;
    for (const MarkupNote &m : std::as_const(all)) {
        if (m.text.isEmpty() && m.id != editingId_) continue;
        notes_.push_back(m);
        keep.insert(m.id);
        NoteCard *c = cards_.value(m.id);
        if (!c) {
            c = new NoteCard(m, excerptFor(m), this);
            cards_.insert(m.id, c);
            connect(c, &NoteCard::heightChanged, this, &NotesPanel::relayout);
            // the note being written is the selected one: a marking selected before must not stay lit
            connect(c, &NoteCard::editingStarted, this, [this, c] { view_->selectAnnot(c->note().page, c->note().id); });
            connect(c, &NoteCard::hovered, this, [this, id = m.id](bool on) {
                if (!on && hoveredId_ != id) return;
                hoveredId_ = on ? id : 0;
                updateLit();
            });
            c->setLit(m.id == hoveredId_ || m.id == selectedId_);
            connect(c, &NoteCard::committed, this, [this](int page, int id, const QString &text) {
                if (editingId_ == id) editingId_ = 0;
                NoteCard *c = cards_.value(id);
                try {
                    view_->flushInk();
                    // a sticky note is nothing without its text
                    if (text.isEmpty() && c && c->note().kind == AnnotKind::Note) doc_->removeAnnots(page, {id});
                    else doc_->setAnnotContents(page, id, text);
                } catch (const PdfError &) {}
                if (placedId_ == id) placedId_ = 0;
                reload();
            });
            connect(c, &NoteCard::cancelled, this, [this](int id) {
                if (editingId_ == id) editingId_ = 0;
                NoteCard *c = cards_.value(id);
                if (c && id == placedId_ && c->note().text.isEmpty()) {     // placed, then not written; an older empty one stays
                    try {
                        view_->flushInk();
                        doc_->removeAnnots(c->note().page, {id});
                    } catch (const PdfError &) {}
                }
                if (placedId_ == id) placedId_ = 0;
                QTimer::singleShot(0, this, &NotesPanel::reload);
            });
            connect(c, &NoteCard::deleteMarking, this, [this](int page, int id) {
                try {
                    view_->flushInk();
                    doc_->removeAnnots(page, {id});
                } catch (const PdfError &) {}
                reload();
            });
        } else if (!c->isEditing())
            c->setNote(m);
    }
    for (auto it = cards_.begin(); it != cards_.end();) {
        if (!keep.contains(it.key()) && !it.value()->isEditing()) {
            it.value()->deleteLater();
            it = cards_.erase(it);
        } else
            ++it;
    }
    relayout();
}

void NotesPanel::editNote(int page, int id)
{
    editingId_ = id;
    reload();
    NoteCard *c = cards_.value(id);
    if (!c) return;     // the marking does not exist (any more)
    Q_UNUSED(page);
    c->show();
    c->startEditing();
    relayout();
}

QPointF NotesPanel::anchorOf(const MarkupNote &n) const
{
    // for a text marking the bottom-left corner of its first line: the line then runs along the
    // bottom of the text line instead of striking through the words next to the marking
    return n.anchor;
}

// A text marking's line runs along the bottom of its text line. A stroke can sit anywhere: its
// line is moved to the bottom of the text line it would otherwise strike through.
qreal NotesPanel::lineYOf(const MarkupNote &n)
{
    if (n.kind != AnnotKind::Ink) return n.anchor.y();
    auto it = lineYs_.find(n.id);
    if (it == lineYs_.end() || it->first != n.anchor) it = lineYs_.insert(n.id, {n.anchor, doc_->clearOfText(n.page, n.anchor)});
    return it->second;
}

void NotesPanel::relayout()
{
    placed_.clear();
    const int w = qMax(60, width() - 2 * kSide);
    QWidget *vp = view_->viewport();
    struct Item { qreal y; NoteCard *c; };
    QVector<Item> items;
    int nAbove = 0, nBelow = 0;
    qreal yAbove = -1e9, yBelow = 1e9;
    for (const MarkupNote &m : std::as_const(notes_)) {
        NoteCard *c = cards_.value(m.id);
        if (!c) continue;
        QPointF a = view_->pageToView(m.page, QPointF(anchorOf(m).x(), lineYOf(m)));
        qreal y = mapFromGlobal(vp->mapToGlobal(a.toPoint())).y();
        items.push_back({y, c});
        if (y < 0) {
            ++nAbove;
            if (y > yAbove) { yAbove = y; aboveId_ = m.id; }
        } else if (y > height()) {
            ++nBelow;
            if (y < yBelow) { yBelow = y; belowId_ = m.id; }
        }
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.y < b.y; });
    qreal bottom = -1e9;
    for (const Item &it : std::as_const(items)) {
        it.c->fitWidth(w);
        qreal top = qMax(it.y - 14, bottom + kGapY);
        bottom = top + it.c->height();
        bool visible = bottom > -200 && top < height() + 200;
        if (visible || it.c->isEditing()) {
            it.c->move(kSide, int(top));
            it.c->show();
            placed_.push_back({it.y, it.c});
        } else
            it.c->hide();
    }
    showHint(above_, true, nAbove, aboveId_);
    showHint(below_, false, nBelow, belowId_);
    updateLinks();
    update();
}

// A hint at the top or bottom edge that there are notes further up or down; a click goes to the
// nearest and selects it.
void NotesPanel::showHint(QToolButton *hint, bool up, int count, int id)
{
    hint->setVisible(count > 0 && id != editingId_);
    if (hint->isHidden()) return;
    hint->setText(up ? (count == 1 ? tr("↑ 1 note above") : tr("↑ %1 notes above").arg(count))
                     : (count == 1 ? tr("↓ 1 note below") : tr("↓ %1 notes below").arg(count)));
    hint->adjustSize();
    hint->move((width() - hint->width()) / 2, up ? 6 : height() - hint->height() - 6);
    hint->raise();
}

void NotesPanel::updateLinks()
{
    // grey, so that lines at the same height cannot be confused; only a lit note (edited, selected
    // or under the pointer) shows its colour, and comes last to be drawn on top
    QVector<NoteLink> links, lit;
    for (const MarkupNote &m : std::as_const(notes_)) {
        NoteCard *c = cards_.value(m.id);
        if (!c) continue;
        if (c->isLit()) lit.push_back({m.page, m.id, m.kind, anchorOf(m), lineYOf(m), noteLineColor(m.color), m.quads, true});
        else links.push_back({m.page, m.id, m.kind, anchorOf(m), lineYOf(m), kIdleLine, m.quads, false});
    }
    view_->setNoteLinks(links + lit);
    view_->setNoteLinksVisible(active_ && isVisible());
}

void NotesPanel::updateLit()
{
    for (auto it = cards_.cbegin(); it != cards_.cend(); ++it) it.value()->setLit(it.key() == hoveredId_ || it.key() == selectedId_);
    updateLinks();
    update();
}

void NotesPanel::setActive(bool on)
{
    active_ = on;
    if (on) relayout();
    view_->setNoteLinksVisible(on && isVisible());
}

void NotesPanel::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    relayout();
}

void NotesPanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (placed_.isEmpty()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect().adjusted(16, 20, -16, -20), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   notes_.isEmpty() ? tr("No notes yet.\n\nSelect text, pick a colour in the small bar and type your note.")
                                    : tr("No notes in this part of the document."));
        return;
    }
    // continue the line from the page: right edge at the marking's height, then to the card
    for (bool lit : {false, true})       // the coloured lines of lit notes on top
        for (const auto &pc : std::as_const(placed_)) {
            NoteCard *c = pc.second;
            if (c->isLit() != lit) continue;
            p.setPen(QPen(lit ? noteLineColor(c->note().color) : kIdleLine, 1.2));
            qreal y = pc.first, cy = c->y() + 14, right = width();
            QPointF elbow(right - kSide / 2.0, y);
            p.drawLine(QPointF(right, y), elbow);
            p.drawLine(elbow, QPointF(c->geometry().right() + 1, cy));
        }
}
