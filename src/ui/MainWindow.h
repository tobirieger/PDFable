// MainWindow: menus, toolbar and a tab bar whose tabs can be dragged out into new windows
// or dropped onto the tab bar of another window.
#pragma once
#include "DocumentTab.h"
#include "Theme.h"
#include <QMainWindow>
#include <QTabBar>
#include <QAbstractButton>
#include <QTabWidget>

class QActionGroup;
class QAction;
class QSpinBox;
class QLabel;

class TabBar : public QTabBar {
    Q_OBJECT
public:
    explicit TabBar(QWidget *parent = nullptr);
signals:
    void detachRequested(int index);
    void tabDropped(DocumentTab *tab, int index);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
private:
    int pressIndex_ = -1;
};

// Round colour swatch. Click selects it; right-click or double-click lets the user change the colour.
class SwatchButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit SwatchButton(QWidget *parent = nullptr);
    void setColor(const QColor &c) { color_ = c; update(); }
    QColor color() const { return color_; }
    QSize sizeHint() const override { return QSize(26, 26); }
signals:
    void editRequested();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
private:
    QColor color_;
};

// Slider drawn by hand so the handle is a true circle at every display scaling.
class SizeSlider : public QWidget {
    Q_OBJECT
public:
    explicit SizeSlider(QWidget *parent = nullptr);
    int value() const { return value_; }
    void setValue(int v);                 // 0..1000
    QSize sizeHint() const override { return QSize(140, 26); }
signals:
    void valueChanged(int v);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
private:
    void setFromPos(qreal x);
    int value_ = 0;
};

class TabWidget : public QTabWidget {
public:
    explicit TabWidget(QWidget *parent = nullptr) : QTabWidget(parent), bar_(new TabBar(this)) { setTabBar(bar_); }
    TabBar *bar() const { return bar_; }
private:
    TabBar *bar_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    static QList<MainWindow *> windows();

    bool openFile(const QString &path);
    DocumentTab *currentTab() const;
    int tabCount() const { return tabs_->count(); }
    void addTab(DocumentTab *tab, int index = -1);
    DocumentTab *takeTab(int index);
    MainWindow *detachTab(int index, const QPoint &globalPos);   // move a tab into a new window
    void adoptTab(DocumentTab *tab, int index);                  // move a tab here from any window
    void setTool(Tool t);
    void toggleFullScreen();
    static void setInterfaceMode(Theme::Mode m);
    void openCreator();
    static void restoreSession();

protected:
    void closeEvent(QCloseEvent *e) override;
public:
    QMenu *createPopupMenu() override { return nullptr; }   // no "hide toolbar" context menu
protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void buildActions();
    void refreshToolOptions();
    void selectSwatch(int i);
    void editSwatch(int i);
    void applyTools();
    void updateUi();
    void closeTab(int index);
    void startTabDrag(int index);
    void openDialog();
    void mergeDialog();
    void insertImage();
    void handSignature();
    void signDigitally();
    void addRecent(const QString &path);
    void rebuildRecent();
    void saveViewState(DocumentTab *tab);
    bool wasMaximized_ = false;
    PdfView *view() const { return currentTab() ? currentTab()->view() : nullptr; }

    TabWidget *tabs_;
    QActionGroup *toolGroup_ = nullptr;
    QHash<int, QAction *> toolActions_;
    QVector<SwatchButton *> swatches_;
    SizeSlider *sizeSlider_ = nullptr;
    QAction *backAct_ = nullptr, *fwdAct_ = nullptr, *undoAct_ = nullptr, *redoAct_ = nullptr;
    QLabel *sizeLabel_ = nullptr;
    QVector<QColor> penColors_, hlColors_;
    int penIdx_ = 0, hlIdx_ = 0;
    float textSize_ = 12;
    QSpinBox *pageSpin_ = nullptr;
    QLabel *pageCount_ = nullptr, *zoomLabel_ = nullptr;
    QMenu *recentMenu_ = nullptr;
    // The pen and the mouse keep separate tools; the toolbar shows the one of the device in use.
    Tool tool_ = Tool::Select, penTool_ = Tool::Pen, mouseTool_ = Tool::Select;
    bool activePen_ = false;
    void onDeviceUsed(bool pen);
    void showPreferences();
    void setPageTheme(PageTheme t);
    PenStyle pen_, hl_{QColor(255, 235, 60), 12.f};
};
