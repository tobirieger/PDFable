#include "MainWindow.h"
#include "Creator.h"
#include <mupdf/fitz/version.h>
#include "Dialogs.h"
#include "Theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCryptographicHash>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QWheelEvent>
#include <cmath>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <functional>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>

using namespace ink;

static const char *kTabMime = "application/x-pdfable-tab";
static QList<MainWindow *> g_windows;
static bool g_tabDropHandled = false;

// Remembers when a pen was last seen anywhere in the application, so a tap on a toolbar
// button can be attributed to the pen or to the mouse.
namespace {
QElapsedTimer g_inputClock;
qint64 g_lastTabletMs = -100000;
class InputWatch : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::TabletEnterProximity: g_lastTabletMs = g_inputClock.elapsed(); break;
        default: break;
        }
        return false;
    }
};
bool penJustUsed() { return g_inputClock.isValid() && g_inputClock.elapsed() - g_lastTabletMs < 700; }
}

static QString viewKey(const QString &path)
{
    return "views/" + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}


// ------------------------------------------------------------------ TabBar
TabBar::TabBar(QWidget *parent) : QTabBar(parent)
{
    setMovable(true);
    setTabsClosable(true);
    setAcceptDrops(true);
    setElideMode(Qt::ElideMiddle);
    setExpanding(false);
    setDocumentMode(true);
}

void TabBar::mousePressEvent(QMouseEvent *e)
{
    pressIndex_ = e->button() == Qt::LeftButton ? tabAt(e->pos()) : -1;
    QTabBar::mousePressEvent(e);
}

void TabBar::mouseMoveEvent(QMouseEvent *e)
{
    // dragging a tab clearly out of the bar tears it off
    if ((e->buttons() & Qt::LeftButton) && pressIndex_ >= 0 && !rect().adjusted(-30, -25, 30, 25).contains(e->pos())) {
        QMouseEvent rel(QEvent::MouseButtonRelease, e->position(), e->globalPosition(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QTabBar::mouseReleaseEvent(&rel);     // end QTabBar's own tab-moving
        int idx = currentIndex();
        pressIndex_ = -1;
        emit detachRequested(idx);
        return;
    }
    QTabBar::mouseMoveEvent(e);
}

void TabBar::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(kTabMime)) e->acceptProposedAction();
}
void TabBar::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasFormat(kTabMime)) e->acceptProposedAction();
}
void TabBar::dropEvent(QDropEvent *e)
{
    if (!e->mimeData()->hasFormat(kTabMime)) return;
    auto *tab = reinterpret_cast<DocumentTab *>(e->mimeData()->data(kTabMime).toULongLong());
    g_tabDropHandled = true;
    e->acceptProposedAction();
    emit tabDropped(tab, tabAt(e->position().toPoint()));
}

// ------------------------------------------------------------------ MainWindow
QList<MainWindow *> MainWindow::windows() { return g_windows; }

MainWindow::MainWindow()
{
    g_windows.push_back(this);
    if (!g_inputClock.isValid()) {
        g_inputClock.start();
        qApp->installEventFilter(new InputWatch(qApp));
    }
    setAttribute(Qt::WA_DeleteOnClose);
    setAcceptDrops(true);
    resize(1280, 860);
    tabs_ = new TabWidget(this);
    setCentralWidget(tabs_);
    QSettings st;
    auto loadColors = [&](const char *key, const QList<QColor> &def) {
        QVector<QColor> out;
        const QStringList names = st.value(key).toStringList();
        for (int i = 0; i < def.size(); ++i) out.push_back(i < names.size() && QColor(names[i]).isValid() ? QColor(names[i]) : def[i]);
        return out;
    };
    penColors_ = loadColors("pen/colors", {QColor(0x1a, 0x1a, 0x1a), QColor(0x1f, 0x4f, 0xd8), QColor(0xd0, 0x28, 0x28),
                                           QColor(0x1e, 0x8e, 0x3e), QColor(0xe6, 0x7e, 0x22), QColor(0x8e, 0x44, 0xad)});
    hlColors_ = loadColors("hl/colors", {QColor(255, 235, 60), QColor(140, 235, 130), QColor(255, 160, 205),
                                         QColor(120, 220, 255), QColor(255, 190, 90), QColor(200, 170, 255)});
    penIdx_ = qBound(0, st.value("pen/index", 0).toInt(), 5);
    hlIdx_ = qBound(0, st.value("hl/index", 0).toInt(), 5);
    pen_.width = qBound(0.3f, st.value("pen/width", 1.6f).toFloat(), 12.f);
    hl_.width = qBound(4.f, st.value("hl/width", 12.f).toFloat(), 40.f);
    textSize_ = qBound(6.f, st.value("text/size", 12.f).toFloat(), 72.f);
    pen_.color = penColors_[penIdx_];
    hl_.color = hlColors_[hlIdx_];
    buildActions();
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { applyTools(); updateUi(); });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabs_->bar(), &TabBar::detachRequested, this, &MainWindow::startTabDrag);
    connect(tabs_->bar(), &TabBar::tabDropped, this, &MainWindow::adoptTab);
    statusBar()->showMessage(tr("Open a PDF with Ctrl+O or drop a file here."));
    updateUi();
}

MainWindow::~MainWindow() { g_windows.removeAll(this); }

