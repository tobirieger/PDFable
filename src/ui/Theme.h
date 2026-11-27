// Look and feel: Fusion-based light/dark theme and the line icons used in the toolbar.
#pragma once
#include <QColor>
#include <QIcon>
#include <QProxyStyle>
#include <QString>

class QApplication;

namespace Theme {
enum class Mode { System = 0, Light = 1, Dark = 2 };
void apply(QApplication &app);     // reads the saved mode
void setMode(Mode m);              // re-colours the running application and saves the mode
Mode mode();
bool isDark();
QColor accent();
QIcon icon(const QString &name);

class PdfableStyle : public QProxyStyle {
public:
    PdfableStyle();
    QIcon standardIcon(StandardPixmap sp, const QStyleOption *opt = nullptr, const QWidget *w = nullptr) const override;
    int pixelMetric(PixelMetric m, const QStyleOption *opt = nullptr, const QWidget *w = nullptr) const override;
};
}
