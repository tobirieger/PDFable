#include "Creator.h"
#include "MainWindow.h"
#include "Theme.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

// One running PDFable: a second start (e.g. double-clicking a PDF in Explorer) hands its
// files to the first one, which opens them as tabs.
static QString serverName()
{
    QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER", "user"));
    return "PDFable-" + user;
}

#ifdef Q_OS_WIN
// Windows remembers "Open with PDFable.exe" by file name only, under
// HKCU\Software\Classes\Applications\PDFable.exe, and keeps the path of the first copy that was
// ever chosen. When that copy is moved or deleted, double-clicking a PDF silently does nothing,
// even after picking the new copy as default app. So every start points the registration at the
// running executable (per user, no admin rights), and also registers a proper ProgID plus
// Capabilities so that PDFable is listed under Settings > Apps > Default apps.
static void registerFileAssociation()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString command = "\"" + exe + "\" \"%1\"";
    const QString icon = "\"" + exe + "\",0";
    bool changed = false;
    auto put = [&changed](QSettings &reg, const QString &key, const QString &value) {
        if (!reg.contains(key) || reg.value(key).toString() != value) {
            reg.setValue(key, value);
            changed = true;
        }
    };
    QSettings classes("HKEY_CURRENT_USER\\Software\\Classes", QSettings::NativeFormat);
    put(classes, "Applications/PDFable.exe/shell/open/command/Default", command);
    put(classes, "Applications/PDFable.exe/FriendlyAppName", "PDFable");
    put(classes, "Applications/PDFable.exe/DefaultIcon/Default", icon);
    put(classes, "Applications/PDFable.exe/SupportedTypes/.pdf", QString());
    put(classes, "PDFable.Document/Default", "PDF document");
    put(classes, "PDFable.Document/DefaultIcon/Default", icon);
    put(classes, "PDFable.Document/shell/open/command/Default", command);
    put(classes, ".pdf/OpenWithProgids/PDFable.Document", QString());
    QSettings software("HKEY_CURRENT_USER\\Software", QSettings::NativeFormat);
    put(software, "PDFable/Capabilities/ApplicationName", "PDFable");
    put(software, "PDFable/Capabilities/ApplicationDescription", "PDF reader for pen devices");
    put(software, "PDFable/Capabilities/FileAssociations/.pdf", "PDFable.Document");
    put(software, "RegisteredApplications/PDFable", "Software\\PDFable\\Capabilities");
    if (changed) {
        classes.sync();
        software.sync();
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}
#endif

static void raiseWindow(MainWindow *w)
{
    w->setWindowState((w->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    w->show();
    w->raise();
    w->activateWindow();
}

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName("PDFable");
    QCoreApplication::setApplicationName("PDFable");
    QCoreApplication::setApplicationVersion(PDFABLE_VERSION);
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);   // deliver every pen sample
    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, false);
    QApplication app(argc, argv);

#ifdef Q_OS_WIN
    registerFileAssociation();
#endif
    QStringList files;
    const QStringList args = app.arguments();
    bool creator = false;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--creator") creator = true;
        else files.push_back(QFileInfo(args[i]).absoluteFilePath());
    }
    auto openCreator = [](const QStringList &paths) {
        CreatorWindow *c = CreatorWindow::showCreator();
        QObject::connect(c, &CreatorWindow::openInReader, c, [](const QString &p) {
            MainWindow *w = MainWindow::windows().isEmpty() ? new MainWindow : MainWindow::windows().last();
            raiseWindow(w);
            w->openFile(p);
        }, Qt::UniqueConnection);
        c->addFiles(paths);
    };

    QLocalSocket probe;
    probe.connectToServer(serverName());
    if (probe.waitForConnected(300)) {
        probe.write(((creator ? QStringList{"--creator"} : QStringList()) + files).join('\n').toUtf8());
        probe.waitForBytesWritten(1000);
        probe.disconnectFromServer();
        return 0;
    }
    QLocalServer server;
    QLocalServer::removeServer(serverName());
    server.listen(serverName());
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&server, openCreator] {
        QLocalSocket *s = server.nextPendingConnection();
        QObject::connect(s, &QLocalSocket::disconnected, s, [s, openCreator] {
            QStringList paths = QString::fromUtf8(s->readAll()).split('\n', Qt::SkipEmptyParts);
            s->deleteLater();
            if (paths.value(0) == "--creator") {
                openCreator(paths.mid(1));
                return;
            }
            MainWindow *w = MainWindow::windows().isEmpty() ? new MainWindow : MainWindow::windows().last();
            raiseWindow(w);
            for (const QString &p : paths) w->openFile(p);
        });
    });

    Theme::apply(app);
    app.setWindowIcon(QIcon(":/app.png"));
    PdfView::setPageTheme(PageTheme(qBound(0, QSettings().value("view/pageTheme", 0).toInt(), 2)));
    PdfView::setScrollSpeed(QSettings().value("view/scrollSpeed", 0.8).toDouble());
    PdfView::setNoteAfterMarking(QSettings().value("notes/afterMarking", true).toBool());
    ink::Document::setAuthor(QSettings().value("notes/author", ink::Document::defaultAuthor()).toString());
    if (creator) {          // started as "PDFable Creator": only the creator window
        openCreator(files);
        return app.exec();
    }
    auto *w = new MainWindow;
    w->show();
    if (files.isEmpty()) MainWindow::restoreSession();
    for (const QString &f : files) w->openFile(f);
    return app.exec();
}