DocumentTab *MainWindow::currentTab() const { return qobject_cast<DocumentTab *>(tabs_->currentWidget()); }

SwatchButton::SwatchButton(QWidget *parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Click to use this colour. Right-click or double-click to change it."));
}

void SwatchButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPointF c = QRectF(rect()).center();
    if (isChecked()) {
        p.setPen(QPen(Theme::accent(), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 11, 11);
    }
    p.setPen(QPen(Theme::isDark() ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 70), 1));
    p.setBrush(color_);
    p.drawEllipse(c, isChecked() ? 7.5 : 8.5, isChecked() ? 7.5 : 8.5);
}

void SwatchButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::RightButton) emit editRequested();
    else QAbstractButton::mousePressEvent(e);
}

void SwatchButton::mouseDoubleClickEvent(QMouseEvent *) { emit editRequested(); }

// slider position (0..1000) <-> size, logarithmic so thin pens get fine control
static void sizeRange(Tool t, float &lo, float &hi)
{
    if (t == Tool::Highlighter) { lo = 4; hi = 40; }
    else if (t == Tool::Text) { lo = 6; hi = 72; }
    else { lo = 0.3f; hi = 12; }
}
static int sizeToSlider(Tool t, float v)
{
    float lo, hi;
    sizeRange(t, lo, hi);
    return qBound(0, int(std::lround(1000 * std::log(v / lo) / std::log(hi / lo))), 1000);
}
static float sliderToSize(Tool t, int pos)
{
    float lo, hi;
    sizeRange(t, lo, hi);
    return lo * std::pow(hi / lo, pos / 1000.f);
}

