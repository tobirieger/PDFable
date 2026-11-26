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
#include <QLabel>
#include <QScrollBar>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QFileInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <cmath>
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
    v->findNext(true);
    wait(300);
    v->viewport()->grab().save(out + "/4_search_select.png");
    v->setSearch(QString());

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

    // page organizer: reorder, rotate, delete, insert, extract, merge
    tab->rearrange({1, 0, 2, 3, 4, 5});
    CHECK(d->selectionText(0, {72, 80}, {200, 95}).startsWith("Section 2"));
    tab->undo();
    CHECK(d->selectionText(0, {72, 80}, {200, 95}).startsWith("Section 1"));
    d->rotatePages({1}, 90);
    CHECK(d->pageSize(1).width() > d->pageSize(1).height());
    d->insertBlank(1, QSizeF(300, 300));
    CHECK(d->pageCount() == 7);
    d->rearrange({0, 2, 3, 4, 5, 6});
    CHECK(d->pageCount() == 6);
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

    // save, reopen: ink survives with widths
    CHECK(tab->save());
    CHECK(!d->isModified());
    CHECK(d->inkAnnots(0).size() == 4);
    CHECK(!d->inkAnnots(0)[0].data.strokes[0].w.isEmpty());

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

    // tabs: second document, tear off into a new window, drop back
    QFile::remove(out + "/second.pdf");
    QFile::copy(sample, out + "/second.pdf");
    CHECK(w->openFile(out + "/second.pdf"));
    CHECK(w->tabCount() == 2);
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

    // notes on markings: highlight -> note editor opens in the Notes tab -> typed text is stored
    {
        DocumentTab *nt = w->currentTab();
        PdfView *nv = nt->view();
        Document *nd = nt->doc();
        nv->setTools(Tool::Select, Tool::Select);
        nv->gotoPage(1, 100, false);
        wait(200);
        PdfView::setNoteAfterMarking(true);
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
        nt->window()->grab().save(out + "/9c_notes.png");
        PdfView::setNoteAfterMarking(true);
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
    }
#endif
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
