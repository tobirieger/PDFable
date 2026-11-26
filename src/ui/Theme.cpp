#include "Theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>

namespace Theme {

static bool g_dark = false;
static Mode g_mode = Mode::System;

bool isDark() { return g_dark; }
Mode mode() { return g_mode; }

static bool systemDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return false;
#endif
}

static void applyColors(QApplication &app);

void setMode(Mode m)
{
    g_mode = m;
    QSettings().setValue("ui/theme", int(m));
    g_dark = m == Mode::Dark || (m == Mode::System && systemDark());
    applyColors(*qApp);
}
QColor accent() { return g_dark ? QColor(0xe2, 0x5d, 0x70) : QColor(0x8c, 0x16, 0x26); }   // deep red

void apply(QApplication &app)
{
    g_mode = Mode(qBound(0, QSettings().value("ui/theme", 0).toInt(), 2));
    g_dark = g_mode == Mode::Dark || (g_mode == Mode::System && systemDark());
    app.setStyle(QStyleFactory::create("Fusion"));
    QFont f = app.font();
    if (f.pointSizeF() > 0 && f.pointSizeF() < 9.5) f.setPointSizeF(9.5);
    app.setFont(f);
    applyColors(app);
}

static void applyColors(QApplication &app)
{
    const QColor window = g_dark ? QColor(0x1f, 0x22, 0x26) : QColor(0xf6, 0xf7, 0xf9);
    const QColor base = g_dark ? QColor(0x27, 0x2a, 0x2f) : QColor(0xff, 0xff, 0xff);
    const QColor text = g_dark ? QColor(0xe6, 0xe8, 0xeb) : QColor(0x1f, 0x23, 0x28);
    const QColor mid = g_dark ? QColor(0x3a, 0x3e, 0x45) : QColor(0xdd, 0xe1, 0xe6);
    const QColor dim = g_dark ? QColor(0x8b, 0x92, 0x9c) : QColor(0x8a, 0x91, 0x9b);
    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, accent());
    p.setColor(QPalette::HighlightedText, g_dark ? QColor(0x10, 0x14, 0x1a) : QColor(Qt::white));
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Dark, mid);
    p.setColor(QPalette::Light, base);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    app.setPalette(p);

    const QString hover = g_dark ? "rgba(255,255,255,0.08)" : "rgba(0,0,0,0.06)";
    const QString soft = QString("rgba(%1,%2,%3,%4)").arg(accent().red()).arg(accent().green()).arg(accent().blue()).arg(g_dark ? "0.24" : "0.13");
    // the selected entry of the list that has the keyboard: the arrow keys and Enter act there, not on the page
    const QString soft2 = QString("rgba(%1,%2,%3,%4)").arg(accent().red()).arg(accent().green()).arg(accent().blue()).arg(g_dark ? "0.44" : "0.27");
    QString qss = QString::fromLatin1(R"(
QMainWindow, QDialog { background: %WINDOW%; }
QMenuBar { background: %WINDOW%; padding: 2px 4px; }
QMenuBar::item { padding: 5px 10px; border-radius: 6px; background: transparent; }
QMenuBar::item:selected { background: %HOVER%; }
QMenu { background: %BASE%; border: 1px solid %MID%; border-radius: 8px; padding: 6px; }
QMenu::item { padding: 6px 28px 6px 14px; border-radius: 6px; }
QMenu::item:selected { background: %SOFT%; color: %TEXT%; }
QMenu::separator { height: 1px; background: %MID%; margin: 5px 8px; }
QToolBar { background: %WINDOW%; border: none; border-bottom: 1px solid %MID%; padding: 5px 8px; spacing: 3px; }
QToolBar::separator { width: 1px; background: %MID%; margin: 5px 7px; }
QToolButton { border: none; border-radius: 7px; padding: 5px; color: %TEXT%; }
QToolButton:hover { background: %HOVER%; }
QToolButton:pressed { background: %SOFT%; }
QToolButton:checked { background: %SOFT%; }
QTabWidget::pane { border: none; }
QTabBar { background: transparent; }
QTabBar::tab { padding: 7px 14px; margin: 3px 1px 0 1px; border-top-left-radius: 8px; border-top-right-radius: 8px;
               color: %DIM%; background: transparent; }
QTabBar::tab:hover { background: %HOVER%; }
QTabBar::tab:selected { background: %BASE%; color: %TEXT%; }
QSplitter::handle { background: %MID%; }
QSplitter::handle:horizontal { width: 1px; }
QTreeWidget, QListWidget { border: none; background: %BASE%; outline: 0; }
QTreeWidget::item { padding: 4px 2px; border-radius: 5px; }
QTreeWidget::item:hover, QListWidget::item:hover { background: %HOVER%; }
QTreeWidget::item:selected, QListWidget::item:selected { background: %SOFT%; color: %TEXT%; }
QTreeWidget::item:selected:focus, QListWidget::item:selected:focus { background: %SOFT2%; }
QListWidget::item { border-radius: 6px; }
QLineEdit, QSpinBox, QComboBox { border: 1px solid %MID%; border-radius: 6px; padding: 4px 8px; background: %BASE%; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid %ACCENT%; }
QSpinBox::up-button, QSpinBox::down-button { width: 0; border: none; }
QPushButton { border: 1px solid %MID%; border-radius: 6px; padding: 6px 14px; background: %BASE%; }
QPushButton:hover { border-color: %ACCENT%; }
QPushButton:default { background: %ACCENT%; color: %ONACCENT%; border-color: %ACCENT%; }
QStatusBar { background: %WINDOW%; border-top: 1px solid %MID%; color: %DIM%; }
QScrollBar:vertical { width: 12px; background: transparent; margin: 0; }
QScrollBar::handle:vertical { background: %SCROLL%; border-radius: 4px; min-height: 36px; margin: 2px; }
QScrollBar:horizontal { height: 12px; background: transparent; margin: 0; }
QScrollBar::handle:horizontal { background: %SCROLL%; border-radius: 4px; min-width: 36px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QToolTip { background: %BASE%; color: %TEXT%; border: 1px solid %MID%; padding: 4px 7px; }
)");
    qss.replace("%WINDOW%", window.name()).replace("%BASE%", base.name()).replace("%TEXT%", text.name());
    qss.replace("%MID%", mid.name()).replace("%DIM%", dim.name()).replace("%ACCENT%", accent().name());
    qss.replace("%ONACCENT%", g_dark ? "#10141a" : "#ffffff").replace("%HOVER%", hover).replace("%SOFT2%", soft2).replace("%SOFT%", soft);
    qss.replace("%SCROLL%", g_dark ? "rgba(255,255,255,0.25)" : "rgba(0,0,0,0.25)");
    app.setStyleSheet(qss);
}