void MainWindow::buildActions()
{
    auto add = [this](QMenu *m, const QString &text, const QKeySequence &ks, std::function<void()> fn) {
        QAction *a = m->addAction(text);
        addAction(a);       // keeps the shortcut alive while the menu bar is hidden (full screen)
        if (!ks.isEmpty()) a->setShortcut(ks);
        connect(a, &QAction::triggered, this, [fn] { fn(); });
        return a;
    };
    auto withTab = [this](std::function<void(DocumentTab *)> fn) {
        return [this, fn] { if (DocumentTab *t = currentTab()) fn(t); };
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *open = add(file, tr("&Open…"), QKeySequence::Open, [this] { openDialog(); });
    recentMenu_ = file->addMenu(tr("Open &recent"));
    rebuildRecent();
    QAction *save = add(file, tr("&Save"), QKeySequence::Save, withTab([](DocumentTab *t) { t->save(); }));
    add(file, tr("Save &as…"), QKeySequence("Ctrl+Shift+S"), withTab([](DocumentTab *t) { t->saveAs(); }));
    add(file, tr("Save optimised copy…"), {}, withTab([](DocumentTab *t) { t->saveAs(true); }));
    file->addSeparator();
    add(file, tr("&Create / merge PDF…"), QKeySequence("Ctrl+M"), [this] { openCreator(); });
    add(file, tr("&Print…"), QKeySequence::Print, withTab([](DocumentTab *t) { t->print(); }));
    file->addSeparator();
    add(file, tr("New &window"), QKeySequence("Ctrl+Shift+N"), [] { (new MainWindow)->show(); });
    add(file, tr("&Close tab"), QKeySequence("Ctrl+W"), [this] { if (tabs_->count()) closeTab(tabs_->currentIndex()); });
    add(file, tr("&Quit"), QKeySequence("Ctrl+Q"), [] { for (MainWindow *w : MainWindow::windows()) w->close(); });

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    QAction *undo = add(edit, tr("&Undo"), QKeySequence::Undo, withTab([](DocumentTab *t) { t->undo(); }));
    QAction *redo = add(edit, tr("&Redo"), QKeySequence("Ctrl+Y"), withTab([](DocumentTab *t) { t->redo(); }));
    edit->addSeparator();
    add(edit, tr("&Copy"), QKeySequence::Copy, withTab([](DocumentTab *t) { t->view()->copySelection(); }));
    add(edit, tr("&Highlight selection (with note)"), QKeySequence("Ctrl+H"), withTab([this](DocumentTab *t) { t->view()->markupSelection(AnnotKind::Highlight, hl_.color); }));
    add(edit, tr("&Underline selection"), QKeySequence("Ctrl+U"), withTab([](DocumentTab *t) { t->view()->markupSelection(AnnotKind::Underline, QColor(220, 40, 40)); }));
    add(edit, tr("&Find…"), QKeySequence::Find, withTab([](DocumentTab *t) { t->showSearch(); }));
    edit->addSeparator();
    add(edit, tr("Copy &area as image"), QKeySequence("Ctrl+Shift+C"), withTab([](DocumentTab *t) { t->view()->copyAreaAsImage(); }));
    add(edit, tr("Insert &image…"), {}, [this] { insertImage(); });
    edit->addSeparator();
    add(edit, tr("&Preferences…"), QKeySequence("Ctrl+,"), [this] { showPreferences(); });

    QMenu *viewM = menuBar()->addMenu(tr("&View"));
    QAction *zin = add(viewM, tr("Zoom &in"), QKeySequence("Ctrl++"), withTab([](DocumentTab *t) { t->view()->setZoom(t->view()->zoom() * 1.2); }));
    zin->setShortcuts({QKeySequence("Ctrl++"), QKeySequence("Ctrl+=")});
    QAction *zout = add(viewM, tr("Zoom &out"), QKeySequence("Ctrl+-"), withTab([](DocumentTab *t) { t->view()->setZoom(t->view()->zoom() / 1.2); }));
    add(viewM, tr("&Actual size"), QKeySequence("Ctrl+0"), withTab([](DocumentTab *t) { t->view()->setZoom(1.0); }));
    QAction *fitW = add(viewM, tr("Fit &width"), QKeySequence("Ctrl+2"), withTab([](DocumentTab *t) { t->view()->fitWidth(); }));
    add(viewM, tr("Fit &page"), QKeySequence("Ctrl+1"), withTab([](DocumentTab *t) { t->view()->fitPage(); }));
    viewM->addSeparator();
    backAct_ = add(viewM, tr("&Back to previous view"), QKeySequence("Alt+Left"), withTab([](DocumentTab *t) { t->view()->goBack(); }));
    fwdAct_ = add(viewM, tr("&Forward"), QKeySequence("Alt+Right"), withTab([](DocumentTab *t) { t->view()->goForward(); }));
    add(viewM, tr("Go to page…"), QKeySequence("Ctrl+G"), [this] { if (pageSpin_) { pageSpin_->setFocus(); pageSpin_->selectAll(); } });
    viewM->addSeparator();
    add(viewM, tr("Show/hide &sidebar"), QKeySequence("F4"), withTab([](DocumentTab *t) { t->toggleSidebar(); }));
    // As in a browser. Ctrl+Tab is left to Qt, which gives it to whichever tab bar has the keyboard: the
    // documents with the keyboard on the page, Outline / Pages / Notes with it in the sidebar.
    add(viewM, tr("&Next document"), QKeySequence("Ctrl+PgDown"), [this] {
        if (tabs_->count() > 1) tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count());
    });
    add(viewM, tr("Pre&vious document"), QKeySequence("Ctrl+PgUp"), [this] {
        if (tabs_->count() > 1) tabs_->setCurrentIndex((tabs_->currentIndex() + tabs_->count() - 1) % tabs_->count());
    });
    QMenu *themeM = viewM->addMenu(tr("Page &theme"));
    auto *themeGroup = new QActionGroup(this);
    const QStringList themeNames{tr("&Light"), tr("&Sepia"), tr("&Dark")};
    for (int i = 0; i < 3; ++i) {
        QAction *a = themeM->addAction(themeNames[i]);
        a->setCheckable(true);
        a->setChecked(int(PageColors::theme()) == i);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { setPageTheme(PageTheme(i)); });
    }
    connect(themeM, &QMenu::aboutToShow, this, [themeGroup] { themeGroup->actions().at(int(PageColors::theme()))->setChecked(true); });
    QAction *darkUi = add(viewM, tr("&Dark interface"), QKeySequence("Ctrl+Alt+D"), [] {
        MainWindow::setInterfaceMode(Theme::isDark() ? Theme::Mode::Light : Theme::Mode::Dark);
    });
    darkUi->setCheckable(true);
    darkUi->setChecked(Theme::isDark());
    darkUi->setObjectName("darkInterfaceAction");
    add(viewM, tr("Toggle dark pages"), QKeySequence("Ctrl+I"), [this] {
        setPageTheme(PageColors::theme() == PageTheme::Dark ? PageTheme::Light : PageTheme::Dark);
    });
    add(viewM, tr("F&ull screen"), QKeySequence("F11"), [this] { toggleFullScreen(); });

    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    toolGroup_ = new QActionGroup(this);
    auto tool = [&](Tool t, const QString &text, const QString &key) {
        QAction *a = tools->addAction(text);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(key));
        a->setToolTip(QString("%1 (%2)").arg(QString(text).remove('&'), key));
        toolGroup_->addAction(a);
        addAction(a);
        toolActions_.insert(int(t), a);
        connect(a, &QAction::triggered, this, [this, t] { setTool(t); });
        return a;
    };
    tool(Tool::Select, tr("&Select / text"), "V");
    tool(Tool::Hand, tr("&Hand"), "H");
    tool(Tool::Pen, tr("&Pen"), "P");
    tool(Tool::Highlighter, tr("High&lighter"), "M");
    tool(Tool::Eraser, tr("&Eraser"), "E");
    tool(Tool::Text, tr("&Text box"), "T");
    toolActions_[int(Tool::Select)]->setChecked(true);

    QMenu *pages = menuBar()->addMenu(tr("&Pages"));
    add(pages, tr("Rotate &left"), QKeySequence("Ctrl+Shift+L"), withTab([](DocumentTab *t) { t->rotatePages(-90); }));
    add(pages, tr("Rotate &right"), QKeySequence("Ctrl+Shift+R"), withTab([](DocumentTab *t) { t->rotatePages(90); }));
    add(pages, tr("&Delete"), {}, withTab([](DocumentTab *t) { t->deletePages(); }));
    add(pages, tr("&Extract to new PDF…"), {}, withTab([](DocumentTab *t) { t->extractPages(); }));
    pages->addSeparator();
    add(pages, tr("Insert pages from &PDF…"), {}, withTab([](DocumentTab *t) { t->insertPdf(); }));
    add(pages, tr("Insert &blank page"), {}, withTab([](DocumentTab *t) { t->insertBlank(); }));

    QMenu *sign = menuBar()->addMenu(tr("&Sign"));
    QAction *hand = add(sign, tr("&Handwritten signature…"), QKeySequence("Ctrl+Shift+H"), [this] { handSignature(); });
