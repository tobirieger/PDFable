// DocumentTab: one open document = sidebar (outline, pages organizer, signatures) + view + search bar.
#pragma once
#include "PdfView.h"
#include <QWidget>

class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QLineEdit;
class QLabel;
class QSplitter;
class NotesPanel;
class QFileSystemWatcher;

class QPrinter;

// Where a page goes on the sheet when printing, in points: the target rectangle on the paper, whether
// the page is turned a quarter, and the scale (1 = actual size).
struct PrintPlacement {
    QRectF target;
    bool rotated = false;
    double scale = 1;
};

class DocumentTab : public QWidget {
    Q_OBJECT
public:
    explicit DocumentTab(std::shared_ptr<ink::Document> doc, QWidget *parent = nullptr);
    PdfView *view() const { return view_; }
    ink::Document *doc() const { return doc_.get(); }
    QString title() const;

    bool save();
    bool saveAs(bool optimize = false);
    bool maybeSave();          // false = user cancelled
    void print();
    // Prints pages [from, to] on an already set-up printer (used by print() and the self-test).
    void printPages(QPrinter &printer, int from, int to);
    static PrintPlacement printPlacement(QSizeF page, const QRectF &paper, const QRectF &printable);
    void showSearch();
    void closeSearch();                  // hide the search bar, drop the marks, back to the page
    void toggleSidebar();
    void showSignatures();
    void undo();
    void redo();

    // page organizer (operate on the pages selected in the sidebar, or the current page)
    QVector<int> selectedPages() const;
    void rotatePages(int delta);
    void deletePages();
    void extractPages();
    void insertPdf();
    void insertBlank();
    void rearrange(const QVector<int> &order);

    void refreshSignatures();
    int signatureCount() const { return sigCount_; }

signals:
    void titleChanged();
    void statusMessage(const QString &msg);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    void rebuildOutline();
    void rebuildPages();
    void requestThumbs();
    void pagesMoved();
    template <class F> void guarded(F &&f);

    std::shared_ptr<ink::Document> doc_;
    PdfView *view_;
    QSplitter *split_;
    QTabWidget *side_;
    QTreeWidget *outline_, *sigTree_;
    QListWidget *pages_;
    QWidget *searchBar_;
    QLineEdit *searchEdit_;
    QLabel *searchInfo_;
    QVector<ink::SignatureInfo> sigs_;
    int sigCount_ = 0;
    QWidget *sigPanel_ = nullptr;
    NotesPanel *notes_ = nullptr;
    QTimer *notesTimer_ = nullptr;
    void updateNotesActive();
public:
    NotesPanel *notesPanel() const { return notes_; }
    void showNotes();
private:
    bool rebuilding_ = false;
    QFileSystemWatcher *watcher_ = nullptr;
    QTimer reloadTimer_;
    int reloadTries_ = 0;
    void watchFile();
    void reloadFromDisk();
};
