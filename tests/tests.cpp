// Headless self-test: run with QT_QPA_PLATFORM=offscreen. Usage: pdfable_tests <sample.pdf> <outdir>
#include "Dialogs.h"
#include "Creator.h"
#include "MainWindow.h"
#include "NotesPanel.h"
#include "Theme.h"
#include <QPlainTextEdit>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QSet>
#include <QLabel>
#include <QScrollBar>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPrinter>
#include <QPageSize>
#include <QMouseEvent>
#include <QFileInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QTabletEvent>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <functional>
#include <cstdio>

using namespace ink;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } else std::printf("ok   %s\n", #c); } while (0)

static void wait(int ms)
{
    QEventLoop l;
    QTimer::singleShot(ms, &l, &QEventLoop::quit);
    l.exec();
}

// Run the event loop until cond holds, at most ms milliseconds: renders arrive from a worker thread, whose pace
// depends on the machine.
static bool waitFor(const std::function<bool()> &cond, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        wait(20);
    }
    return true;
}

// The view once everything it shows is rendered and searched (painting asks for the pictures still missing).
static QImage settled(PdfView *v)
{
    waitFor([v] { v->viewport()->grab(); return !v->testBusy(); });
    return v->viewport()->grab().toImage();
}

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName("PDFableTest");
    QCoreApplication::setApplicationName("PDFableTest");
    QApplication app(argc, argv);
    Theme::apply(app);
    QString sample = argc > 1 ? argv[1] : "tests/sample.pdf";
    QString out = argc > 2 ? argv[2] : "testout";
    QDir().mkpath(out);
    QFile::remove(out + "/work.pdf");
    QFile::copy(sample, out + "/work.pdf");

    // ---- ink engine
    {
        StrokeBuilder b;
        b.begin({100, 100}, 0.5f, 0, 2, true, 1.5);
        for (int i = 1; i <= 60; ++i) b.add({100 + i * 3.0, 100 + std::sin(i * 0.9) * 1.2}, 0.5f, i * 8);
        CHECK(b.points().size() > 60);
        CHECK(b.snapToShape() && b.shape() == StrokeBuilder::Shape::Line);
        CHECK(std::fabs(b.points()[0].y() - b.points()[1].y()) < 1e-9);   // snapped horizontal
        StrokeBuilder c;
        c.begin({200, 100}, 0.5f, 0, 2, true, 1.5);
        for (int i = 1; i <= 80; ++i) { double t = 2 * M_PI * i / 78; c.add({150 + 50 * std::cos(t) + std::sin(i) * 1.5, 100 + 48 * std::sin(t)}, 0.5f, i * 8); }
        CHECK(c.snapToShape() && c.shape() == StrokeBuilder::Shape::Ellipse);
        StrokeBuilder r;
        r.begin({0, 0}, 0.5f, 0, 2, true, 1.5);
        int k = 0;
        auto seg = [&](QPointF a, QPointF e) { for (int i = 1; i <= 20; ++i) r.add(a + (e - a) * (i / 20.0) + QPointF(std::sin(i) * 0.8, std::cos(i) * 0.8), 0.5f, ++k * 8); };
        seg({0, 0}, {120, 0}); seg({120, 0}, {120, 70}); seg({120, 70}, {0, 70}); seg({0, 70}, {2, 3});
        CHECK(r.snapToShape() && r.shape() == StrokeBuilder::Shape::Rect);
        QRectF before = QPolygonF(r.points()).boundingRect();
        r.add({2, 3}, 0.5f, 5000);            // still inside the dead zone: nothing moves
        r.add({20, 12}, 0.5f, 5010);          // leaving it starts the resize ...
        r.add({-40, -30}, 0.5f, 5020);        // ... and dragging outwards enlarges the rectangle
        QRectF after = QPolygonF(r.points()).boundingRect();
        std::printf("     rect %.0fx%.0f -> %.0fx%.0f\n", before.width(), before.height(), after.width(), after.height());
        CHECK(after.width() > before.width() * 1.2 && after.height() > before.height() * 1.2);
        CHECK(std::fabs(after.right() - before.right()) < 1e-6 && std::fabs(after.bottom() - before.bottom()) < 1e-6);
        StrokeBuilder z;   // a scribble must not be converted
        z.begin({0, 0}, 0.5f, 0, 2, true, 1.5);
        for (int i = 1; i <= 80; ++i) z.add({i * 2.0, 30 * std::sin(i * 0.5)}, 0.5f, i * 8);
        CHECK(!z.snapToShape());
    }

    // ---- UI
    auto *w = new MainWindow;
    w->resize(1300, 900);
    w->show();
    CHECK(w->openFile(out + "/work.pdf"));
    wait(400);
    DocumentTab *tab = w->currentTab();
    PdfView *v = tab->view();
    Document *d = tab->doc();
    CHECK(d->pageCount() == 6);
    CHECK(d->outline().size() == 6);
    QAction *undoAct = nullptr, *redoAct = nullptr;         // greyed out while there is nothing to undo or redo
    for (QAction *a : w->findChildren<QAction *>()) {
        if (a->text() == "&Undo") undoAct = a;
        if (a->text() == "&Redo") redoAct = a;
    }
    CHECK(undoAct && redoAct && !undoAct->isEnabled() && !redoAct->isEnabled());
    wait(500);
    w->grab().save(out + "/1_open.png");

    // dark interface (Ctrl+Alt+D) switches palette and icons, and back
    MainWindow::setInterfaceMode(Theme::Mode::Dark);
    wait(200);
    CHECK(Theme::isDark() && qApp->palette().color(QPalette::Window).lightness() < 60);
    w->grab().save(out + "/1b_dark_interface.png");
    MainWindow::setInterfaceMode(Theme::Mode::Light);
    CHECK(!Theme::isDark() && qApp->palette().color(QPalette::Window).lightness() > 200);

    // pen stroke with pressure, then hold to straighten a second one
    w->setTool(Tool::Pen);
    QPointF o(300, 300);
    v->testPointer(0, o, 0.2f, 0);
    for (int i = 1; i <= 80; ++i) v->testPointer(1, o + QPointF(i * 4, 40 * std::sin(i * 0.15)), 0.2f + 0.7f * float(std::fabs(std::sin(i * 0.08))), i * 7);
    wait(30);
    v->viewport()->grab().save(out + "/2_live_stroke.png");
    v->testPointer(2, o + QPointF(320, 0), 0, 600);
    v->testPointer(0, {300, 420}, 0.5f, 1000);
    for (int i = 1; i <= 60; ++i) v->testPointer(1, QPointF(300 + i * 5, 420 + std::sin(i * 0.7) * 2), 0.5f, 1000 + i * 7);
    CHECK(v->testHold());
    v->testPointer(1, QPointF(640, 424), 0.5f, 2000);   // drag the end of the snapped line
    v->testPointer(2, QPointF(640, 424), 0, 2010);
    w->setTool(Tool::Highlighter);
    v->testPointer(0, {180, 210}, -1, 3000);
    for (int i = 1; i <= 40; ++i) v->testPointer(1, QPointF(180 + i * 8, 210), -1, 3000 + i * 7);
    v->testPointer(2, {500, 210}, -1, 3400);
    v->flushInk();
    int pg = v->currentPage();
    CHECK(d->inkAnnots(pg).size() == 3);
    CHECK(d->inkAnnots(pg)[0].data.strokes[0].w.size() == d->inkAnnots(pg)[0].data.strokes[0].pts.size());   // pressure kept
    CHECK(d->inkAnnots(pg)[1].data.strokes[0].pts.size() == 2);                                           // straight line
    wait(500);
    v->viewport()->grab().save(out + "/3_ink_committed.png");

    // eraser removes the wavy stroke
    w->setTool(Tool::Eraser);
    v->testPointer(0, o + QPointF(160, -60), -1, 4000);
    for (int i = 1; i <= 30; ++i) v->testPointer(1, o + QPointF(160, -60 + i * 5), -1, 4000 + i * 7);
    v->testPointer(2, o + QPointF(160, 90), -1, 4300);
    CHECK(d->inkAnnots(pg).size() == 2);
    tab->undo();
    CHECK(d->inkAnnots(pg).size() == 3);

    // text selection + highlight, search
    w->setTool(Tool::Select);
    v->testPointer(0, {190, 180}, -1, 5000);
    v->testPointer(1, {520, 184}, -1, 5100);
    v->testPointer(2, {520, 184}, -1, 5200);
    CHECK(v->hasTextSelection());
    std::printf("     selected: %s\n", qPrintable(v->selectedText().left(60)));
    v->markupSelection(AnnotKind::Highlight, QColor(140, 235, 130));
    CHECK(d->annots(pg).size() == 4);
    v->setSearch("kinetic");
    wait(300);
    std::printf("     search=%d\n", v->searchCount());
    CHECK(v->searchCount() >= 6 * 28);
    // undo and redo of the highlight change nothing else: the frame right after each is the one before (the new
    // picture is still being rendered), and once it arrives only the highlight differs; hits and selection stay
    {
        v->testPointer(0, {190, 400}, -1, 5300);
        v->testPointer(1, {520, 404}, -1, 5400);
        v->testPointer(2, {520, 404}, -1, 5500);
        CHECK(v->hasTextSelection());
        const QImage marked = settled(v);
        const int hits = v->searchCount();
        tab->undo();
        CHECK(d->annots(pg).size() == 3);
        CHECK(v->viewport()->grab().toImage() == marked);
        const QImage unmarked = settled(v);
        CHECK(unmarked != marked && v->searchCount() == hits && v->hasTextSelection());
        tab->redo();
        CHECK(d->annots(pg).size() == 4);
        CHECK(v->viewport()->grab().toImage() == unmarked);
        CHECK(settled(v) == marked && v->searchCount() == hits && v->hasTextSelection());
    }
    v->findNext(true);
    wait(300);
    v->viewport()->grab().save(out + "/4_search_select.png");
    v->setSearch(QString());

    // search marks are replaced in the same frame while typing, instead of flashing off until the background search
    // gets round to the page in view
    {
        auto finish = [&] { for (int i = 0; i < 500 && v->searching(); ++i) wait(10); return !v->searching(); };
        auto visible = [&](int page) {   // the part of a page in view, in viewport pixels
            QSizeF s = d->pageSize(page);
            return QRectF(v->pageToView(page, {0, 0}), v->pageToView(page, {s.width(), s.height()})).toAlignedRect().intersected(v->viewport()->rect());
        };
        auto marks = [](const QImage &img, const QRect &r) {   // pixels tinted yellow or orange by search marks
            int n = 0;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    QColor c = img.pixelColor(x, y);
                    n += c.red() > 200 && c.green() > 150 && c.red() - c.blue() > 60;
                }
            return n;
        };
        auto grab = [&] { return v->viewport()->grab().toImage(); };
        const QString term = "kinetic";

        v->gotoPage(2, 300, false);
        wait(500);
        const int cur = v->currentPage();
        CHECK(marks(grab(), visible(cur)) == 0);
        v->setSearch(term);
        const int early = v->searchCount();
        const QImage first = grab();
        const QRect area = visible(cur);                   // the first hit may have been cut off at the top: the view moved to show it
        const QPoint topA = v->pageToView(cur, {0, 0}).toPoint();
        std::printf("     right after setSearch: %d hits, %d marked pixels on page %d\n", early, marks(first, area), cur + 1);
        CHECK(early > 0);
        CHECK(marks(first, area) > 0);
        CHECK(finish());
        // The current hit is found and drawn orange in that first frame already: once the search has finished, the
        // part of the page in view must look the same.
        const QImage done = grab();
        const QPoint topB = v->pageToView(cur, {0, 0}).toPoint();
        const QRect both = area.translated(-topA).intersected(visible(cur).translated(-topB));
        int compared = 0, differ = 0;
        for (int y = both.top(); y <= both.bottom(); ++y)
            for (int x = both.left(); x <= both.right(); ++x) {
                ++compared;
                differ += first.pixel(QPoint(x, y) + topA) != done.pixel(QPoint(x, y) + topB);
            }
        std::printf("     compared %d page pixels (%d marked), %d differ\n", compared, marks(done, both.translated(topB)), differ);
        CHECK(compared > 10000 && marks(done, both.translated(topB)) > 1000 && differ == 0);

        // type the term letter by letter; every other keystroke arrives while the previous background search still runs
        v->setSearch(QString());
        bool marked = true;
        for (int i = 1; i <= term.size(); ++i) {
            v->setSearch(term.left(i));
            marked &= marks(grab(), visible(v->currentPage())) > 0;
            if (i % 2) {
                QCoreApplication::processEvents();
                marked &= marks(grab(), visible(v->currentPage())) > 0;
            }
        }
        CHECK(marked);
        CHECK(finish());
        QVector<int> perPage;
        int total = 0;
        for (int p = 0; p < d->pageCount(); ++p) total += perPage.emplace_back(int(d->search(p, term).size()));
        std::printf("     %d hits, the pages hold %d\n", v->searchCount(), total);
        CHECK(v->searchCount() == total);

        // Started further down, the current hit is the first one at or after the top of the view (wrapping round), and
        // findNext goes from there through every hit once in each direction and comes back to it.
        v->setSearch(QString());
        v->gotoPage(4, 200, false);
        const ViewState from = v->viewState();
        v->setSearch(term);
        CHECK(finish());
        int ordinal0 = 0;
        for (int step = 0; step <= d->pageCount() && !ordinal0; ++step) {
            const int p = (from.page + step) % d->pageCount();
            const QVector<QPolygonF> hs = d->search(p, term);
            int first = 0;
            while (step == 0 && first < hs.size() && hs[first].boundingRect().bottom() < from.y) ++first;
            if (first == hs.size()) continue;
            ordinal0 = first + 1;
            for (int b = 0; b < p; ++b) ordinal0 += perPage[b];
        }
        QVector<int> seen;
        auto conn = QObject::connect(v, &PdfView::searchChanged, [&](int, int c) { seen.push_back(c); });
        for (int i = 0; i < total; ++i) v->findNext(true);
        for (int i = 0; i < total; ++i) v->findNext(false);
        QObject::disconnect(conn);
        bool fwd = seen.size() == 2 * total, back = fwd;
        int o = ordinal0;
        for (int i = 0; i < total && fwd; ++i) { o = o % total + 1; fwd = seen[i] == o; }
        for (int i = 0; i < total && back; ++i) { o = (o + total - 2) % total + 1; back = seen[total + i] == o; }
        CHECK(fwd);
        CHECK(back);
        // typing on: a longer term is only looked for where the shorter one was found, and must find what a
        // fresh search of the whole document finds; a shorter or different term starts from scratch
        auto everywhere = [&](const QString &t) {
            int n = 0;
            for (int p = 0; p < d->pageCount(); ++p) n += d->search(p, t).size();
            return n;
        };
        bool same = true;
        for (const QString &t : {QString("Line 17"), QString("Line 17 of page 2"), QString("Line 17 of page 2."), QString("Line 17"), QString("kinetic")}) {
            v->setSearch(t);
            same = same && finish() && v->searchCount() == everywhere(t);
        }
        CHECK(same && everywhere("Line 17 of page 2") > 0 && everywhere("Line 17 of page 2") < everywhere("Line 17"));
        // While a term is typed, the search starts from where it began: the view stays put as long as the current
        // hit is in view,
        v->setSearch(QString());
        v->gotoPage(1, 0, false);
        const int rest = v->verticalScrollBar()->value();
        bool still = true;
        for (const QString &t : {QString("Line"), QString("Line 1"), QString("Line 1 of page 2")}) {
            v->setSearch(t);
            still = still && finish() && v->verticalScrollBar()->value() == rest;
        }
        CHECK(still);
        // moves on when the term only matches further down, and comes back when letters are deleted again
        v->setSearch("Line 1 of page 4");
        CHECK(finish() && v->currentPage() == 3);
        v->setSearch("Line 1 of page");
        CHECK(finish() && v->currentPage() == 1);
        // "find next" moves the starting point: typing on continues from that hit instead of jumping back
        v->setSearch("Line 1");
        CHECK(finish() && v->currentPage() == 1);
        for (int i = 0; i < 200 && v->currentPage() != 3; ++i) v->findNext(true);
        v->setSearch("Line 1 of");
        CHECK(finish() && v->currentPage() == 3);
        // Esc in the search field closes the search, like its close button: the marks go, the page has the keyboard
        tab->showSearch();
        auto *field = tab->findChild<QLineEdit *>();
        field->setText("kinetic");
        CHECK(finish() && field->isVisible() && v->searchCount() > 0);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(field, &esc);
        CHECK(!field->isVisible() && v->searchCount() == 0 && v->hasFocus());
        // the match count has a fixed space: the field and the buttons stay where they are as it changes
        {
            tab->showSearch();
            auto *info = tab->findChild<QLabel *>("searchInfo");
            QToolButton *down = nullptr;
            for (QToolButton *b : tab->findChildren<QToolButton *>()) if (b->text() == QString::fromUtf8("▼")) down = b;
            QStringList seen;
            QSet<int> fieldW, downX, infoX;
            for (const char *term : {"kinetic", "Line", "zzqqxx", "Line 1 of page 3"}) {
                field->setText(term);
                finish();
                wait(50);
                for (int i = 0; i < 2; ++i) {      // and after stepping to the next hit ("1 of n" -> "2 of n")
                    seen << info->text();
                    fieldW << field->width();
                    downX << down->x();
                    infoX << info->x();
                    v->findNext(true);
                    wait(30);
                }
            }
            std::printf("     search counts %s: field widths %lld, button positions %lld\n", qPrintable(seen.join(" | ")),
                        (long long)fieldW.size(), (long long)downX.size());
            CHECK(down && fieldW.size() == 1 && downX.size() == 1 && infoX.size() == 1);
            CHECK(info->fontMetrics().horizontalAdvance(info->text()) <= info->width());
            QApplication::sendEvent(field, &esc);
        }
        bool previous = false;                              // Shift+F3
        for (QAction *a : w->findChildren<QAction *>()) previous = previous || a->shortcut() == QKeySequence(QKeySequence::FindPrevious);
        CHECK(previous);
        v->setSearch(QString());
    }

    // link hover preview
    v->gotoPage(0, 600, false);
    wait(200);
    QVector<LinkInfo> links = d->links(0);
    CHECK(links.size() == 2 && links[0].page == 4);
    QPointF lp = v->viewport()->rect().center();
    {   // locate the link in view coordinates through a page-space search of the viewport
        for (int y = 0; y < v->viewport()->height(); y += 4)
            for (int x = 0; x < v->viewport()->width(); x += 8) {
                QMouseEvent mv(QEvent::MouseMove, QPointF(x, y), v->viewport()->mapToGlobal(QPointF(x, y)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(v->viewport(), &mv);
                if (v->viewport()->cursor().shape() == Qt::PointingHandCursor) { lp = QPointF(x, y); y = 1e6; break; }
            }
    }
    wait(600);
    QWidget *popup = nullptr;
    for (QWidget *tw : QApplication::topLevelWidgets())
        if (tw->objectName() == "linkPreview" && tw->isVisible()) popup = tw;
    CHECK(popup != nullptr);
    if (popup) {
        wait(500);
        PdfView *mini = popup->findChild<PdfView *>();
        CHECK(mini && mini->currentPage() == 4);
        // the preview is a live view: select text inside it and highlight it
        int before = d->annots(4).size();
        mini->testPointer(0, {120, 60}, -1, 100);
        mini->testPointer(1, {420, 64}, -1, 150);
        mini->testPointer(2, {420, 64}, -1, 200);
        CHECK(mini->hasTextSelection());
        mini->markupSelection(AnnotKind::Highlight, QColor(255, 235, 60));
        CHECK(d->annots(4).size() == before + 1);
        wait(500);
        popup->grab().save(out + "/5_link_preview.png");
    }
    // click follows the link
    v->testPointer(0, lp, -1, 6000);
    v->testPointer(2, lp, -1, 6050);
    wait(100);
    CHECK(v->currentPage() == 4);
    v->goBack();
    CHECK(v->currentPage() == 0);

    // text box typed directly on the page
    {
        CHECK(w->createPopupMenu() == nullptr);
        w->setTool(Tool::Text);
        int before = d->annots(0).size();
        v->gotoPage(0, 300, false);
        v->testPointer(0, {420, 300}, -1, 6500);
        v->testPointer(2, {420, 300}, -1, 6510);
        CHECK(v->isEditingText());
        auto *ed = v->viewport()->findChild<QPlainTextEdit *>();
        CHECK(ed != nullptr);
        if (ed) ed->setPlainText("Typed on the page\nsecond line");
        wait(50);
        v->viewport()->grab().save(out + "/5b_text_editing.png");
        v->testPointer(0, {900, 700}, -1, 6600);      // click elsewhere commits
        v->testPointer(2, {900, 700}, -1, 6610);
        CHECK(!v->isEditingText());
        CHECK(d->annots(0).size() == before + 1 && d->annots(0).last().text.startsWith("Typed on the page"));
        wait(500);
        v->viewport()->grab().save(out + "/5c_text_committed.png");
        w->setTool(Tool::Select);
    }

    // the left and right arrow keys turn the pages
    {
        const int at = v->verticalScrollBar()->value();
        v->gotoPage(2, 100, false);
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier), left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QApplication::sendEvent(v, &right);
        CHECK(v->currentPage() == 3);
        QApplication::sendEvent(v, &left);
        QApplication::sendEvent(v, &left);
        CHECK(v->currentPage() == 1);
        // zoomed out so far that a page is shorter than a third of the view: Left used to get stuck
        const double z0 = v->zoom();
        v->setZoom(0.2);
        wait(200);
        QScrollBar *sb = v->verticalScrollBar();
        auto topOf = [&](int page) {      // where the view goes for the top of a page
            const int was = sb->value();
            v->gotoPage(page, 0, false);
            const int to = sb->value();
            sb->setValue(was);
            return to;
        };
        v->gotoPage(0, 0, false);
        CHECK(sb->maximum() > topOf(2) && v->currentPage() > 0);    // the third of the view is on a later page
        QApplication::sendEvent(v, &right);
        CHECK(sb->value() == topOf(1));
        QApplication::sendEvent(v, &right);
        CHECK(sb->value() == topOf(2));
        QApplication::sendEvent(v, &left);
        CHECK(sb->value() == topOf(1));
        QApplication::sendEvent(v, &left);
        CHECK(sb->value() == topOf(0));
        v->setZoom(z0);
        wait(200);
        v->verticalScrollBar()->setValue(at);
    }

    // handwritten signature: pad -> library -> placement
    {
        SignaturePad pad;
        pad.resize(460, 170);
        pad.feed(0, {40, 110}, 0.4f);
        for (int i = 1; i <= 90; ++i) pad.feed(1, {40 + i * 4.0, 100 + 35 * std::sin(i * 0.35) * std::cos(i * 0.05)}, 0.3f + 0.5f * float(std::fabs(std::sin(i * 0.1))));
        pad.feed(2, {400, 100}, 0);
        CHECK(!pad.isEmpty());
        pad.grab().save(out + "/6_signature_pad.png");
        SignatureLibrary::save({pad.ink()});
        CHECK(SignatureLibrary::load().size() == 1);
        v->gotoPage(0, 500, false);
        v->startPlacingInk(pad.ink(), 170);
        int before = d->inkAnnots(0).size();
        v->testPointer(0, {700, 500}, -1, 7000);
        v->testPointer(2, {700, 500}, -1, 7010);
        CHECK(d->inkAnnots(0).size() == before + 1);
        // move the placed signature with the select tool
        QRectF r0 = d->annots(0).last().rect;
        v->testPointer(0, {700, 500}, -1, 7100);
        v->testPointer(1, {660, 540}, -1, 7150);
        v->testPointer(2, {660, 540}, -1, 7200);
        QRectF r1 = d->annots(0).last().rect;
        CHECK(std::fabs((r1.left() - r0.left()) + 40 / (v->zoom() * 96 / 72)) < 1.0);
        wait(400);
        v->viewport()->grab().save(out + "/7_signature_placed.png");
    }

    // Undo and Redo follow the history
    {
        InkData k;
        InkStroke s;
        s.pts = {QPointF(60, 700), QPointF(90, 720)};
        k.strokes.push_back(s);
        d->addInk(0, k);
        CHECK(undoAct->isEnabled() && !redoAct->isEnabled());
        tab->undo();
        CHECK(redoAct->isEnabled());
        tab->redo();
        CHECK(undoAct->isEnabled() && !redoAct->isEnabled());
        tab->undo();
    }

    // page organizer: reorder, rotate, delete, insert, extract, merge
    // undoing a move of pages of one size is still a change of structure: the outline points at the pages again
    QTreeWidget *outlineTree = nullptr;
    for (QTreeWidget *t : tab->findChildren<QTreeWidget *>())
        if (t->topLevelItemCount() == 6) outlineTree = t;
    CHECK(outlineTree != nullptr);
    QListWidget *pageList = tab->findChild<QListWidget *>();
    tab->rearrange({1, 0, 2, 3, 4, 5});
    CHECK(d->selectionText(0, {72, 80}, {200, 95}).startsWith("Section 2"));
    CHECK(outlineTree->topLevelItem(0)->data(0, Qt::UserRole).toInt() == 1);
    pageList->item(0)->setSelected(true);
    tab->undo();
    CHECK(d->selectionText(0, {72, 80}, {200, 95}).startsWith("Section 1"));
    CHECK(outlineTree->topLevelItem(0)->data(0, Qt::UserRole).toInt() == 0);
    CHECK(pageList->selectedItems().isEmpty());          // the selected row now holds another page: Delete must not hit it
    d->rotatePages({1}, 90);
    CHECK(d->pageSize(1).width() > d->pageSize(1).height());
    d->insertBlank(1, QSizeF(300, 300));
    CHECK(d->pageCount() == 7);
    d->rearrange({0, 2, 3, 4, 5, 6});
    CHECK(d->pageCount() == 6 && pageList->count() == 6);
    tab->undo();                                        // the deleted page comes back, in the Pages list too
    CHECK(d->pageCount() == 7 && pageList->count() == 7);
    tab->redo();
    CHECK(d->pageCount() == 6 && pageList->count() == 6);
    d->extractPages({0, 4}, out + "/extract.pdf");
    Document::mergeFiles({out + "/extract.pdf", sample}, out + "/merged.pdf");
    {
        Document m;
        m.open(out + "/merged.pdf");
        CHECK(m.pageCount() == 8);
        std::printf("     merged inks=%d annots=%d src inks=%d\n", int(m.inkAnnots(0).size()), int(m.annots(0).size()), int(d->inkAnnots(0).size()));
        CHECK(m.inkAnnots(0).size() == 4 && m.annots(0).size() == 6);   // annotations travel with extracted pages
    }
    CHECK(d->insertPdf(out + "/extract.pdf", 6) == 2);
    CHECK(d->pageCount() == 8);
    tab->findChild<QTabWidget *>()->setCurrentIndex(1);
    wait(800);
    w->grab().save(out + "/8_pages_panel.png");

    // the keyboard in the Pages list and in the outline: the arrow keys take the document along, like a click,
    // and Enter hands the keyboard to the page
    {
        auto *side = tab->findChild<QTabWidget *>();
        auto *tree = qobject_cast<QTreeWidget *>(side->widget(0));
        auto *list = qobject_cast<QListWidget *>(side->widget(1));
        w->activateWindow();
        wait(50);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier), enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        v->gotoPage(0, 0, false);
        list->setFocus();
        list->setCurrentRow(2);
        CHECK(list->hasFocus() && v->currentPage() == 2);
        QApplication::sendEvent(list, &down);
        CHECK(v->currentPage() == 3);
        QApplication::sendEvent(list, &enter);
        CHECK(v->hasFocus() && v->currentPage() == 3);
        side->setCurrentIndex(0);
        wait(100);
        auto target = [&](QTreeWidgetItem *it) {      // where a click on the entry takes the view
            const int at = v->verticalScrollBar()->value();
            v->gotoPage(it->data(0, Qt::UserRole).toInt(), it->data(0, Qt::UserRole + 1).toDouble(), false);
            const int to = v->verticalScrollBar()->value();
            v->verticalScrollBar()->setValue(at);
            return to;
        };
        tree->setFocus();
        tree->setCurrentItem(tree->topLevelItem(1));
        CHECK(tree->hasFocus() && v->verticalScrollBar()->value() == target(tree->topLevelItem(1)));
        QApplication::sendEvent(tree, &down);
        CHECK(tree->currentItem() != tree->topLevelItem(1) && v->verticalScrollBar()->value() == target(tree->currentItem()));
        // the selected entry is drawn stronger while its list has the keyboard
        const QImage held = tree->grab().toImage();
        QApplication::sendEvent(tree, &enter);
        CHECK(v->hasFocus() && tree->grab().toImage() != held);
        side->setCurrentIndex(1);
        wait(100);
    }

    // save, reopen: ink survives with widths; the view keeps showing the pages and the search hits meanwhile
    v->gotoPage(0, 0, false);
    v->setSearch("kinetic");
    const QImage shown = settled(v);
    const int hitsBefore = v->searchCount();
    CHECK(tab->save());
    CHECK(v->viewport()->grab().toImage() == shown);
    CHECK(hitsBefore > 0 && v->searchCount() == hitsBefore);
    v->setSearch(QString());
    CHECK(!d->isModified());
    // a fresh history: undo and redo asked for anyway (Undo/Redo out of date) do nothing and change nothing
    {
        int told = 0;
        auto cn = QObject::connect(d, &Document::historyChanged, [&] { ++told; });
        d->undo();
        d->redo();
        QObject::disconnect(cn);
        CHECK(told == 2 && !d->isModified() && !d->canUndo() && !d->canRedo());
    }
    CHECK(d->inkAnnots(0).size() == 4);
    CHECK(!d->inkAnnots(0)[0].data.strokes[0].w.isEmpty());

    // an optimised save renumbers the objects, so the old id of a selected stroke may now name another one: Delete
    // must then remove the selected stroke or nothing, never a different one
    {
        const int sp = 5, chosen = 0;
        for (int k = 0; k < 20; ++k) {
            InkData s;
            s.strokes = {InkStroke{{QPointF(100, 100 + 30 * k), QPointF(400, 100 + 30 * k)}, {}}};
            s.width = 6;
            d->addInk(sp, s);
        }
        auto rowOf = [](const InkAnnot &a) { return qRound((a.data.bounds().center().y() - 100) / 30); };
        auto rows = [&] {
            QVector<int> r;
            for (const InkAnnot &a : d->inkAnnots(sp)) r.push_back(rowOf(a));
            std::sort(r.begin(), r.end());
            return r;
        };
        int oldId = 0;
        for (const InkAnnot &a : d->inkAnnots(sp))
            if (rowOf(a) == chosen) oldId = a.id;
        const QVector<int> before = rows();
        v->gotoPage(sp, 0, false);
        w->setTool(Tool::Select);
        const QPointF at = v->pageToView(sp, {250, 100 + 30.0 * chosen});
        v->testPointer(0, at, -1, 20000);
        v->testPointer(2, at, -1, 20050);
        d->save(d->path(), true);
        bool taken = false;            // the case under test: the old id now belongs to another stroke
        for (const InkAnnot &a : d->inkAnnots(sp)) taken = taken || (a.id == oldId && rowOf(a) != chosen);
        CHECK(taken);
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(v, &del);
        QVector<int> expect = before;
        expect.removeOne(chosen);
        CHECK(rows() == before || rows() == expect);
        CHECK(tab->save());
    }

    // undoing back to the saved state leaves nothing to save, in the title and when closing; redo or another edit
    // changes that again
    {
        auto stroke = [](qreal y) { InkData s; s.strokes = {InkStroke{{QPointF(100, y), QPointF(300, y)}, {}}}; return s; };
        auto starred = [&] { return tab->title().endsWith(" *") && w->windowTitle().contains(" *"); };
        CHECK(!d->isModified() && !starred());
        d->addInk(4, stroke(700));
        CHECK(d->isModified() && starred());
        tab->undo();
        CHECK(!d->isModified() && !starred());
        {
            QTimer answer;            // should the "save changes?" question come up, cancel it rather than hang
            answer.setSingleShot(true);
            QObject::connect(&answer, &QTimer::timeout, [] { if (QWidget *m = QApplication::activeModalWidget()) m->close(); });
            answer.start(1000);
            CHECK(tab->maybeSave());
        }
        tab->redo();
        CHECK(d->isModified() && starred());
        tab->undo();
        d->addInk(4, stroke(720));    // a different edit from the saved state
        CHECK(d->isModified() && !d->canRedo());
        d->addInk(4, stroke(740));
        tab->undo();                  // one of two edits undone: still one to save
        CHECK(d->isModified());
        CHECK(tab->save());
        CHECK(!d->isModified() && !starred());
    }

    // view state round trip, night mode, area copy, reload after an external change
    {
        v->setZoom(1.37);
        v->gotoPage(3, 120, false);
        ViewState vs = v->viewState();
        v->setZoom(0.8);
        v->gotoPage(0, 0, false);
        v->setViewState(vs);
        ViewState back = v->viewState();
        CHECK(back.page == 3 && std::fabs(back.zoom - 1.37) < 1e-9 && std::fabs(back.y - vs.y) < 1.5);
        // dark page theme: dark paper, light text, hues kept
        PdfView::setPageTheme(PageTheme::Dark);
        v->gotoPage(0, 560, false);
        wait(500);
        QImage night = v->viewport()->grab().toImage();
        night.save(out + "/8b_dark.png");
        QColor paper = PageColors::paper();
        CHECK(paper.lightness() < 50 && paper.lightness() > 15);                         // dark grey, not black
        CHECK(PageColors::map(Qt::black).lightness() > 200);                             // text becomes light
        CHECK(PageColors::map(QColor(0, 0, 200)).blue() > PageColors::map(QColor(0, 0, 200)).red() + 60);   // blue stays blue
        PdfView::setPageTheme(PageTheme::Sepia);
        wait(300);
        v->viewport()->grab().save(out + "/8c_sepia.png");
        PdfView::setPageTheme(PageTheme::Light);

        // history: a jump and a long settled scroll can both be undone with "back"
        v->gotoPage(0, 0, false);
        wait(1700);
        v->gotoPage(4, 100);
        CHECK(v->currentPage() == 4 && v->canGoBack());
        v->goBack();
        CHECK(v->currentPage() == 0);
        v->goForward();
        CHECK(v->currentPage() == 4);
        v->verticalScrollBar()->setValue(0);          // scroll far away by hand ...
        wait(1700);                                   // ... and stay there
        v->goBack();
        CHECK(v->currentPage() == 4);

        // pen and mouse keep separate tools: the pen writes, the mouse selects
        v->gotoPage(2, 0, false);
        v->setTools(Tool::Pen, Tool::Select);
        int inkBefore = d->inkAnnots(2).size();
        v->testPointer(0, {300, 300}, -1, 9100);                       // mouse drag: selects, no ink
        v->testPointer(1, {560, 304}, -1, 9110);
        v->testPointer(2, {560, 304}, -1, 9120);
        v->flushInk();
        CHECK(d->inkAnnots(2).size() == inkBefore && v->hasTextSelection());
        // the quick bar appears next to the selection; its first button highlights
        QFrame *bar = v->viewport()->findChild<QFrame *>("selBar");
        CHECK(bar && bar->isVisible());
        v->viewport()->grab().save(out + "/8d_selection_bar.png");
        int annBefore = d->annots(2).size();
        if (bar) bar->findChild<QToolButton *>()->click();
        CHECK(d->annots(2).size() == annBefore + 1 && !v->hasTextSelection());
        v->testPointer(0, {300, 500}, 0.5f, 9200, true);               // pen drag: ink
        for (int i = 1; i <= 30; ++i) v->testPointer(1, QPointF(300 + i * 6, 500 + 10 * std::sin(i * 0.4)), 0.6f, 9200 + i * 8, true);
        v->testPointer(2, {480, 500}, 0, 9500, true);
        v->flushInk();
        CHECK(d->inkAnnots(2).size() == inkBefore + 1);
        v->setTools(Tool::Select, Tool::Select);

        // dragging the corner of a text box scales its font
        {
            AnnotInfo tb;
            for (const AnnotInfo &a : d->annots(0)) if (a.kind == AnnotKind::FreeText) tb = a;
            CHECK(tb.id != 0 && tb.fontSize > 0);
            v->gotoPage(0, qMax(0.0, tb.rect.top() - 150), false);
            wait(100);
            const double k = v->zoom() * 96.0 / 72.0;
            QPointF origin;      // view position of the page's top-left corner, found through a known mapping
            {
                v->startRectPick([&](int, QRectF r) { origin = QPointF(400, 300) - r.topLeft() * k; });
                v->testPointer(0, {400, 300}, -1, 9600);
                v->testPointer(2, {400, 300}, -1, 9610);
            }
            QPointF c = origin + tb.rect.center() * k, br = origin + tb.rect.bottomRight() * k;
            v->testPointer(0, c, -1, 9700);                 // select the box
            v->testPointer(2, c, -1, 9710);
            v->testPointer(0, br, -1, 9800);                // drag its lower right handle outwards
            v->testPointer(1, br + QPointF(120, 60), -1, 9810);
            v->viewport()->grab().save(out + "/8e_text_scaling.png");
            v->testPointer(2, br + QPointF(120, 60), -1, 9820);
            AnnotInfo after;
            for (const AnnotInfo &a : d->annots(0)) if (a.id == tb.id) after = a;
            std::printf("     text size %.1f -> %.1f\n", tb.fontSize, after.fontSize);
            CHECK(after.fontSize > tb.fontSize * 1.2);
        }
        v->copyAreaAsImage();
        v->testPointer(0, {300, 200}, -1, 9000);
        v->testPointer(1, {500, 300}, -1, 9010);
        v->testPointer(2, {500, 300}, -1, 9020);
        CHECK(!QApplication::clipboard()->image().isNull());
        CHECK(!d->reloadIfChanged());                 // nothing changed on disk
        QFile::remove(d->path());
        QFile::copy(sample, d->path());               // "LaTeX" rewrote the file
        CHECK(d->reloadIfChanged() && d->pageCount() == 6);
    }

    // PDF creator: whole files, zipper, hand-arranged pages, images
    {
        QImage pic(600, 400, QImage::Format_RGB32);
        pic.fill(QColor(200, 60, 60));
        pic.save(out + "/pic.png");
        auto *c = new CreatorWindow;
        c->resize(1100, 760);
        c->show();
        c->addFiles({sample, out + "/extract.pdf", out + "/pic.png"});
        CHECK(c->pageCount() == 6 + 2 + 1);
        c->setMode(CreatorWindow::Zipper);
        auto z = c->items();
        CHECK(z.size() == 9 && z[0].path.endsWith("sample.pdf") && z[0].page == 0 && z[1].path.endsWith("extract.pdf") && z[1].page == 0 &&
              z[2].image && z[3].path.endsWith("sample.pdf") && z[3].page == 1 && z[4].path.endsWith("extract.pdf") && z[4].page == 1 && z[5].page == 2);
        c->setReverseSecond(true);
        CHECK(c->items()[1].page == 1 && c->items()[4].page == 0);
        c->setMode(CreatorWindow::ArrangePages);
        c->movePage(8, 0);
        CHECK(c->items()[0].path.endsWith("sample.pdf") == false || c->items()[0].page != 0);
        wait(600);
        c->grab().save(out + "/8f_creator.png");
        c->setMode(CreatorWindow::WholeFiles);
        CHECK(c->build(out + "/created.pdf"));
        Document made;
        made.open(out + "/created.pdf");
        CHECK(made.pageCount() == 9 && made.pageSize(8).width() > made.pageSize(8).height());
        made.render(8, 0.5, QRect(0, 0, 225, 150)).save(out + "/8g_image_page.png");
        CHECK(QColor(made.render(8, 0.5, QRect(0, 0, 225, 150)).pixel(100, 70)).red() > 150);
        c->close();
    }

    // printing: actual size on the paper, turned for landscape, shrunk only when too large
    {
        const QRectF a4(0, 0, 595.28, 841.89), a4p(10, 10, 575.28, 821.89);
        PrintPlacement pl = DocumentTab::printPlacement(QSizeF(595.28, 841.89), a4, a4p);
        CHECK(!pl.rotated && std::fabs(pl.scale - 1) < 1e-9 && std::fabs(pl.target.left()) < 1e-6 && std::fabs(pl.target.width() - 595.28) < 1e-6);
        pl = DocumentTab::printPlacement(QSizeF(419.53, 595.28), a4, a4p);  // A5 on A4: actual size, centred
        CHECK(!pl.rotated && pl.scale == 1 && std::fabs(pl.target.center().y() - a4.center().y()) < 1e-6 && pl.target.top() > 0);
        pl = DocumentTab::printPlacement(QSizeF(841.89, 595.28), a4, a4p);   // landscape A4: turned, actual size
        CHECK(pl.rotated && std::fabs(pl.scale - 1) < 1e-9);
        pl = DocumentTab::printPlacement(QSizeF(841.89, 1190.55), a4, a4p);  // A3 on A4: into the printable area
        std::printf("     A3 on A4: scale %.3f, target %.1f,%.1f %.1fx%.1f\n", pl.scale, pl.target.x(), pl.target.y(), pl.target.width(), pl.target.height());
        CHECK(!pl.rotated && pl.scale < 0.7 && a4p.adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(pl.target));

        // end to end: print page 1 to a PDF on paper of its own size; it must come out at the same size
        // and position as the page itself, not shrunk into the printable area
        const QSizeF ps = d->pageSize(0);
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(out + "/printed.pdf");
        printer.setPageSize(QPageSize(ps, QPageSize::Point));
        printer.setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Point);
        tab->printPages(printer, 0, 0);
        Document printed;
        printed.open(out + "/printed.pdf");
        const QSizeF pps = printed.pageSize(0);
        const double s = 1.0;
        const QRect clip(0, 0, int(ps.width() * s), int(ps.height() * s));
        QImage a = d->render(0, s, clip).convertToFormat(QImage::Format_Grayscale8);
        QImage b = printed.render(0, s, clip).convertToFormat(QImage::Format_Grayscale8);
        auto inkBox = [](const QImage &im) {   // bounding box of the dark pixels
            QRect r;
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x)
                    if (qGray(im.pixel(x, y)) < 128) r |= QRect(x, y, 1, 1);
            return r;
        };
        const QRect ia = inkBox(a), ib = inkBox(b);
        std::printf("     printed %.0fx%.0f pt page on %.0fx%.0f pt paper; ink at %d,%d-%d,%d, original %d,%d-%d,%d\n", ps.width(), ps.height(),
                    pps.width(), pps.height(), ib.left(), ib.top(), ib.right(), ib.bottom(), ia.left(), ia.top(), ia.right(), ia.bottom());
        CHECK(printed.pageCount() == 1 && std::fabs(pps.width() - ps.width()) < 1 && std::fabs(pps.height() - ps.height()) < 1);
        // shrunk into the printable area (as before) the ink would be several points off at the edges
        CHECK(!ia.isEmpty() && std::abs(ia.left() - ib.left()) <= 1 && std::abs(ia.top() - ib.top()) <= 1 &&
              std::abs(ia.right() - ib.right()) <= 1 && std::abs(ia.bottom() - ib.bottom()) <= 1);
        // a portrait page on landscape paper is turned a quarter clockwise, still at actual size:
        // the page's left margin becomes the top margin
        QPrinter land(QPrinter::HighResolution);
        land.setOutputFormat(QPrinter::PdfFormat);
        land.setOutputFileName(out + "/printed-land.pdf");
        land.setPageSize(QPageSize(QSizeF(ps.height(), ps.width()), QPageSize::Point));
        land.setPageOrientation(QPageLayout::Portrait);
        tab->printPages(land, 0, 0);
        Document turned;
        turned.open(out + "/printed-land.pdf");
        const QSizeF tps = turned.pageSize(0);
        const QRect ic = inkBox(turned.render(0, 1.0, QRect(0, 0, int(tps.width()), int(tps.height()))).convertToFormat(QImage::Format_Grayscale8));
        std::printf("     turned onto %.0fx%.0f pt paper: ink at %d,%d-%d,%d\n", tps.width(), tps.height(), ic.left(), ic.top(), ic.right(), ic.bottom());
        CHECK(tps.width() > tps.height());
        CHECK(std::abs(ic.top() - ia.left()) <= 1 && std::abs(ic.bottom() - ia.right()) <= 1 &&
              std::abs(int(tps.width()) - 1 - ic.left() - ia.bottom()) <= 2 && std::abs(int(tps.width()) - 1 - ic.right() - ia.top()) <= 2);
    }

    // tabs: second document, tear off into a new window, drop back
    QFile::remove(out + "/second.pdf");
    QFile::copy(sample, out + "/second.pdf");
    CHECK(w->openFile(out + "/second.pdf"));
    CHECK(w->tabCount() == 2);
    {       // Ctrl+PgDown and Ctrl+PgUp go round the open documents
        QAction *next = nullptr, *previous = nullptr;
        for (QAction *a : w->findChildren<QAction *>()) {
            if (a->shortcut() == QKeySequence("Ctrl+PgDown")) next = a;
            if (a->shortcut() == QKeySequence("Ctrl+PgUp")) previous = a;
        }
        DocumentTab *second = w->currentTab();
        CHECK(next && previous);
        if (next && previous) {
            next->trigger();
            CHECK(w->currentTab() != second);
            next->trigger();
            CHECK(w->currentTab() == second);
            previous->trigger();
            CHECK(w->currentTab() != second);
            previous->trigger();
            CHECK(w->currentTab() == second);
        }
    }
    MainWindow *w2 = w->detachTab(1, QPoint(200, 200));
    CHECK(w2 && w2 != w && MainWindow::windows().size() == 2 && w->tabCount() == 1 && w2->tabCount() == 1);
    wait(300);
    w2->grab().save(out + "/9_detached_window.png");
    w->adoptTab(w2->currentTab(), 0);
    std::printf("     w2 visible=%d\n", int(w2->isVisible()));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    wait(100);
    std::printf("     tabs=%d windows=%d\n", w->tabCount(), int(MainWindow::windows().size()));
    CHECK(w->tabCount() == 2 && MainWindow::windows().size() == 1);

    // where the cards of the notes go: they give way downwards, but the selected one sits at its marking
    CHECK(stackNotes({10, 20, 200}, {50, 50, 50}, 8) == QVector<qreal>({10, 68, 200}));
    CHECK(stackNotes({10, 20, 200}, {50, 50, 50}, 8, 1) == QVector<qreal>({-38, 20, 200}));
    CHECK(stackNotes({10, 20, 30}, {50, 50, 50}, 8, 2) == QVector<qreal>({-86, -28, 30}));
    CHECK(stackNotes({10, 100, 300}, {50, 50, 50}, 8, 2) == QVector<qreal>({10, 100, 300}));         // nothing in the way

    // notes on markings: highlight -> note editor opens in the Notes tab -> typed text is stored
    {
        DocumentTab *nt = w->currentTab();
        PdfView *nv = nt->view();
        Document *nd = nt->doc();
        nv->setTools(Tool::Select, Tool::Select);
        nv->gotoPage(1, 100, false);
        wait(200);
        PdfView::setNoteAfterMarking(true);
        Document::setAuthor("Test Author");
        QPointF a = nv->pageToView(1, QPointF(75, 255)), b = nv->pageToView(1, QPointF(330, 262));
        nv->testPointer(0, a, -1, 30000);
        nv->testPointer(1, b, -1, 30010);
        nv->testPointer(2, b, -1, 30020);
        CHECK(nv->hasTextSelection());
        int id = nv->markupSelection(AnnotKind::Highlight, QColor(255, 235, 60));
        CHECK(id > 0);
        wait(50);
        NotesPanel *np = nt->notesPanel();
        NoteCard *card = np->card(id);
        CHECK(card && card->isEditing() && np->isVisible());
        QPlainTextEdit *ed = card ? card->findChild<QPlainTextEdit *>() : nullptr;
        CHECK(ed && ed->hasFocus());
        if (ed) {
            ed->setPlainText("Check this functional against the Thomas-Fermi limit.");
            QKeyEvent done(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
            QApplication::sendEvent(ed, &done);
        }
        wait(300);
        QString stored;
        for (const MarkupNote &m : nd->markups()) if (m.id == id) stored = m.text;
        CHECK(stored.startsWith("Check this functional"));
        // the note records who wrote it and when, and its card shows both
        auto noteMeta = [&](int which) {
            for (const MarkupNote &m : nd->markups()) if (m.id == which) return m;
            return MarkupNote();
        };
        CHECK(noteMeta(id).author == "Test Author" && std::llabs(noteMeta(id).modified.secsTo(QDateTime::currentDateTime())) < 120);
        bool named = false;
        for (QLabel *l : card->findChildren<QLabel *>()) named = named || (l->isVisible() && l->text().startsWith("Test Author · "));
        CHECK(named);
        Document::setAuthor("Someone Else");              // a later edit by somebody else keeps the author
        nd->setAnnotContents(1, id, stored);
        CHECK(noteMeta(id).author == "Test Author");
        Document::setAuthor("Test Author");
        wait(300);
        // a card looks the same while it is edited; a long note scrolls instead of growing on
        if (card && ed) {
            int shown = card->height();
            card->startEditing();
            wait(50);
            CHECK(card->isEditing() && card->height() == shown);
            ed->setPlainText(QString("A long note that goes on. ").repeated(60));
            wait(50);
            CHECK(ed->verticalScrollBar()->maximum() > 0 && card->height() < 320);
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(ed, &esc);
            wait(100);
            CHECK(!card->isEditing() && card->height() == shown && ed->verticalScrollBar()->maximum() == 0);
        }
        // a second note further down, written by clicking an existing marking
        PdfView::setNoteAfterMarking(false);
        a = nv->pageToView(1, QPointF(75, 418)); b = nv->pageToView(1, QPointF(300, 424));
        nv->testPointer(0, a, -1, 31000);
        nv->testPointer(1, b, -1, 31010);
        nv->testPointer(2, b, -1, 31020);
        int id2 = nv->markupSelection(AnnotKind::Highlight, QColor(140, 235, 130));
        wait(300);
        CHECK(id2 > 0 && !np->card(id2));                 // setting off: no editor yet
        QPointF c = nv->pageToView(1, QPointF(150, 421));
        nv->testPointer(0, c, -1, 32000);
        nv->testPointer(2, c, -1, 32010);
        wait(50);
        CHECK(!np->card(id2));                            // a click only selects the marking
        // Enter opens a note for it; given up with Esc, Enter must open one again
        for (int round = 0; round < 2; ++round) {
            QKeyEvent open(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), giveUp(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(round == 0 ? nv : QApplication::focusWidget(), &open);
            wait(50);
            CHECK(np->card(id2) && np->card(id2)->isEditing());
            if (np->card(id2)) QApplication::sendEvent(np->card(id2)->findChild<QPlainTextEdit *>(), &giveUp);
            wait(300);
            CHECK(!np->card(id2));
        }
        QMouseEvent dbl(QEvent::MouseButtonDblClick, c, nv->viewport()->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(nv->viewport(), &dbl);
        wait(50);
        CHECK(np->card(id2) && np->card(id2)->isEditing());
        if (np->card(id2)) {
            auto *ed2 = np->card(id2)->findChild<QPlainTextEdit *>();
            ed2->setPlainText("Second note: compare with the gradient expansion.");
            QKeyEvent done(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
            QApplication::sendEvent(ed2, &done);
        }
        wait(400);
        CHECK(np->noteCount() == 2);
        // cards sit at the height of their markings
        NoteCard *c1 = np->card(id), *c2 = np->card(id2);
        if (c1 && c2) {
            QRectF q1;
            for (const MarkupNote &m : nd->markups()) if (m.id == id) q1 = m.quads.first().boundingRect();
            QPoint anchor1 = np->mapFromGlobal(nv->viewport()->mapToGlobal(nv->pageToView(1, QPointF(q1.left(), q1.bottom())).toPoint()));
            std::printf("     card1 y=%d anchor y=%d, card2 y=%d\n", c1->y(), anchor1.y(), c2->y());
            CHECK(std::abs(c1->y() + 14 - anchor1.y()) < 4 && c2->y() > c1->y() + c1->height());
        }
        // a selected marking lights its note up, and so does the pointer on a marking or on a card
        if (c1 && c2) {
            CHECK(!c1->isLit() && c2->isLit());           // the second marking is still selected
            QPointF off = nv->pageToView(1, QPointF(40, 330));
            nv->testPointer(0, off, -1, 32500);
            nv->testPointer(2, off, -1, 32510);
            CHECK(!c1->isLit() && !c2->isLit());
            QPointF on = nv->pageToView(1, QPointF(150, 421));
            QMouseEvent mv(QEvent::MouseMove, on, nv->viewport()->mapToGlobal(on), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(nv->viewport(), &mv);
            CHECK(!c1->isLit() && c2->isLit());
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(nv, &leave);
            CHECK(!c2->isLit());
            // the pen (holding the select tool): one tap selects the marking, a second tap opens the editor
            QPointF m1 = nv->pageToView(1, QPointF(150, 258));
            QTabletEvent near(QEvent::TabletMove, QPointingDevice::primaryPointingDevice(), m1, nv->viewport()->mapToGlobal(m1),
                              0, 0, 0, 0, 0, 0, Qt::NoModifier, Qt::NoButton, Qt::NoButton);
            QApplication::sendEvent(nv->viewport(), &near);
            w->setTool(Tool::Select);
            nv->testPointer(0, m1, 0.5f, 33000, true);
            nv->testPointer(2, m1, 0, 33010, true);
            CHECK(c1->isLit() && !c1->isEditing());
            nv->testPointer(0, m1, 0.5f, 33200, true);
            nv->testPointer(2, m1, 0, 33210, true);
            wait(50);
            CHECK(c1->isEditing());
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(c1->findChild<QPlainTextEdit *>(), &esc);
            wait(100);
            // Del removes the selected marking, and its note with it
            CHECK(c1->isLit() && !c1->isEditing());
            QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(nv, &del);
            wait(400);
            CHECK(!np->card(id) && np->noteCount() == 1);
            nt->undo();
            wait(400);
            CHECK(np->noteCount() == 2);
            nv->testPointer(0, off, -1, 34000);           // back to the mouse
            nv->testPointer(2, off, -1, 34010);
            // going from one marking to the other by clicks on the page selects the new one in one step, without
            // nothing being selected in between (the Notes sidebar would lay its cards out twice)
            {
                QApplication::sendEvent(nv, &leave);      // the pen hovered over the first marking
                QPointF first = nv->pageToView(1, QPointF(150, 258));
                nv->testPointer(0, first, -1, 34600);
                nv->testPointer(2, first, -1, 34610);
                QVector<int> told;
                auto conn = QObject::connect(nv, &PdfView::noteSelected, [&](int n) { told.push_back(n); });
                nv->testPointer(0, on, -1, 34700);
                nv->testPointer(2, on, -1, 34710);
                QObject::disconnect(conn);
                CHECK(told == QVector<int>({id2}));
                nv->testPointer(0, off, -1, 34800);       // beside any marking: nothing selected
                nv->testPointer(2, off, -1, 34810);
                CHECK(!np->card(id)->isLit() && !np->card(id2)->isLit());
            }
            // writing another note moves the selection to its marking
            QApplication::sendEvent(nv, &leave);          // the pen hovered over the first marking
            // undo and redo keep the selection in the view while the marking is still there unchanged, and the
            // sidebar follows the view either way
            nv->testPointer(0, on, -1, 34500);
            nv->testPointer(2, on, -1, 34510);
            CHECK(np->card(id2) && np->card(id2)->isLit());
            nd->undo();
            nd->redo();
            wait(400);
            CHECK(np->card(id2) && np->card(id2)->isLit());
            nv->selectAnnot(1, 0);                        // the view lets go: so does the sidebar
            wait(50);
            CHECK(np->card(id2) && !np->card(id2)->isLit());
            nv->testPointer(0, on, -1, 35000);
            nv->testPointer(2, on, -1, 35010);
            NoteCard *n1 = np->card(id), *n2 = np->card(id2);
            CHECK(n1 && n2 && n2->isLit() && !n1->isLit());
            if (n1 && n2) {
                QPointF in(n1->width() / 2.0, 6);
                QMouseEvent click(QEvent::MouseButtonPress, in, n1->mapToGlobal(in), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(n1, &click);
                wait(50);
                CHECK(n1->isLit() && !n1->isEditing() && !n2->isLit() && np->hasFocus());     // beside the text: selected only
                // the selected note is tinted in its marking's colour while the sidebar has the keyboard
                const QImage held = n1->grab().toImage();
                const QPoint inside(n1->width() - 6, n1->height() / 2);       // card background, clear of text and outline
                nv->setFocus();
                const QImage plain = n1->grab().toImage();
                CHECK(n1->isLit() && plain.pixel(inside) != held.pixel(inside));
                np->setFocus();
                CHECK(n1->grab().toImage() == held);
                // with the keyboard in the sidebar, Up and Down go from note to note and Enter opens one
                QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier), up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(np, &down);
                CHECK(n2->isLit() && !n1->isLit());
                QApplication::sendEvent(np, &down);           // the last note: stays
                QApplication::sendEvent(np, &up);
                CHECK(n1->isLit() && !n2->isLit());
                QApplication::sendEvent(np, &enter);
                wait(50);
                CHECK(n1->isEditing());
                QApplication::sendEvent(n1->findChild<QPlainTextEdit *>(), &esc);
                wait(100);
                CHECK(n1->isLit() && !n1->isEditing() && !n2->isLit() && np->hasFocus());
                // Enter opens the note of the selected marking also with the keyboard on the page
                nv->setFocus();
                QApplication::sendEvent(nv, &enter);
                wait(50);
                CHECK(n1->isEditing());
                QApplication::sendEvent(n1->findChild<QPlainTextEdit *>(), &esc);
                wait(200);
                // a click on the text itself opens the note
                QWidget *text = n2->findChild<QPlainTextEdit *>()->viewport();
                QMouseEvent type(QEvent::MouseButtonPress, QPointF(5, 5), text->mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(text, &type);
                wait(50);
                CHECK(n2->isEditing() && n2->isLit() && !n1->isLit());
                QApplication::sendEvent(n2->findChild<QPlainTextEdit *>(), &esc);
                wait(100);
                // a click on the text of another note while one is being written moves on to that note: it must not
                // close both editors
                auto typeInto = [&](NoteCard *c) {
                    QWidget *tv = c->findChild<QPlainTextEdit *>()->viewport();
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), tv->mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(tv, &press);
                    wait(200);
                };
                typeInto(n1);
                CHECK(n1->isEditing() && !n2->isEditing());
                CHECK(n1->grab().toImage().pixel(inside) == held.pixel(inside));       // the note being written is tinted the same
                typeInto(n2);
                CHECK(n2->isEditing() && n2->findChild<QPlainTextEdit *>()->hasFocus() && !n1->isEditing());
                typeInto(n1);
                CHECK(n1->isEditing() && n1->findChild<QPlainTextEdit *>()->hasFocus() && !n2->isEditing());
                QApplication::sendEvent(n1->findChild<QPlainTextEdit *>(), &esc);
                wait(100);
                // Del removes the selected note, not its marking, and the next note becomes the selected one
                QApplication::sendEvent(n1, &click);
                QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
                QApplication::sendEvent(np, &remove);
                wait(400);
                bool marked = false;
                for (const AnnotInfo &a : nd->annots(1)) marked = marked || a.id == id;
                CHECK(np->noteCount() == 1 && !np->card(id) && marked && n2->isLit());
                nd->undo();
                wait(400);
                CHECK(np->noteCount() == 2 && np->card(id));
            }
        }
        nt->window()->grab().save(out + "/9c_notes.png");
        // notes outside the part of the document in view: a hint says how many, a click goes to the nearest
        {
            auto hint = [&](const QString &arrow) -> QToolButton * {
                for (QToolButton *b : np->findChildren<QToolButton *>())
                    if (b->isVisible() && b->text().startsWith(arrow)) return b;
                return nullptr;
            };
            CHECK(!hint("↑") && !hint("↓"));
            nv->gotoPage(4, 0, false);
            wait(200);
            QToolButton *up = hint("↑");
            CHECK(up && up->text() == "↑ 2 notes above" && !hint("↓"));
            if (up) up->click();
            wait(200);
            CHECK(nv->currentPage() == 1 && np->card(id2) && np->card(id2)->isVisible() && hint("↑") && hint("↑")->text() == "↑ 1 note above");
            CHECK(np->card(id2)->isLit() && !np->card(id)->isLit());       // the note it went to is the selected one
            nv->gotoPage(0, 0, false);
            wait(200);
            CHECK(hint("↓") && hint("↓")->text() == "↓ 2 notes below");
            nt->window()->grab().save(out + "/9h_hint.png");
            nv->gotoPage(1, 100, false);
            wait(200);
        }
        // Down goes through the notes in the order their cards are stacked in, also where two notes sit on one text
        // line and the right one was made first
        {
            const int right = nd->addMarkup(1, AnnotKind::Highlight, {QPolygonF(QRectF(260, 322, 70, 12))}, QColor(140, 235, 130));
            nd->setAnnotContents(1, right, "Right half of the line.");
            const int left = nd->addMarkup(1, AnnotKind::Highlight, {QPolygonF(QRectF(80, 322, 70, 12))}, QColor(255, 235, 60));
            nd->setAnnotContents(1, left, "Left half of the line.");
            wait(400);
            QPointF off = nv->pageToView(1, QPointF(40, 600));
            nv->testPointer(0, off, -1, 36000);               // nothing selected: the cards are where they are by default
            nv->testPointer(2, off, -1, 36010);
            wait(200);
            QVector<int> stacked = {id, id2, left, right};
            std::sort(stacked.begin(), stacked.end(), [&](int a, int b) { return np->card(a)->y() < np->card(b)->y(); });
            QVector<int> walked;
            np->setFocus();
            QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            for (int i = 0; i < 4; ++i) {
                QApplication::sendEvent(np, &down);
                wait(200);
                for (int n : {id, id2, left, right})
                    if (np->card(n)->isLit()) walked.push_back(n);
            }
            std::printf("     stacked %d %d %d %d, walked %d %d %d %d (left %d, right %d)\n", stacked[0], stacked[1], stacked[2], stacked[3],
                        walked.value(0), walked.value(1), walked.value(2), walked.value(3), left, right);
            CHECK(walked == stacked && stacked.indexOf(left) + 1 == stacked.indexOf(right));
            nv->testPointer(0, off, -1, 37000);
            nv->testPointer(2, off, -1, 37010);
            nd->removeAnnots(1, {left, right});
            wait(400);
        }
        PdfView::setNoteAfterMarking(true);
    }

    // sticky notes: the tool puts an icon on the page and opens its note in the Notes sidebar
    {
        DocumentTab *nt = w->currentTab();
        PdfView *nv = nt->view();
        Document *nd = nt->doc();
        NotesPanel *np = nt->notesPanel();
        const int before = np->noteCount();
        auto stickies = [&] {
            QVector<AnnotInfo> v;
            for (const AnnotInfo &a : nd->annots(1)) if (a.kind == AnnotKind::Note) v.push_back(a);
            return v;
        };
        auto key = [&](QWidget *to, int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
            QKeyEvent e(QEvent::KeyPress, k, m);
            QApplication::sendEvent(to, &e);
        };
        QPointF at = nv->pageToView(1, QPointF(420, 330));
        w->setTool(Tool::Note);
        nv->testPointer(0, at, -1, 40000);
        nv->testPointer(2, at, -1, 40010);
        wait(50);
        CHECK(stickies().size() == 1 && nv->tool() == Tool::Select);
        int sid = stickies().value(0).id;
        CHECK(np->card(sid) && np->card(sid)->isEditing());
        if (np->card(sid)) key(np->card(sid)->findChild<QPlainTextEdit *>(), Qt::Key_Escape);
        wait(300);
        CHECK(stickies().isEmpty() && !np->card(sid));     // placed but not written: gone again
        w->setTool(Tool::Note);
        nv->testPointer(0, at, -1, 41000);
        nv->testPointer(2, at, -1, 41010);
        wait(50);
        sid = stickies().value(0).id;
        if (NoteCard *sc = np->card(sid)) {
            auto *ed = sc->findChild<QPlainTextEdit *>();
            ed->setPlainText("Ask about the boundary term.");
            key(ed, Qt::Key_Return, Qt::ControlModifier);
        }
        wait(300);
        CHECK(stickies().size() == 1 && stickies().value(0).text == "Ask about the boundary term." && np->noteCount() == before + 1);
        // a click selects it, a drag moves it, a double-click opens its note
        NoteCard *sc = np->card(sid);
        QRectF r0 = stickies().value(0).rect;
        for (const MarkupNote &m : nd->markups())            // the sidebar's rectangle is where the icon is drawn
            if (m.id == sid) CHECK(QLineF(m.rect.topLeft(), r0.topLeft()).length() < 0.6 && std::fabs(m.rect.width() - r0.width()) < 0.6);
        nv->testPointer(0, at, -1, 42000);
        nv->testPointer(2, at, -1, 42010);
        CHECK(sc && sc->isLit() && !sc->isEditing());
        QPointF to = at + QPointF(60, 40);
        nv->testPointer(0, at, -1, 43000);
        nv->testPointer(1, to, -1, 43010);
        nv->testPointer(2, to, -1, 43020);
        wait(300);
        QRectF r1 = stickies().value(0).rect;
        qreal ppp = nv->zoom() * 96.0 / 72.0;
        std::printf("     sticky note moved by %.1f, %.1f pt\n", r1.x() - r0.x(), r1.y() - r0.y());
        CHECK(std::fabs(r1.x() - r0.x() - 60 / ppp) < 1 && std::fabs(r1.y() - r0.y() - 40 / ppp) < 1 && r1.size() == r0.size());
        CHECK(stickies().value(0).id == sid && stickies().value(0).text.startsWith("Ask about"));
        QMouseEvent dbl(QEvent::MouseButtonDblClick, to, nv->viewport()->mapToGlobal(to), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(nv->viewport(), &dbl);
        wait(50);
        sc = np->card(sid);
        CHECK(sc && sc->isEditing());
        if (sc) key(sc->findChild<QPlainTextEdit *>(), Qt::Key_Escape);
        wait(300);
        CHECK(stickies().size() == 1);                       // a written note survives Esc
        // so does an empty sticky note that was already in the file, e.g. from another program
        {
            int old = nd->addNote(1, QPointF(470, 250), QColor(255, 235, 60));
            wait(300);
            QPointF on = nv->pageToView(1, QPointF(470, 250));
            QMouseEvent open(QEvent::MouseButtonDblClick, on, nv->viewport()->mapToGlobal(on), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(nv->viewport(), &open);
            wait(50);
            CHECK(np->card(old) && np->card(old)->isEditing());
            if (np->card(old)) key(np->card(old)->findChild<QPlainTextEdit *>(), Qt::Key_Escape);
            wait(300);
            CHECK(stickies().size() == 2);
            nd->removeAnnots(1, {old});
            wait(300);
        }
        sc = np->card(sid);
        CHECK(sc && sc->isVisible() && sc->height() > 20);  // a card without a quoted excerpt
        nt->window()->grab().save(out + "/9d_sticky.png");
    }

    // two taps with the pen on a sticky note open it even when the pen wobbles a little between down and up, and
    // the note stays where it is; a real drag still moves it
    {
        DocumentTab *nt = w->currentTab();
        PdfView *nv = nt->view();
        Document *nd = nt->doc();
        NotesPanel *np = nt->notesPanel();
        w->setTool(Tool::Select);
        nv->gotoPage(1, 150, false);
        wait(200);
        const int jid = nd->addNote(1, QPointF(470, 250), QColor(255, 235, 60));
        nd->setAnnotContents(1, jid, "wobble");
        wait(300);
        nv->selectAnnot(1, 0);
        const QPointF on = nv->pageToView(1, QPointF(470, 250));
        auto rectOf = [&] { for (const AnnotInfo &a : nd->annots(1)) if (a.id == jid) return a.rect; return QRectF(); };
        const QRectF before = rectOf();
        bool opened = false;
        auto cn = QObject::connect(nv, &PdfView::noteRequested, [&](int, int id) { opened = opened || id == jid; });
        qint64 t = 60000;
        for (int tap = 0; tap < 2; ++tap, t += 150) {
            nv->testPointer(0, on, 0.5f, t, true);
            nv->testPointer(1, on + QPointF(1.5, 1), 0.55f, t + 20, true);
            nv->testPointer(2, on + QPointF(1.5, 1), 0, t + 40, true);
        }
        QObject::disconnect(cn);
        wait(100);
        CHECK(opened && rectOf() == before);
        if (np->card(jid) && np->card(jid)->isEditing()) {
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(np->card(jid)->findChild<QPlainTextEdit *>(), &esc);
        }
        wait(200);
        nv->testPointer(0, on, -1, t + 1000);
        nv->testPointer(1, on + QPointF(30, 20), -1, t + 1020);
        nv->testPointer(2, on + QPointF(30, 20), -1, t + 1040);
        CHECK(rectOf().topLeft() != before.topLeft());
        nd->removeAnnots(1, {jid});
        wait(300);
    }

    // an optimised save renumbers the objects, so an id may afterwards name another marking: its card must quote
    // the text of the marking it now stands for, not what was read for the id before
    {
        DocumentTab *nt = w->currentTab();
        Document *nd = nt->doc();
        NotesPanel *np = nt->notesPanel();
        const QVector<QPolygonF> q1 = nd->search(2, "the"), q2 = nd->search(2, "of");
        CHECK(!q1.isEmpty() && !q2.isEmpty());
        if (!q1.isEmpty() && !q2.isEmpty()) {
            const int h1 = nd->addMarkup(2, AnnotKind::Highlight, {q1[0]}, QColor(255, 235, 60));
            nd->setAnnotContents(2, h1, "first");
            const int h2 = nd->addMarkup(2, AnnotKind::Highlight, {q2[0]}, QColor(255, 235, 60));
            nd->setAnnotContents(2, h2, "second");
            wait(400);
            nd->removeAnnots(2, {h1});
            wait(400);
            nd->save(nd->path(), true);
            wait(400);
            MarkupNote second;
            for (const MarkupNote &m : nd->markups()) if (m.text == "second") second = m;
            std::printf("     second marking: id %d -> %d (first had %d)\n", h2, second.id, h1);
            const QString quoted = nd->textInQuads(second.page, second.quads);
            NoteCard *c = np->card(second.id);
            bool right = false;
            for (QLabel *l : c ? c->findChildren<QLabel *>() : QList<QLabel *>()) right = right || l->text() == QStringLiteral("“%1”").arg(quoted);
            CHECK(second.id > 0 && !quoted.isEmpty() && right);
            nd->removeAnnots(second.page, {second.id});
            wait(300);
        }
    }

    // notes on pen strokes: a double-click opens the note of a stroke; it stays with the stroke and goes with it
    {
        DocumentTab *nt = w->currentTab();
        PdfView *nv = nt->view();
        Document *nd = nt->doc();
        NotesPanel *np = nt->notesPanel();
        const int before = np->noteCount();
        InkData k;
        InkStroke st;
        for (int j = 0; j <= 20; ++j) st.pts.push_back(QPointF(380 + j * 5, 470 + 6 * std::sin(j * 0.6)));
        k.strokes.push_back(st);
        k.color = QColor(200, 30, 30);
        k.width = 2;
        nv->flushInk();
        int ink = nd->addInk(1, k);
        wait(300);
        CHECK(ink > 0 && !np->card(ink) && np->noteCount() == before);     // no note, no card
        auto noteOf = [&](int id) {
            for (const AnnotInfo &a : nd->annots(1)) if (a.id == id) return a.text;
            return QString("(gone)");
        };
        w->setTool(Tool::Select);
        QPointF on = nv->pageToView(1, st.pts[10]);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, on, nv->viewport()->mapToGlobal(on), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(nv->viewport(), &dbl);
        wait(50);
        NoteCard *ic = np->card(ink);
        CHECK(ic && ic->isEditing());
        if (ic) {
            auto *ed = ic->findChild<QPlainTextEdit *>();
            ed->setPlainText("Sign error here?");
            QKeyEvent done(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
            QApplication::sendEvent(ed, &done);
        }
        wait(300);
        CHECK(noteOf(ink) == "Sign error here?" && np->noteCount() == before + 1);
        // the line from the note ends on the stroke, at its leftmost point, not at a corner of its box
        for (const MarkupNote &m : nd->markups())
            if (m.id == ink) CHECK(QLineF(m.anchor, st.pts.first() - QPointF(k.width / 2 + 1.5, 0)).length() < 0.5);     // beside it
        // and runs below the text line it would strike through; where there is no text, straight
        CHECK(nd->clearOfText(1, st.pts.first()) > st.pts.first().y() + 1 && nd->clearOfText(1, QPointF(40, 330)) == 330);
        // the pointer lights the note where a click would select the stroke, not anywhere in its box
        if (NoteCard *sc = np->card(ink)) {
            QPointF away = nv->pageToView(1, QPointF(40, 330));
            nv->testPointer(0, away, -1, 49000);              // writing the note had selected the stroke
            nv->testPointer(2, away, -1, 49010);
            auto moveTo = [&](QPointF pagePt) {
                QPointF v = nv->pageToView(1, pagePt);
                QMouseEvent mv(QEvent::MouseMove, v, nv->viewport()->mapToGlobal(v), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(nv->viewport(), &mv);
            };
            moveTo(QPointF(420, 475.5));                      // inside the box, off the line
            CHECK(!sc->isLit());
            moveTo(st.pts[8]);
            CHECK(sc->isLit());
            moveTo(QPointF(40, 330));
            CHECK(!sc->isLit());
        }
        // a note does not change how a stroke is drawn (a highlighter stroke went opaque over the text)
        {
            InkData h;
            InkStroke hs;
            for (int j = 0; j <= 10; ++j) hs.pts.push_back(QPointF(100 + j * 12, 560));
            hs.w.fill(9.f, hs.pts.size());
            h.strokes.push_back(hs);
            h.color = QColor(255, 235, 60);
            h.width = 9;
            h.highlighter = true;
            int hid = nd->addInk(1, h);
            const QRect clip(150, 1040, 500, 160);
            QImage plain = nd->render(1, 2.0, clip);
            nd->setAnnotContents(1, hid, "Marked with the highlighter.");
            CHECK(!plain.isNull() && nd->render(1, 2.0, clip) == plain);
            wait(300);
            QPointF v = nv->pageToView(1, hs.pts[5]);         // for the picture: the pointer on it, its outline shown
            QMouseEvent mv(QEvent::MouseMove, v, nv->viewport()->mapToGlobal(v), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(nv->viewport(), &mv);
            CHECK(np->card(hid) && np->card(hid)->isLit());
            nt->window()->grab().save(out + "/9g_highlighter_lit.png");
            // removing the note again leaves the stroke as it was, and no empty comment behind
            nd->setAnnotContents(1, hid, QString());
            bool listed = false;
            for (const MarkupNote &m : nd->markups()) listed = listed || m.id == hid;
            CHECK(nd->render(1, 2.0, clip) == plain && !listed);
            nd->removeAnnots(1, {hid});
            // the line ends at the leftmost point, also when the stroke runs down to the right in small steps
            InkData dg;
            InkStroke ds;
            for (int j = 0; j <= 120; ++j) ds.pts.push_back(QPointF(300 + j * 0.5, 400 + j * 0.5));
            dg.strokes.push_back(ds);
            dg.width = 2;
            int did = nd->addInk(1, dg);
            nd->setAnnotContents(1, did, "Diagonal.");
            for (const MarkupNote &m : nd->markups())
                if (m.id == did) CHECK(QLineF(m.anchor, QPointF(300 - dg.width / 2 - 1.5, 400)).length() < 2);     // not (358, 460)
            nd->removeAnnots(1, {did});
            wait(300);
        }
        QRectF r0;
        for (const AnnotInfo &a : nd->annots(1)) if (a.id == ink) r0 = a.rect;
        nv->testPointer(0, on, -1, 50000);
        nv->testPointer(1, on + QPointF(40, 30), -1, 50010);
        nv->testPointer(2, on + QPointF(40, 30), -1, 50020);
        wait(300);
        QRectF r1;
        for (const AnnotInfo &a : nd->annots(1)) if (a.id == ink) r1 = a.rect;
        CHECK(r1.x() > r0.x() + 5 && noteOf(ink) == "Sign error here?" && np->card(ink));     // moved, note kept
        if (NoteCard *sc = np->card(ink)) {                   // for the picture: the pointer on the stroke
            QPointF v = nv->pageToView(1, r1.center() + QPointF(0, 0));
            for (const InkAnnot &ia : nd->inkAnnots(1)) if (ia.id == ink) v = nv->pageToView(1, ia.data.strokes[0].pts[8]);
            nv->testPointer(0, nv->pageToView(1, QPointF(40, 330)), -1, 51000);
            nv->testPointer(2, nv->pageToView(1, QPointF(40, 330)), -1, 51010);
            QMouseEvent mv(QEvent::MouseMove, v, nv->viewport()->mapToGlobal(v), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(nv->viewport(), &mv);
            CHECK(sc->isLit());
        }
        wait(300);
        nt->window()->grab().save(out + "/9e_stroke_note.png");
        {       // for the picture: a selected underline over several text lines
            QPointF a = nv->pageToView(1, QPointF(200, 290)), b = nv->pageToView(1, QPointF(300, 356));
            nv->testPointer(0, a, -1, 52000);
            nv->testPointer(1, b, -1, 52010);
            nv->testPointer(2, b, -1, 52020);
            PdfView::setNoteAfterMarking(false);
            int ul = nv->markupSelection(AnnotKind::Underline, QColor(220, 40, 40));
            PdfView::setNoteAfterMarking(true);
            QPointF c = nv->pageToView(1, QPointF(250, 322));
            nv->testPointer(0, c, -1, 53000);
            nv->testPointer(2, c, -1, 53010);
            wait(300);
            nt->window()->grab().save(out + "/9f_underline_selected.png");
            nd->removeAnnots(1, {ul});
            wait(200);
        }
        nd->removeAnnots(1, {ink});                          // what the eraser does
        wait(300);
        CHECK(!np->card(ink) && np->noteCount() == before);
    }

    // fillable forms: type in place, Tab to the next field, checkbox, saved values
    {
        QFile::remove(out + "/form.pdf");
        QFile::copy(QFileInfo(sample).absolutePath() + "/form.pdf", out + "/form.pdf");
        CHECK(w->openFile(out + "/form.pdf"));
        wait(300);
        PdfView *fv = w->currentTab()->view();
        Document *fd = w->currentTab()->doc();
        fv->setTools(Tool::Select, Tool::Select);
        QVector<FieldInfo> fs = fd->fields(0);
        CHECK(fs.size() == 9);
        auto centre = [&](int i) { return fv->pageToView(0, fs[i].rect.center()); };
        fv->testPointer(0, centre(0), -1, 20000);
        fv->testPointer(2, centre(0), -1, 20010);
        auto *le = qobject_cast<QLineEdit *>(fv->fieldEditor());
        CHECK(le != nullptr);
        if (le) {
            le->setText("Erika Mustermann");
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
            QApplication::sendEvent(le, &tab);                      // commits and moves to the e-mail field
        }
        auto *le2 = qobject_cast<QLineEdit *>(fv->fieldEditor());
        CHECK(le2 && le2 != le && le2->maxLength() == 40);
        if (le2) le2->setText("erika@example.org");
        wait(50);
        fv->viewport()->grab().save(out + "/9a_form_editing.png");
        fv->commitFieldEdit();
        fv->testPointer(0, centre(3), -1, 20100);                    // checkbox
        fv->testPointer(2, centre(3), -1, 20110);
        fs = fd->fields(0);
        CHECK(fs[0].value == "Erika Mustermann" && fs[1].value == "erika@example.org" && fs[3].checked);
        CHECK(w->currentTab()->save());
        Document again;
        again.open(out + "/form.pdf");
        CHECK(again.fields(0)[0].value == "Erika Mustermann" && again.fields(0)[3].checked);
        wait(400);
        fv->viewport()->grab().save(out + "/9b_form_filled.png");
    }

    // the Pages list scrolls by pixels, one wheel step as far as on the page
    {
        DocumentTab *pt = w->currentTab();
        auto *list = pt->findChild<QListWidget *>();
        const QSize old = w->size();
        w->resize(old.width(), 700);
        wait(300);
        QScrollBar *sb = list->verticalScrollBar();
        sb->setValue(0);
        const QPoint c = list->viewport()->rect().center();
        QWheelEvent we(QPointF(c), QPointF(list->viewport()->mapToGlobal(c)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                       Qt::NoScrollPhase, false);
        QApplication::sendEvent(list->viewport(), &we);
        const int after = sb->value();
        const int itemH = list->visualItemRect(list->item(0)).height();
        std::printf("     pages list: one wheel step scrolls %d px (preview %d px, range %d)\n", after, itemH, sb->maximum());
        CHECK(list->verticalScrollMode() == QAbstractItemView::ScrollPerPixel);
        CHECK(after == qMin(sb->maximum(), int(144 * PdfView::scrollSpeed())));
        w->resize(old);
        wait(200);
    }

    // page previews: the pages that come into view when the window grows are rendered as well
    {
        QFile::remove(out + "/previews.pdf");
        QFile::copy(sample, out + "/previews.pdf");
        w->resize(w->width(), 620);
        CHECK(w->openFile(out + "/previews.pdf"));
        DocumentTab *pt = w->currentTab();
        auto *side = pt->findChild<QTabWidget *>();
        auto *list = pt->findChild<QListWidget *>();
        for (int i = 0; i < side->count(); ++i)
            if (side->tabText(i).remove('&') == "Pages") side->setCurrentIndex(i);
        auto previews = [&] {
            int n = 0;
            for (int i = 0; i < list->count(); ++i) n += list->item(i)->data(Qt::UserRole + 1).toBool();
            return n;
        };
        wait(1200);
        int small = previews();
        w->resize(w->width(), 1500);
        wait(1500);
        std::printf("     previews: %d of %d, %d after the window grew\n", small, list->count(), previews());
        CHECK(small > 0 && small < list->count() && previews() == list->count());
    }

#ifdef PDFABLE_SIGNING
    {
        QString err;
        CHECK(createSelfSignedId(out + "/id.p12", "pw", "Test Signer", "Org", "t@example.org", "DE", 2, &err));
        auto id = loadPkcs12(out + "/id.p12", "pw", &err);
        CHECK(id != nullptr);
        SignParams sp;
        sp.id = id;
        sp.page = 0;
        sp.rect = QRectF(320, 620, 220, 70);
        sp.reason = "Approved";
        d->sign(sp, out + "/signed.pdf");
        auto sigs = d->signatures({});
        CHECK(sigs.size() == 1 && sigs[0].digestOk && sigs[0].coversWholeFile && !sigs[0].trusted);
        CHECK(d->signatures({sigs[0].certDer})[0].trusted);
        // a signed file is saved incrementally, but the undo history still starts afresh from the file on disk
        InkData s;
        s.strokes = {InkStroke{{QPointF(100, 600), QPointF(300, 600)}, {}}};
        CHECK(!d->isModified());
        d->addInk(1, s);
        tab->undo();
        CHECK(!d->isModified());
        d->addInk(1, s);
        CHECK(tab->save() && !d->isModified());
        d->addInk(1, s);
        tab->undo();
        CHECK(!d->isModified() && d->signatures({})[0].digestOk);
    }
#endif
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