#ifdef PDFABLE_SIGNING
    add(sign, tr("Sign with &digital ID…"), QKeySequence("Ctrl+Shift+D"), [this] { signDigitally(); });
    add(sign, tr("&Create digital ID…"), {}, [this] { NewIdDialog(this).exec(); });
    add(sign, tr("Show &signatures"), {}, withTab([](DocumentTab *t) { t->showSignatures(); }));
#endif

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    add(help, tr("&About PDFable"), {}, [this] {
        QMessageBox::about(this, tr("About PDFable"),
                           tr("<h3>PDFable %1</h3><p>PDF reader with pen ink, signatures and a PDF creator.</p>"
                              "<p>Qt %2, MuPDF %3</p><p>MuPDF is licensed under the GNU AGPL v3.</p>")
                               .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion()), QString::fromLatin1(FZ_VERSION)));
    });

    add(edit, tr("Find &next"), QKeySequence::FindNext, withTab([](DocumentTab *t) { t->view()->findNext(true); }));

    QToolBar *tb = addToolBar(tr("Main"));
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->toggleViewAction()->setEnabled(false);
    tb->setContextMenuPolicy(Qt::PreventContextMenu);
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setIconSize(QSize(20, 20));
    auto deco = [](QAction *a, const char *icon, const QString &tip) {
        a->setIcon(Theme::icon(icon));
        a->setProperty("pdfableIcon", QString::fromLatin1(icon));     // redrawn when the theme changes
        QString t = tip;
        if (!a->shortcut().isEmpty()) t += QString("  (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
        a->setToolTip(t);
    };
    deco(open, "open", tr("Open"));
    deco(save, "save", tr("Save"));
    deco(undo, "undo", tr("Undo"));
    deco(redo, "redo", tr("Redo"));
    tb->addAction(open);
    tb->addAction(save);
    tb->addSeparator();
    tb->addAction(undo);
    tb->addAction(redo);
    tb->addSeparator();
    deco(backAct_, "back", tr("Back to the previous view"));
    deco(fwdAct_, "forward", tr("Forward"));
    tb->addAction(backAct_);
    tb->addAction(fwdAct_);
    tb->addSeparator();
    const char *toolIcons[6] = {"select", "hand", "pen", "marker", "eraser", "text"};
    const QStringList toolTips{tr("Select text and objects"), tr("Pan"), tr("Pen"), tr("Highlighter"), tr("Eraser"), tr("Text box")};
    for (int i = 0; i < 6; ++i) {
        deco(toolActions_[i], toolIcons[i], toolTips[i]);
        tb->addAction(toolActions_[i]);
    }
    tb->addSeparator();
    QWidget *sw = new QWidget;
    auto *swl = new QHBoxLayout(sw);
    swl->setContentsMargins(2, 0, 2, 0);
    swl->setSpacing(1);
    for (int i = 0; i < 6; ++i) {
        auto *b = new SwatchButton;
        swatches_.push_back(b);
        connect(b, &QAbstractButton::clicked, this, [this, i] { selectSwatch(i); });
        connect(b, &SwatchButton::editRequested, this, [this, i] { editSwatch(i); });
        swl->addWidget(b);
    }
    tb->addWidget(sw);
    sizeSlider_ = new SizeSlider;
    sizeSlider_->setToolTip(tr("Pen thickness / text size"));
    sizeLabel_ = new QLabel;
    sizeLabel_->setMinimumWidth(52);
    sizeLabel_->setAlignment(Qt::AlignCenter);
    tb->addWidget(sizeSlider_);
    tb->addWidget(sizeLabel_);
    connect(sizeSlider_, &SizeSlider::valueChanged, this, [this](int pos) {
        float v = sliderToSize(tool_, pos);
        QSettings st;
        if (tool_ == Tool::Highlighter) { hl_.width = v; st.setValue("hl/width", v); }
        else if (tool_ == Tool::Text) { textSize_ = std::round(v); st.setValue("text/size", textSize_); }
        else { pen_.width = v; st.setValue("pen/width", v); }
        applyTools();
        refreshToolOptions();
    });
    tb->addSeparator();
    deco(hand, "sign", tr("Handwritten signature"));
    tb->addAction(hand);
    tb->addSeparator();
    deco(zout, "zoomout", tr("Zoom out"));
    deco(zin, "zoomin", tr("Zoom in"));
    deco(fitW, "fit", tr("Fit width"));
    tb->addAction(zout);
    zoomLabel_ = new QLabel("100%");
    zoomLabel_->setMinimumWidth(48);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    tb->addWidget(zoomLabel_);
    tb->addAction(zin);
    tb->addAction(fitW);
    tb->addSeparator();
    pageSpin_ = new QSpinBox;
    pageSpin_->setRange(1, 1);
    pageSpin_->setKeyboardTracking(false);
    pageSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    pageSpin_->setAlignment(Qt::AlignCenter);
    pageSpin_->setFixedWidth(56);
    pageCount_ = new QLabel(" / 0 ");
    tb->addWidget(pageSpin_);
    tb->addWidget(pageCount_);
    connect(pageSpin_, &QSpinBox::valueChanged, this, [this](int v) {
        if (PdfView *pv = view())
            if (pv->currentPage() != v - 1) pv->gotoPage(v - 1);
    });
    refreshToolOptions();
}

