#include "NotesPanel.h"
#include "PdfView.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextLayout>
#include <QStyle>
#include <QToolButton>
#include <QVariantAnimation>
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

QVector<qreal> stackNotes(const QVector<qreal> &want, const QVector<int> &heights, int gap, int pinned)
{
    QVector<qreal> top(want.size());
    qreal bottom = -1e9;
    for (int i = 0; i < want.size(); ++i) {
        top[i] = i == pinned ? want[i] : qMax(want[i], bottom + gap);
        bottom = top[i] + heights[i];
    }
    // the cards before the pinned one that are now in its way move up, as few of them as needed
    for (int i = pinned - 1; i >= 0 && top[i] + heights[i] + gap > top[i + 1]; --i) top[i] = top[i + 1] - gap - heights[i];
    return top;
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
    setExcerpt(excerpt);
    edit_ = new QPlainTextEdit(this);
    edit_->setPlaceholderText(tr("Type a note…  (Ctrl+Enter to finish, Esc to cancel)"));
    edit_->setFrameShape(QFrame::NoFrame);
    edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit_->setStyleSheet("QPlainTextEdit{background:transparent;border:none;padding:0;}");
    edit_->document()->setDocumentMargin(0);
    edit_->installEventFilter(this);
    edit_->viewport()->installEventFilter(this);
    edit_->viewport()->setCursor(Qt::IBeamCursor);
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

void NoteCard::setExcerpt(const QString &excerpt)
{
    const QString ex = excerpt.size() > 140 ? excerpt.left(137) + QStringLiteral("…") : excerpt;
    const QString text = ex.isEmpty() ? QString() : QStringLiteral("“%1”").arg(ex);
    if (text == excerpt_->text() && excerpt_->isHidden() == text.isEmpty()) return;
    excerpt_->setText(text);
    excerpt_->setVisible(!text.isEmpty());
    fitted_ = 0;        // its height may have changed
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
    // Del and the arrow keys act on the notes while the sidebar has the keyboard, and on the page otherwise: the
    // note the keyboard acts on shows it with a tint of its marking's colour. So does the note being written,
    // whose text cursor alone is easy to miss.
    QColor bg = palette().color(QPalette::Base);
    if (keyboard_ || editing_) {
        const qreal t = bg.lightness() < 128 ? 0.18 : 0.24;     // less on a dark card, where yellow soon turns olive
        bg = QColor::fromRgbF(bg.redF() + (note_.color.redF() - bg.redF()) * t, bg.greenF() + (note_.color.greenF() - bg.greenF()) * t,
                              bg.blueF() + (note_.color.blueF() - bg.blueF()) * t);
    }
    p.setBrush(bg);
    p.drawRoundedRect(r, 7, 7);
    p.setPen(Qt::NoPen);
    p.setBrush(note_.color);
    p.drawRoundedRect(QRectF(r.left() + 3, r.top() + 6, 4, r.height() - 12), 2, 2);   // colour of the marking
}

// Not editing: the text is only shown. A click on it starts editing, a click beside it selects the note.
void NoteCard::setEditing(bool on)
{
    editing_ = on;
    edit_->setReadOnly(!on);
    edit_->setTextInteractionFlags(on ? Qt::TextEditorInteraction : Qt::NoTextInteraction);
    edit_->setFocusPolicy(on ? Qt::StrongFocus : Qt::NoFocus);
    edit_->setContextMenuPolicy(on ? Qt::DefaultContextMenu : Qt::NoContextMenu);
    fitted_ = 0;
    update();
}

void NoteCard::setKeyboard(bool on)
{
    if (on == keyboard_) return;
    keyboard_ = on;
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
    // Finished with Esc or Ctrl+Enter: the keyboard goes to the sidebar, where the arrow keys go from note to
    // note. Finished because the keyboard went elsewhere, perhaps to another note's text: leave it there.
    if (edit_->hasFocus()) parentWidget()->setFocus();
    edit_->setPlainText(note_.text);
    fitWidth(width());
    if (keep && text != note_.text) emit committed(note_.page, note_.id, text);
    else emit cancelled(note_.id);
}

bool NoteCard::eventFilter(QObject *o, QEvent *e)
{
    if (o == edit_->viewport() && !editing_ && e->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
        startEditing();
        return true;
    }
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
    if (e->button() == Qt::LeftButton) emit clicked();
    QFrame::mousePressEvent(e);
}

void NoteCard::removeNote()
{
    finish(false);
    emit committed(note_.page, note_.id, QString());
}

void NoteCard::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu m(this);
    m.addAction(tr("Edit note"), this, &NoteCard::startEditing);
    m.addAction(tr("Delete note"), this, &NoteCard::removeNote);
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
    setFocusPolicy(Qt::ClickFocus);
    slide_ = new QVariantAnimation(this);
    slide_->setStartValue(0.0);
    slide_->setEndValue(1.0);
    slide_->setEasingCurve(QEasingCurve::OutCubic);
    connect(slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        for (auto it = sliding_.cbegin(); it != sliding_.cend(); ++it)
            it.key()->move(kSide, int(std::lround(it->first + (it->second - it->first) * v.toDouble())));
        update();       // the lines to the cards
    });
    connect(slide_, &QVariantAnimation::finished, this, [this] { sliding_.clear(); });
    connect(view_, &PdfView::viewChanged, this, &NotesPanel::relayout);
    for (QToolButton **hint : {&above_, &below_}) {
        auto *b = *hint = new QToolButton(this);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet("QToolButton{background:palette(base);border:1px solid palette(mid);border-radius:9px;padding:2px 10px;}"
                         "QToolButton:hover{border-color:palette(highlight);}");
        b->hide();
        connect(b, &QToolButton::clicked, this, [this, b] {
            for (const MarkupNote &m : std::as_const(notes_))
                if (m.id == (b == above_ ? aboveId_ : belowId_)) {
                    goTo(m, true);
                    break;
                }
        });
    }
    connect(view_, &PdfView::noteHovered, this, [this](int id) { hoveredId_ = id; updateLit(); });
    connect(view_, &PdfView::noteSelected, this, [this](int id) {
        selectedId_ = id;
        updateLit();
        relayout();     // the selected note moves to the height of its marking
    });
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
    // Kept with the page and the quads it was read from: ids are object numbers, which an optimised save renumbers
    // (and undo can bring back), so an id alone may name another marking afterwards, with other text.
    auto it = excerpts_.find(n.id);
    if (it == excerpts_.end() || it->page != n.page || it->quads != n.quads)
        it = excerpts_.insert(n.id, {n.page, n.quads, doc_->textInQuads(n.page, n.quads)});
    return it->text;
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
            connect(c, &NoteCard::clicked, this, [this, c] {
                view_->selectAnnot(c->note().page, c->note().id);
                setFocus();
            });
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
                if (text.isEmpty()) view_->setFocus();      // its card goes: nothing here for the keyboard to act on
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
                if (!c || c->note().text.isEmpty()) view_->setFocus();     // a note begun and given up: back to its marking
                QTimer::singleShot(0, this, &NotesPanel::reload);
            });
            connect(c, &NoteCard::deleteMarking, this, [this](int page, int id) {
                try {
                    view_->flushInk();
                    doc_->removeAnnots(page, {id});
                } catch (const PdfError &) {}
                reload();
            });
        } else if (!c->isEditing()) {
            c->setNote(m);
            c->setExcerpt(excerptFor(m));
        }
    }
    // One order for everything: the cards are stacked in it and the arrow keys walk it. Reading order, by page,
    // then by height, then from the left. (Sorting the cards by their rounded pixel height instead let two notes
    // on one text line come out in another order than the keys took them in.)
    std::sort(notes_.begin(), notes_.end(), [this](const MarkupNote &a, const MarkupNote &b) {
        if (a.page != b.page) return a.page < b.page;
        const qreal ya = lineYOf(a), yb = lineYOf(b);
        return ya != yb ? ya < yb : a.anchor.x() != b.anchor.x() ? a.anchor.x() < b.anchor.x() : a.id < b.id;
    });
    for (auto it = cards_.begin(); it != cards_.end();) {
        if (!keep.contains(it.key()) && !it.value()->isEditing()) {
            sliding_.remove(it.value());
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
    QVector<qreal> want;
    QVector<int> heights;
    int pinned = -1;        // the note being written, else the selected one
    for (int i = 0; i < items.size(); ++i) {
        NoteCard *c = items[i].c;
        c->fitWidth(w);
        want.push_back(items[i].y - 14);
        heights.push_back(c->height());
        if (c->isEditing() || (c->note().id == selectedId_ && pinned < 0)) pinned = i;
    }
    const QVector<qreal> tops = stackNotes(want, heights, kGapY, pinned);
    const int pinnedId = pinned >= 0 ? items[pinned].c->note().id : 0;
    // short, and not at all where the system has animations switched off
    const int ms = qMin(120, style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, this));
    // Slide when another note became the pinned one. If the goals change again while the cards are on their way
    // (a card grew, the view moved), they set off anew from where they are, so that none jumps back.
    bool retarget = false;
    for (int i = 0; i < items.size() && !retarget; ++i)
        retarget = sliding_.contains(items[i].c) && sliding_[items[i].c].second != int(tops[i]);
    const bool start = (pinnedId != pinnedId_ || retarget) && isVisible() && ms > 0;
    pinnedId_ = pinnedId;
    if (start) sliding_.clear();
    for (int i = 0; i < items.size(); ++i) {
        const Item &it = items[i];
        bool visible = tops[i] + it.c->height() > -200 && tops[i] < height() + 200;
        if (visible || it.c->isEditing()) {
            const int to = int(tops[i]);
            if (start && it.c->isVisible() && it.c->y() != to) sliding_.insert(it.c, {it.c->y(), to});
            else if (!sliding_.contains(it.c)) it.c->move(kSide, to);
            it.c->show();
            placed_.push_back({it.y, it.c});
        } else {
            sliding_.remove(it.c);
            it.c->hide();
        }
    }
    if (start && !sliding_.isEmpty()) {
        slide_->stop();
        slide_->setDuration(ms);
        slide_->start();
    }
    showHint(above_, true, nAbove, aboveId_);
    showHint(below_, false, nBelow, belowId_);
    updateLinks();
    update();
}

