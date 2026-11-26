// Notes sidebar: the notes attached to text markings, each shown at the height of its marking
// in the document, with a fine line from the marking to the note (like comments in Word).
#pragma once
#include "Document.h"
#include <QFrame>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <memory>

class PdfView;
class QLabel;
class QPlainTextEdit;
class QToolButton;

class NoteCard : public QFrame {
    Q_OBJECT
public:
    NoteCard(const ink::MarkupNote &n, const QString &excerpt, QWidget *parent);
    void setNote(const ink::MarkupNote &n);
    const ink::MarkupNote &note() const { return note_; }
    void startEditing();
    bool isEditing() const { return editing_; }
    void setLit(bool on);                // its marking is selected, or the pointer is on either
    bool isLit() const { return editing_ || lit_; }      // shows the colour of its marking
    void fitWidth(int w);                // also sets the height: grows with the note, a long one scrolls
signals:
    void committed(int page, int id, const QString &text);
    void cancelled(int id);
    void deleteMarking(int page, int id);
    void heightChanged();
    void editingStarted();
    void hovered(bool on);
protected:
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
private:
    void setEditing(bool on);
    void finish(bool keep);
    ink::MarkupNote note_;
    QLabel *excerpt_, *meta_;            // quoted text of the marking; author and date of the note
    QPlainTextEdit *edit_;               // shows the note and edits it, so both look the same
    bool editing_ = false, lit_ = false;
    int fitted_ = 0;                     // width the height was computed for (0 = compute again)
};

class NotesPanel : public QWidget {
    Q_OBJECT
public:
    NotesPanel(std::shared_ptr<ink::Document> doc, PdfView *view, QWidget *parent = nullptr);
    void reload();                       // re-read all markings (call after document changes)
    void editNote(int page, int id);     // open the editor of a marking's note (new or existing)
    void setActive(bool on);             // visible: lines are drawn in the page view
    int noteCount() const;
    NoteCard *card(int id) const { return cards_.value(id); }
protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
private:
    void relayout();
    void updateLinks();
    void updateLit();                    // after the hovered or the selected note changed
    QPointF anchorOf(const ink::MarkupNote &n) const;
    qreal lineYOf(const ink::MarkupNote &n);
    void showHint(QToolButton *hint, bool up, int count, int id);
    QString excerptFor(const ink::MarkupNote &n);

    std::shared_ptr<ink::Document> doc_;
    PdfView *view_;
    QVector<ink::MarkupNote> notes_;     // markings that have a note (plus the one being written)
    QHash<int, NoteCard *> cards_;
    QHash<int, QString> excerpts_;
    QHash<int, QPair<QPointF, qreal>> lineYs_;   // per stroke: its anchor and the height its line runs at
    QVector<QPair<qreal, NoteCard *>> placed_;   // anchor y in panel coordinates, card
    int editingId_ = 0, hoveredId_ = 0, selectedId_ = 0;
    int placedId_ = 0;                   // the sticky note just put on the page: removed again if it gets no text
    QToolButton *above_, *below_;        // "3 notes above": notes outside the part of the document in view
    int aboveId_ = 0, belowId_ = 0;      // the nearest of them, where a click on the hint goes
    bool active_ = false;
};