void MainWindow::refreshToolOptions()
{
    bool hl = tool_ == Tool::Highlighter;
    const QVector<QColor> &pal = hl ? hlColors_ : penColors_;
    for (int i = 0; i < swatches_.size(); ++i) {
        swatches_[i]->setColor(pal[i]);
        swatches_[i]->setChecked(i == (hl ? hlIdx_ : penIdx_));
    }
    float v = hl ? hl_.width : tool_ == Tool::Text ? textSize_ : pen_.width;
    QSignalBlocker b(sizeSlider_);
    sizeSlider_->setValue(sizeToSlider(tool_, v));
    sizeLabel_->setText(tool_ == Tool::Text ? tr("%1 pt").arg(int(v)) : tr("%1 pt").arg(QString::number(v, 'f', v < 10 ? 1 : 0)));
}

void MainWindow::selectSwatch(int i)
{
    QSettings st;
    if (tool_ == Tool::Highlighter) {
        hlIdx_ = i;
        hl_.color = hlColors_[i];
        st.setValue("hl/index", i);
    } else {
        penIdx_ = i;
        pen_.color = penColors_[i];
        st.setValue("pen/index", i);
        if (tool_ != Tool::Pen && tool_ != Tool::Text) { setTool(Tool::Pen); return; }
    }
    applyTools();
    refreshToolOptions();
}

void MainWindow::editSwatch(int i)
{
    bool hl = tool_ == Tool::Highlighter;
    QVector<QColor> &pal = hl ? hlColors_ : penColors_;
    QColor c = QColorDialog::getColor(pal[i], this, hl ? tr("Highlighter colour") : tr("Pen colour"));
    if (!c.isValid()) { refreshToolOptions(); return; }
    pal[i] = c;
    QStringList names;
    for (const QColor &q : std::as_const(pal)) names.push_back(q.name());
    QSettings().setValue(hl ? "hl/colors" : "pen/colors", names);
    selectSwatch(i);
}

void MainWindow::setTool(Tool t)
{
    // the device that picked the tool gets it
    activePen_ = penJustUsed();
    (activePen_ ? penTool_ : mouseTool_) = t;
    tool_ = t;
    if (QAction *a = toolActions_.value(int(t))) a->setChecked(true);
    applyTools();
    refreshToolOptions();
}

// Called when a pen or the mouse starts working on the page: the toolbar follows the device,
// and once the pen is in use the mouse goes back to navigating.
void MainWindow::onDeviceUsed(bool pen)
{
    if (pen) {
        if (activePen_ && mouseTool_ == Tool::Select) return;
        activePen_ = true;
        mouseTool_ = Tool::Select;
        tool_ = penTool_;
    } else {
        if (!activePen_) return;
        activePen_ = false;
        tool_ = mouseTool_;
    }
    if (QAction *a = toolActions_.value(int(tool_))) a->setChecked(true);
    applyTools();
    refreshToolOptions();
}

void MainWindow::setPageTheme(PageTheme t)
{
    PdfView::setPageTheme(t);
    QSettings().setValue("view/pageTheme", int(t));
}

void MainWindow::openCreator()
{
    CreatorWindow *c = CreatorWindow::showCreator();
    connect(c, &CreatorWindow::openInReader, this, [this](const QString &p) {
        openFile(p);
        raise();
        activateWindow();
    }, Qt::UniqueConnection);
}

// Switch light/dark interface in every open window: palette, style sheet and the drawn icons.
void MainWindow::setInterfaceMode(Theme::Mode m)
{
    Theme::setMode(m);
    for (MainWindow *w : std::as_const(g_windows)) {
        for (QAction *a : w->findChildren<QAction *>()) {
            QString icon = a->property("pdfableIcon").toString();
            if (!icon.isEmpty()) a->setIcon(Theme::icon(icon));
            if (a->objectName() == "darkInterfaceAction") a->setChecked(Theme::isDark());
        }
        w->refreshToolOptions();
        w->update();
    }
    for (QWidget *top : QApplication::topLevelWidgets()) top->update();
}