void NotesPanel::focusInEvent(QFocusEvent *e)
{
    QWidget::focusInEvent(e);
    updateLit();
}

void NotesPanel::focusOutEvent(QFocusEvent *e)
{
    QWidget::focusOutEvent(e);
    updateLit();
}

void NotesPanel::goTo(const MarkupNote &n, bool always)
{
    const qreal y = mapFromGlobal(view_->viewport()->mapToGlobal(view_->pageToView(n.page, QPointF(n.anchor.x(), lineYOf(n))).toPoint())).y();
    if (always || y < 20 || y > height() - 40) view_->gotoPage(n.page, qMax(0.0, lineYOf(n) - 60));
    view_->selectAnnot(n.page, n.id);       // lit, and its card at the height of its marking
}

// With the keyboard in the sidebar (after a click on a note): Up and Down go to the previous and the
// next note in the document, Enter opens the selected one, Del removes it, Esc goes back to the page.
void NotesPanel::keyPressEvent(QKeyEvent *e)
{
    QVector<MarkupNote> order;      // notes_ is in reading order
    for (const MarkupNote &m : std::as_const(notes_))
        if (cards_.contains(m.id)) order.push_back(m);
    int at = -1;
    for (int i = 0; i < order.size(); ++i)
        if (order[i].id == selectedId_) at = i;
    switch (e->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int to = at < 0 ? (e->key() == Qt::Key_Down ? 0 : int(order.size()) - 1) : at + (e->key() == Qt::Key_Down ? 1 : -1);
        if (to >= 0 && to < order.size()) goTo(order[to], false);
        return;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (at >= 0) cards_.value(order[at].id)->startEditing();
        else QCoreApplication::sendEvent(view_, e);     // a selected marking without a note: the page opens one
        return;
    case Qt::Key_Delete:        // not Backspace: too easily pressed in the belief of being in a text field
        if (at >= 0) {
            const int next = at + 1 < order.size() ? at + 1 : at - 1;
            cards_.value(order[at].id)->removeNote();
            if (next >= 0) {        // stay among the notes
                goTo(order[next], false);
                setFocus();
            }
        }
        return;
    case Qt::Key_Escape: view_->setFocus(); return;
    default: QWidget::keyPressEvent(e);
    }
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
    for (auto it = cards_.cbegin(); it != cards_.cend(); ++it) {
        it.value()->setLit(it.key() == hoveredId_ || it.key() == selectedId_);
        it.value()->setKeyboard(it.key() == selectedId_ && hasFocus());
    }
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