// Icons are drawn in a 20x20 box so they stay crisp at any scale factor.
static void drawIcon(QPainter &p, const QString &n, const QColor &c)
{
    QPen pen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    auto poly = [&](std::initializer_list<QPointF> pts, bool close = false) {
        QPainterPath pp;
        bool first = true;
        for (const QPointF &q : pts) { if (first) pp.moveTo(q); else pp.lineTo(q); first = false; }
        if (close) pp.closeSubpath();
        p.drawPath(pp);
    };
    if (n == "open") {
        poly({{2.5, 15.5}, {2.5, 5}, {7.5, 5}, {9.5, 7}, {16.5, 7}, {16.5, 9.5}});
        poly({{2.5, 15.5}, {5, 9.5}, {18, 9.5}, {15.5, 15.5}}, true);
    } else if (n == "save") {
        poly({{3.5, 3.5}, {13.5, 3.5}, {16.5, 6.5}, {16.5, 16.5}, {3.5, 16.5}}, true);
        p.drawRect(QRectF(6.5, 3.5, 6, 4));
        p.drawRect(QRectF(6.5, 11, 7, 5.5));
    } else if (n == "undo" || n == "redo") {
        if (n == "redo") { p.translate(20, 0); p.scale(-1, 1); }
        poly({{7.5, 4.5}, {4, 8}, {7.5, 11.5}});
        path.moveTo(4, 8);
        path.lineTo(11.5, 8);
        path.arcTo(QRectF(7.5, 8, 8, 8), 90, -180);
        path.lineTo(7, 16);
        p.drawPath(path);
    } else if (n == "select") {
        poly({{5, 2.5}, {5, 15.5}, {8.3, 12.4}, {10.6, 17.3}, {12.6, 16.4}, {10.4, 11.6}, {14.8, 11.6}}, true);
    } else if (n == "hand") {
        p.drawLine(QPointF(10, 2.5), QPointF(10, 17.5));
        p.drawLine(QPointF(2.5, 10), QPointF(17.5, 10));
        poly({{7.8, 4.7}, {10, 2.5}, {12.2, 4.7}});
        poly({{7.8, 15.3}, {10, 17.5}, {12.2, 15.3}});
        poly({{4.7, 7.8}, {2.5, 10}, {4.7, 12.2}});
        poly({{15.3, 7.8}, {17.5, 10}, {15.3, 12.2}});
    } else if (n == "pen") {
        poly({{3, 17}, {4.2, 12.6}, {13.2, 3.6}, {16.4, 6.8}, {7.4, 15.8}}, true);
        p.drawLine(QPointF(11.4, 5.4), QPointF(14.6, 8.6));
    } else if (n == "marker") {
        poly({{6, 13.2}, {4.8, 10}, {12.3, 2.8}, {16.6, 7.1}, {9.2, 14.4}}, true);
        poly({{6, 13.2}, {4, 15.2}, {7.4, 15.8}, {9.2, 14.4}});
        p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.5, 18), QPointF(16.5, 18));
    } else if (n == "eraser") {
        poly({{7.6, 16}, {3, 11.4}, {11.2, 3.2}, {17, 9}, {10, 16}}, true);
        p.drawLine(QPointF(6.9, 7.5), QPointF(12.7, 13.3));
        p.drawLine(QPointF(7.6, 16), QPointF(17, 16));
    } else if (n == "text") {
        poly({{4.5, 6.5}, {4.5, 4.5}, {15.5, 4.5}, {15.5, 6.5}});
        p.drawLine(QPointF(10, 4.5), QPointF(10, 16));
        p.drawLine(QPointF(7.8, 16), QPointF(12.2, 16));
    } else if (n == "sign") {
        path.moveTo(2.5, 12.5);
        path.cubicTo(5, 2, 8, 2.5, 6.5, 9);
        path.cubicTo(5.5, 13.5, 9, 13.5, 10.5, 8.5);
        path.cubicTo(11, 12.5, 13.5, 12, 14.5, 9.5);
        path.cubicTo(15, 11.5, 16.5, 11.5, 17.5, 10.5);
        p.drawPath(path);
        p.drawLine(QPointF(3, 16.5), QPointF(17, 16.5));
    } else if (n == "zoomin" || n == "zoomout") {
        p.drawEllipse(QPointF(8.8, 8.8), 5.6, 5.6);
        p.drawLine(QPointF(13, 13), QPointF(17.2, 17.2));
        p.drawLine(QPointF(6.3, 8.8), QPointF(11.3, 8.8));
        if (n == "zoomin") p.drawLine(QPointF(8.8, 6.3), QPointF(8.8, 11.3));
    } else if (n == "fit") {
        p.drawLine(QPointF(3, 4), QPointF(3, 16));
        p.drawLine(QPointF(17, 4), QPointF(17, 16));
        p.drawLine(QPointF(5.8, 10), QPointF(14.2, 10));
        poly({{8, 7.8}, {5.8, 10}, {8, 12.2}});
        poly({{12, 7.8}, {14.2, 10}, {12, 12.2}});
    } else if (n == "back" || n == "forward") {
        if (n == "forward") { p.translate(20, 0); p.scale(-1, 1); }
        p.drawLine(QPointF(4, 10), QPointF(16, 10));
        poly({{9, 5}, {4, 10}, {9, 15}});
    } else if (n == "find") {
        p.drawEllipse(QPointF(8.8, 8.8), 5.6, 5.6);
        p.drawLine(QPointF(13, 13), QPointF(17.2, 17.2));
    } else if (n == "sidebar") {
        p.drawRoundedRect(QRectF(2.5, 4, 15, 12), 2, 2);
        p.drawLine(QPointF(7.5, 4), QPointF(7.5, 16));
    }
}

QIcon icon(const QString &name)
{
    QIcon ic;
    const QColor normal = g_dark ? QColor(0xdc, 0xe0, 0xe5) : QColor(0x3b, 0x42, 0x4c);
    for (int scale : {1, 2, 3}) {
        for (int state = 0; state < 2; ++state) {
            QPixmap pm(20 * scale, 20 * scale);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(scale, scale);
            drawIcon(p, name, state ? accent() : normal);
            p.end();
            pm.setDevicePixelRatio(scale);
            ic.addPixmap(pm, QIcon::Normal, state ? QIcon::On : QIcon::Off);
        }
    }
    return ic;
}

} // namespace Theme