void MainWindow::showPreferences()
{
    QDialog d(this);
    d.setWindowTitle(tr("Preferences"));
    auto *form = new QFormLayout(&d);
    auto *speed = new SizeSlider;
    auto *speedLabel = new QLabel;
    speedLabel->setMinimumWidth(48);
    auto toPos = [](double f) { return int(std::lround((f - 0.3) / 1.7 * 1000)); };
    auto toSpeed = [](int pos) { return 0.3 + pos / 1000.0 * 1.7; };
    speed->setValue(toPos(PdfView::scrollSpeed()));
    speedLabel->setText(QString("%1 %").arg(qRound(PdfView::scrollSpeed() * 100)));
    connect(speed, &SizeSlider::valueChanged, &d, [=](int pos) {
        double f = std::round(toSpeed(pos) * 20) / 20;
        PdfView::setScrollSpeed(f);
        QSettings().setValue("view/scrollSpeed", f);
        speedLabel->setText(QString("%1 %").arg(qRound(f * 100)));
    });
    auto *row = new QHBoxLayout;
    row->addWidget(speed);
    row->addWidget(speedLabel);
    form->addRow(tr("Scroll speed (wheel, keys)"), row);
    auto *theme = new QComboBox;
    theme->addItems({tr("Light"), tr("Sepia"), tr("Dark")});
    theme->setCurrentIndex(int(PageColors::theme()));
    connect(theme, &QComboBox::currentIndexChanged, this, [this](int i) { setPageTheme(PageTheme(i)); });
    form->addRow(tr("Page theme"), theme);
    auto *ui = new QComboBox;
    ui->addItems({tr("Follow Windows"), tr("Light"), tr("Dark")});
    ui->setCurrentIndex(int(Theme::mode()));
    connect(ui, &QComboBox::currentIndexChanged, this, [](int i) { MainWindow::setInterfaceMode(Theme::Mode(i)); });
    form->addRow(tr("Interface (Ctrl+Alt+D)"), ui);
    auto *noteMode = new QComboBox;
    noteMode->addItems({tr("Right after marking (colour bar or Ctrl+H)"), tr("Only when I click the marking")});
    noteMode->setCurrentIndex(PdfView::noteAfterMarking() ? 0 : 1);
    noteMode->setToolTip(tr("Clicking a marking always opens its note."));
    connect(noteMode, &QComboBox::currentIndexChanged, this, [](int i) {
        PdfView::setNoteAfterMarking(i == 0);
        QSettings().setValue("notes/afterMarking", i == 0);
    });
    form->addRow(tr("Open the note editor"), noteMode);
    auto *note = new QLabel(tr("Pen, mouse and finger: the pen and the mouse each keep their own tool. Tap a toolbar "
                               "button with the pen to set the pen's tool, click it with the mouse to set the mouse's "
                               "tool. A finger always pans and zooms."));
    note->setWordWrap(true);
    form->addRow(note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::accept);
    form->addRow(bb);
    d.exec();
}

// ------------------------------------------------------------------ SizeSlider
SizeSlider::SizeSlider(QWidget *parent) : QWidget(parent)
{
    setFixedSize(140, 26);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
}

void SizeSlider::setValue(int v)
{
    v = qBound(0, v, 1000);
    if (v == value_) return;
    value_ = v;
    update();
    emit valueChanged(v);
}

void SizeSlider::setFromPos(qreal x) { setValue(int(std::lround((x - 10) / (width() - 20.0) * 1000))); }

void SizeSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal cy = height() / 2.0, x0 = 10, x1 = width() - 10, hx = x0 + (x1 - x0) * value_ / 1000.0;
    p.setPen(QPen(palette().color(QPalette::Mid), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(x0, cy), QPointF(x1, cy));
    p.setPen(QPen(Theme::accent(), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(x0, cy), QPointF(hx, cy));
    p.setPen(QPen(Theme::accent(), 2));
    p.setBrush(palette().color(QPalette::Base));
    p.drawEllipse(QPointF(hx, cy), 7, 7);       // one circle, drawn in floating point: always round
}

void SizeSlider::mousePressEvent(QMouseEvent *e) { setFromPos(e->position().x()); }
void SizeSlider::mouseMoveEvent(QMouseEvent *e) { if (e->buttons() & Qt::LeftButton) setFromPos(e->position().x()); }
void SizeSlider::wheelEvent(QWheelEvent *e)
{
    setValue(value_ + (e->angleDelta().y() > 0 ? 20 : -20));
    e->accept();
}
void SizeSlider::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Down) setValue(value_ - 10);
    else if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Up) setValue(value_ + 10);
    else QWidget::keyPressEvent(e);
}

void MainWindow::applyTools()
{
    if (PdfView *pv = view()) {
        pv->setPenStyle(pen_);
        pv->setHighlighterStyle(hl_);
        pv->setTextSize(textSize_);
        pv->setHighlightPalette(hlColors_);
        pv->setTools(penTool_, mouseTool_);
    }
}

void MainWindow::updateUi()
{
    DocumentTab *t = currentTab();
    setWindowTitle(t ? t->title() + " – PDFable" : QString("PDFable"));
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *dt = static_cast<DocumentTab *>(tabs_->widget(i));
        tabs_->setTabText(i, dt->title());
        tabs_->setTabToolTip(i, dt->doc()->path());
    }
    QSignalBlocker b(pageSpin_);
    if (t) {
        pageSpin_->setRange(1, qMax(1, t->doc()->pageCount()));
        pageSpin_->setValue(t->view()->currentPage() + 1);
        pageCount_->setText(QString(" / %1 ").arg(t->doc()->pageCount()));
        zoomLabel_->setText(QString("%1%").arg(qRound(t->view()->zoom() * 100)));
        backAct_->setEnabled(t->view()->canGoBack());
        fwdAct_->setEnabled(t->view()->canGoForward());
    } else {
        pageSpin_->setRange(1, 1);
        pageCount_->setText(" / 0 ");
    }
}

