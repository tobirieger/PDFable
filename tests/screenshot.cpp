// Renders the README screenshot (docs/screenshot.png) without a display:
//   QT_QPA_PLATFORM=offscreen pdfable_screenshot docs/screenshot/sample.pdf docs/screenshot.png docs/screenshot-pages.png
// The sample PDF is compiled from docs/screenshot/sample.typ with Typst.
#include "MainWindow.h"
#include "NotesPanel.h"
#include "Theme.h"
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFrame>
#include <QMouseEvent>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace ink;

static void wait(int ms)
{
    QEventLoop l;
    QTimer::singleShot(ms, &l, &QEventLoop::quit);
    l.exec();
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: pdfable_screenshot <sample.pdf> <out.png> [pages.png]\n");
        return 2;
    }
    QCoreApplication::setOrganizationName("PDFableScreenshot");
    QCoreApplication::setApplicationName("PDFableScreenshot");
    QApplication app(argc, argv);
    Theme::apply(app);
    PdfView::setNoteAfterMarking(false);
    Document::setAuthor("Erika Mustermann");
    QSettings().setValue("pen/index", 2);          // red pen, as picked from the toolbar

    QTemporaryDir tmp;
    const QString pdf = tmp.filePath("Typst advertisement.pdf");
    QFile::copy(QString::fromLocal8Bit(argv[1]), pdf);
    QFile(pdf).setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    auto *w = new MainWindow;
    w->resize(1360, 900);
    w->show();
    if (!w->openFile(pdf)) return 1;
    wait(300);
    DocumentTab *tab = w->currentTab();
    PdfView *v = tab->view();
    Document *d = tab->doc();
    v->setZoom(1.12);
    v->gotoPage(0, 0, false);
    wait(300);

    // highlight a span of text (page coordinates in points) and attach a note
    qint64 t = 1000;
    auto mark = [&](QPointF from, QPointF to, QColor color, const QString &note) {
        w->setTool(Tool::Select);
        v->testPointer(0, v->pageToView(0, from), -1, t += 100);
        v->testPointer(1, v->pageToView(0, to), -1, t += 10);
        v->testPointer(2, v->pageToView(0, to), -1, t += 10);
        int id = v->markupSelection(AnnotKind::Highlight, color);
        if (id > 0 && !note.isEmpty()) d->setAnnotContents(0, id, note);
        wait(100);
    };
    mark({383, 144}, {495, 144}, QColor(255, 235, 60),
         "Fast enough for a live preview while writing.");
    mark({463, 288}, {185, 302}, QColor(140, 235, 130),
         "Checked: (3/10)(3π²)^(2/3) ≈ 2.871 ✓");
    mark({118, 434}, {357, 434}, QColor(130, 200, 255),
         "Try this for the thesis template.");

    // a sticky note in the margin
    d->setAnnotContents(0, d->addNote(0, QPointF(536, 232), QColor(255, 235, 60)), "Cite the benchmark here.");

    // a hand-drawn red loop around the equation
    w->setTool(Tool::Pen);
    const QPointF c(296, 331);
    const int n = 90;
    for (int i = 0; i <= n; ++i) {
        double a = -2.2 + 2 * M_PI * 1.06 * i / n;
        double r = 1.0 + 0.04 * std::sin(3 * a);
        QPointF p = c + QPointF(176 * r * std::cos(a), 22 * r * std::sin(a));
        v->testPointer(i == 0 ? 0 : 1, v->pageToView(0, p), 0.45f + 0.3f * float(std::sin(M_PI * i / n)), t += 8, true);
    }
    v->testPointer(2, v->pageToView(0, c + QPointF(176 * std::cos(-2.2 + 2 * M_PI * 1.06), 22 * std::sin(-2.2 + 2 * M_PI * 1.06))), 0, t += 8, true);
    v->flushInk();
    w->setTool(Tool::Select);

    tab->showNotes();
    v->gotoPage(0, 0, false);
    if (auto *bar = v->viewport()->findChild<QFrame *>("selBar")) bar->hide();
    w->statusBar()->clearMessage();
    wait(800);
    {       // the pointer on the first highlight: its note shows the colour of the marking
        QPointF on = v->pageToView(0, QPointF(440, 144));
        QMouseEvent mv(QEvent::MouseMove, on, v->viewport()->mapToGlobal(on), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(v->viewport(), &mv);
        wait(200);
    }
    bool ok = w->grab().save(QString::fromLocal8Bit(argv[2]));
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", argv[2]);

    // optional second picture: the same document with the page previews in the sidebar
    if (argc > 3) {
        QTabWidget *side = tab->findChild<QTabWidget *>();
        for (int i = 0; side && i < side->count(); ++i)
            if (side->tabText(i).remove('&') == "Pages") side->setCurrentIndex(i);
        wait(1500);
        bool ok2 = w->grab().save(QString::fromLocal8Bit(argv[3]));
        std::printf("%s %s\n", ok2 ? "wrote" : "FAILED", argv[3]);
        ok = ok && ok2;
    }
    return ok ? 0 : 1;
}