void MainWindow::addTab(DocumentTab *tab, int index)
{
    int i = tabs_->insertTab(index < 0 ? tabs_->count() : index, tab, tab->title());
    tab->disconnect(this);
    tab->view()->disconnect(this);
    connect(tab, &DocumentTab::titleChanged, this, &MainWindow::updateUi);
    connect(tab, &DocumentTab::statusMessage, this, [this](const QString &m) { statusBar()->showMessage(m, 6000); });
    connect(tab->view(), &PdfView::currentPageChanged, this, [this, tab] { if (tab == currentTab()) updateUi(); });
    connect(tab->view(), &PdfView::zoomChanged, this, [this, tab] { if (tab == currentTab()) updateUi(); });
    connect(tab->view(), &PdfView::toolChangeRequested, this, &MainWindow::setTool);
    connect(tab->view(), &PdfView::deviceUsed, this, &MainWindow::onDeviceUsed);
    connect(tab->view(), &PdfView::historyChanged, this, [this, tab] { if (tab == currentTab()) updateUi(); });
    tabs_->setCurrentIndex(i);
    applyTools();
    updateUi();
}

DocumentTab *MainWindow::takeTab(int index)
{
    auto *tab = qobject_cast<DocumentTab *>(tabs_->widget(index));
    if (!tab) return nullptr;
    tabs_->removeTab(index);
    tab->disconnect(this);
    tab->view()->disconnect(this);
    tab->setParent(nullptr);
    updateUi();
    return tab;
}

MainWindow *MainWindow::detachTab(int index, const QPoint &globalPos)
{
    if (tabs_->count() <= 1) {        // the only tab: just move this window
        move(globalPos - QPoint(80, 20));
        return this;
    }
    DocumentTab *tab = takeTab(index);
    if (!tab) return nullptr;
    auto *w = new MainWindow;
    w->resize(size());
    w->move(globalPos - QPoint(80, 20));
    w->penTool_ = penTool_;
    w->mouseTool_ = mouseTool_;
    w->activePen_ = activePen_;
    w->tool_ = tool_;
    w->addTab(tab);
    if (QAction *a = w->toolActions_.value(int(tool_))) a->setChecked(true);
    w->applyTools();
    w->refreshToolOptions();
    w->show();
    w->raise();
    w->activateWindow();
    return w;
}

void MainWindow::adoptTab(DocumentTab *tab, int index)
{
    MainWindow *src = nullptr;
    for (MainWindow *w : std::as_const(g_windows))
        if (w->tabs_->indexOf(tab) >= 0) src = w;
    if (!src) return;
    if (src == this) {
        int from = tabs_->indexOf(tab);
        if (index >= 0 && index != from) tabs_->bar()->moveTab(from, index);
        return;
    }
    src->takeTab(src->tabs_->indexOf(tab));
    addTab(tab, index);
    raise();
    activateWindow();
    if (src->tabs_->count() == 0) src->close();
}

void MainWindow::startTabDrag(int index)
{
    auto *tab = qobject_cast<DocumentTab *>(tabs_->widget(index));
    if (!tab) return;
    auto *drag = new QDrag(tabs_->bar());
    auto *mime = new QMimeData;
    mime->setData(kTabMime, QByteArray::number(qulonglong(reinterpret_cast<quintptr>(tab))));
    drag->setMimeData(mime);
    QPixmap pm = tab->view()->viewport()->grab().scaledToWidth(260, Qt::SmoothTransformation);
    drag->setPixmap(pm.copy(0, 0, pm.width(), qMin(pm.height(), 180)));
    g_tabDropHandled = false;
    drag->exec(Qt::MoveAction);
    if (!g_tabDropHandled) {          // released outside every tab bar: new window
        int i = tabs_->indexOf(tab);
        if (i >= 0) detachTab(i, QCursor::pos());
    }
}

void MainWindow::closeTab(int index)
{
    auto *tab = qobject_cast<DocumentTab *>(tabs_->widget(index));
    if (!tab || !tab->maybeSave()) return;
    saveViewState(tab);
    tabs_->removeTab(index);
    tab->deleteLater();
    updateUi();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    for (int i = 0; i < tabs_->count(); ++i) {
        tabs_->setCurrentIndex(i);
        if (!static_cast<DocumentTab *>(tabs_->widget(i))->maybeSave()) {
            e->ignore();
            return;
        }
    }
    QStringList session;
    for (MainWindow *w : std::as_const(g_windows))
        for (int i = 0; i < w->tabs_->count(); ++i) {
            auto *t = static_cast<DocumentTab *>(w->tabs_->widget(i));
            if (w == this) saveViewState(t);
            if (!t->doc()->path().isEmpty()) session.push_back(t->doc()->path());
        }
    if (g_windows.size() == 1) QSettings().setValue("session/files", session);   // last window: remember what was open
    e->accept();
}

void MainWindow::saveViewState(DocumentTab *tab)
{
    if (!tab || tab->doc()->path().isEmpty()) return;
    ViewState s = tab->view()->viewState();
    QSettings().setValue(viewKey(tab->doc()->path()), QVariantList{s.zoom, s.page, s.y});
}

void MainWindow::restoreSession()
{
    if (g_windows.isEmpty()) return;
    for (const QString &f : QSettings().value("session/files").toStringList())
        if (QFileInfo::exists(f)) g_windows.first()->openFile(f);
}

void MainWindow::toggleFullScreen()
{
    if (isFullScreen()) {
        menuBar()->show();
        statusBar()->show();
        if (wasMaximized_) showMaximized();
        else showNormal();
    } else {
        wasMaximized_ = isMaximized();
        menuBar()->hide();
        statusBar()->hide();
        showFullScreen();
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) openFile(u.toLocalFile());
}

bool MainWindow::openFile(const QString &path)
{
    QString abs = QFileInfo(path).absoluteFilePath();
    for (MainWindow *w : std::as_const(g_windows))
        for (int i = 0; i < w->tabs_->count(); ++i)
            if (static_cast<DocumentTab *>(w->tabs_->widget(i))->doc()->path() == abs) {
                w->tabs_->setCurrentIndex(i);
                w->raise();
                w->activateWindow();
                return true;
            }
    auto doc = std::make_shared<Document>();
    try {
        doc->open(abs);
        while (doc->needsPassword()) {
            bool ok = false;
            QString pw = QInputDialog::getText(this, tr("Password"), tr("%1 is protected. Password:").arg(QFileInfo(abs).fileName()),
                                               QLineEdit::Password, QString(), &ok);
            if (!ok) return false;
            doc->authenticate(pw);
        }
    } catch (const PdfError &err) {
        QMessageBox::warning(this, tr("Open"), tr("Cannot open %1:\n%2").arg(abs, err.message()));
        return false;
    }
    auto *tab = new DocumentTab(doc);
    addTab(tab);
    addRecent(abs);
    // come back to where this file was left; first time: fit the page width
    QVariantList vs = QSettings().value(viewKey(abs)).toList();
    QTimer::singleShot(0, tab, [tab, vs] {
        if (vs.size() == 3 && vs[0].toDouble() > 0) tab->view()->setViewState(ViewState{vs[0].toDouble(), vs[1].toInt(), vs[2].toDouble()});
        else tab->view()->fitWidth();
    });
    return true;
}

void MainWindow::openDialog()
{
    QSettings st;
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open PDF"), st.value("lastDir").toString(), tr("PDF (*.pdf);;All files (*)"));
    for (const QString &f : files) {
        st.setValue("lastDir", QFileInfo(f).absolutePath());
        openFile(f);
    }
}

void MainWindow::addRecent(const QString &path)
{
    QSettings st;
    QStringList r = st.value("recent").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 12) r.removeLast();
    st.setValue("recent", r);
    for (MainWindow *w : std::as_const(g_windows)) w->rebuildRecent();
}

void MainWindow::rebuildRecent()
{
    recentMenu_->clear();
    for (const QString &f : QSettings().value("recent").toStringList())
        connect(recentMenu_->addAction(QFileInfo(f).fileName() + "   —   " + QFileInfo(f).absolutePath()), &QAction::triggered, this, [this, f] { openFile(f); });
    recentMenu_->setEnabled(!recentMenu_->isEmpty());
}

void MainWindow::mergeDialog()
{
    QSettings st;
    QStringList files = QFileDialog::getOpenFileNames(this, tr("Choose the PDF files to merge (in name order)"), st.value("lastDir").toString(), tr("PDF (*.pdf)"));
    if (files.size() < 2) {
        if (!files.isEmpty()) QMessageBox::information(this, tr("Merge"), tr("Choose at least two files."));
        return;
    }
    files.sort(Qt::CaseInsensitive);
    QString out = QFileDialog::getSaveFileName(this, tr("Save merged PDF"), QFileInfo(files.first()).absolutePath() + "/merged.pdf", tr("PDF (*.pdf)"));
    if (out.isEmpty()) return;
    try {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        Document::mergeFiles(files, out);
        QApplication::restoreOverrideCursor();
        openFile(out);
    } catch (const PdfError &err) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Merge"), err.message());
    }
}

void MainWindow::insertImage()
{
    PdfView *pv = view();
    if (!pv) return;
    QString f = QFileDialog::getOpenFileName(this, tr("Insert image"), QString(), tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"));
    if (f.isEmpty()) return;
    QImageReader rd(f);
    rd.setAutoTransform(true);
    QImage img = rd.read();
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Insert image"), rd.errorString());
        return;
    }
    if (img.width() > 2400) img = img.scaledToWidth(2400, Qt::SmoothTransformation);
    pv->startPlacingImage(img);
}

void MainWindow::handSignature()
{
    PdfView *pv = view();
    if (!pv) return;
    HandSignatureDialog d(this);
    if (d.exec() == QDialog::Accepted && !d.result().strokes.isEmpty()) pv->startPlacingInk(d.result(), 170);
}

void MainWindow::signDigitally()
{
#ifdef PDFABLE_SIGNING
    DocumentTab *tab = currentTab();
    if (!tab) return;
    DigitalSignDialog d(tab->doc()->hasSignatures(), this);
    if (d.exec() != QDialog::Accepted) return;
    SignParams params = d.params();
    auto finish = [this, tab, params](int page, QRectF rect) mutable {
        params.page = page;
        params.rect = rect;
        QFileInfo fi(tab->doc()->path());
        QString out = QFileDialog::getSaveFileName(this, tr("Save signed PDF"), fi.absolutePath() + "/" + fi.completeBaseName() + "_signed.pdf", tr("PDF (*.pdf)"));
        if (out.isEmpty()) return;
        try {
            tab->view()->flushInk();
            QApplication::setOverrideCursor(Qt::WaitCursor);
            tab->doc()->sign(params, out);
            QApplication::restoreOverrideCursor();
            tab->showSignatures();
            statusBar()->showMessage(tr("Signed and saved as %1").arg(out), 8000);
        } catch (const PdfError &err) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Sign"), err.message());
        }
    };
    if (d.visibleSignature()) tab->view()->startRectPick(finish);
    else finish(tab->view()->currentPage(), QRectF());
#endif
}
