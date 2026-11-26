#include "DocumentTab.h"
#include "Dialogs.h"
#include "NotesPanel.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QLocale>
#include <algorithm>

using namespace ink;

static void fillOutline(QTreeWidgetItem *parent, QTreeWidget *tree, const QVector<OutlineItem> &items)
{
    for (const OutlineItem &o : items) {
        auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        it->setText(0, o.title);
        it->setData(0, Qt::UserRole, o.page);
        it->setData(0, Qt::UserRole + 1, o.y);
        it->setToolTip(0, o.title);
        fillOutline(it, tree, o.children);
    }
}

DocumentTab::DocumentTab(std::shared_ptr<Document> doc, QWidget *parent) : QWidget(parent), doc_(std::move(doc))
{
    view_ = new PdfView(doc_, this);
    side_ = new QTabWidget;
    side_->setDocumentMode(true);

    outline_ = new QTreeWidget;
    outline_->setHeaderHidden(true);
    side_->addTab(outline_, tr("Outline"));

    pages_ = new QListWidget;
    pages_->setIconSize(QSize(120, 160));
    pages_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    pages_->setDragDropMode(QAbstractItemView::InternalMove);
    pages_->setDefaultDropAction(Qt::MoveAction);
    pages_->setSpacing(4);
    pages_->setContextMenuPolicy(Qt::CustomContextMenu);
    pages_->setToolTip(tr("Drag pages to reorder them. Right-click for more."));
    side_->addTab(pages_, tr("Pages"));
    notes_ = new NotesPanel(doc_, view_);
    side_->addTab(notes_, tr("Notes"));

    sigTree_ = new QTreeWidget;
    sigTree_->setHeaderHidden(true);
    sigTree_->setWordWrap(true);
    auto *sigPanel = sigPanel_ = new QWidget;
    auto *sl = new QVBoxLayout(sigPanel);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(sigTree_, 1);
    auto *trust = new QPushButton(tr("Trust this signer's certificate"));
    sl->addWidget(trust);
#ifdef PDFABLE_SIGNING
    side_->addTab(sigPanel, tr("Signatures"));
#else
    sigPanel->setParent(this);
    sigPanel->hide();
#endif

    searchBar_ = new QWidget;
    auto *sb = new QHBoxLayout(searchBar_);
    sb->setContentsMargins(6, 3, 6, 3);
    searchEdit_ = new QLineEdit;
    searchEdit_->setPlaceholderText(tr("Find in document"));
    searchEdit_->setClearButtonEnabled(true);
    auto *prev = new QToolButton, *next = new QToolButton, *close = new QToolButton;
    prev->setText("▲");
    next->setText("▼");
    close->setText("✕");
    searchInfo_ = new QLabel;
    sb->addWidget(searchEdit_, 1);
    sb->addWidget(prev);
    sb->addWidget(next);
    sb->addWidget(searchInfo_);
    sb->addStretch();
    sb->addWidget(close);
    searchBar_->hide();

    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    rl->addWidget(searchBar_);
    rl->addWidget(view_, 1);
    split_ = new QSplitter;
    split_->addWidget(side_);
    split_->addWidget(right);
    split_->setStretchFactor(1, 1);
    split_->setSizes({230, 900});
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split_);

    connect(outline_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        view_->gotoPage(it->data(0, Qt::UserRole).toInt(), it->data(0, Qt::UserRole + 1).toDouble());
    });
    connect(pages_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { view_->gotoPage(pages_->row(it), 0); });
    connect(pages_->model(), &QAbstractItemModel::rowsMoved, this, [this] { QTimer::singleShot(0, this, &DocumentTab::pagesMoved); });
    connect(pages_->verticalScrollBar(), &QScrollBar::valueChanged, this, &DocumentTab::requestThumbs);
    // the list got taller or shorter (the window is often resized right after a file was opened)
    connect(pages_->verticalScrollBar(), &QScrollBar::rangeChanged, this, &DocumentTab::requestThumbs);
    connect(side_, &QTabWidget::currentChanged, this, &DocumentTab::requestThumbs);
    connect(side_, &QTabWidget::currentChanged, this, &DocumentTab::updateNotesActive);
    notesTimer_ = new QTimer(this);
    notesTimer_->setSingleShot(true);
    notesTimer_->setInterval(150);
    connect(notesTimer_, &QTimer::timeout, notes_, &NotesPanel::reload);
    connect(doc_.get(), &Document::pageChanged, notesTimer_, qOverload<>(&QTimer::start));
    connect(doc_.get(), &Document::structureChanged, notesTimer_, qOverload<>(&QTimer::start));
    connect(view_, &PdfView::noteRequested, this, [this](int page, int id) {
        showNotes();
        notes_->editNote(page, id);
    });
    connect(pages_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        QMenu m(this);
        m.addAction(tr("Rotate left"), this, [this] { rotatePages(-90); });
        m.addAction(tr("Rotate right"), this, [this] { rotatePages(90); });
        m.addSeparator();
        m.addAction(tr("Extract to new PDF…"), this, &DocumentTab::extractPages);
        m.addAction(tr("Insert pages from PDF…"), this, &DocumentTab::insertPdf);
        m.addAction(tr("Insert blank page"), this, &DocumentTab::insertBlank);
        m.addSeparator();
        m.addAction(tr("Delete"), this, &DocumentTab::deletePages);
        m.exec(pages_->mapToGlobal(p));
    });
    connect(view_->worker(), &RenderWorker::rendered, this, [this](const RenderJob &job, const QImage &img) {
        if (job.kind != 1 || job.gen != doc_->generation() || img.isNull()) return;
        if (QListWidgetItem *it = pages_->item(job.page)) {
            QPixmap pm(pages_->iconSize());
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            QImage sc = img.scaled(pm.size() - QSize(2, 2), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QRect r(QPoint((pm.width() - sc.width()) / 2, (pm.height() - sc.height()) / 2), sc.size());
            p.drawImage(r, sc);
            p.setPen(QColor(120, 120, 120));
            p.drawRect(r.adjusted(0, 0, -1, -1));
            it->setIcon(QIcon(pm));
            it->setData(Qt::UserRole + 1, true);
        }
    });
    watcher_ = new QFileSystemWatcher(this);
    reloadTimer_.setSingleShot(true);
    reloadTimer_.setInterval(600);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this] { reloadTries_ = 0; reloadTimer_.start(); });
    connect(&reloadTimer_, &QTimer::timeout, this, &DocumentTab::reloadFromDisk);
    watchFile();
    connect(doc_.get(), &Document::structureChanged, this, [this] {
        watchFile();
        rebuildOutline();
        rebuildPages();
        refreshSignatures();
        emit titleChanged();
    });
    connect(doc_.get(), &Document::pageChanged, this, [this](int page) {
        // refresh the thumbnail of an edited page lazily
        for (int i = 0; i < pages_->count(); ++i)
            if (page < 0 || i == page) pages_->item(i)->setData(Qt::UserRole + 1, false);
        QTimer::singleShot(400, this, &DocumentTab::requestThumbs);
    });
    connect(doc_.get(), &Document::modifiedChanged, this, &DocumentTab::titleChanged);
    connect(view_, &PdfView::statusMessage, this, &DocumentTab::statusMessage);
    connect(view_, &PdfView::currentPageChanged, this, [this](int pg) {
        if (pages_->isVisible() && pages_->selectedItems().size() <= 1 && pg < pages_->count()) {
            QSignalBlocker b(pages_);
            pages_->setCurrentRow(pg);
        }
    });
    connect(searchEdit_, &QLineEdit::textChanged, view_, &PdfView::setSearch);
    connect(searchEdit_, &QLineEdit::returnPressed, this, [this] { view_->findNext(!(QApplication::keyboardModifiers() & Qt::ShiftModifier)); });
    connect(next, &QToolButton::clicked, this, [this] { view_->findNext(true); });
    connect(prev, &QToolButton::clicked, this, [this] { view_->findNext(false); });
    connect(close, &QToolButton::clicked, this, [this] {
        searchBar_->hide();
        searchEdit_->clear();
        view_->setFocus();
    });
    connect(view_, &PdfView::searchChanged, this, [this](int total, int cur) {
        searchInfo_->setText(searchEdit_->text().isEmpty() ? QString() : total == 0 ? tr("No matches") : cur > 0 ? tr("%1 of %2").arg(cur).arg(total) : tr("%1 matches").arg(total));
    });
    connect(trust, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem *it = sigTree_->currentItem();
        while (it && it->parent()) it = it->parent();
        int idx = it ? sigTree_->indexOfTopLevelItem(it) : (sigs_.size() == 1 ? 0 : -1);
        if (idx < 0 || idx >= sigs_.size() || sigs_[idx].certDer.isEmpty()) return;
        if (QMessageBox::question(this, tr("Trust certificate"),
                                  tr("Trust the certificate of “%1” for all documents?\nOnly do this if you verified it with the signer.").arg(sigs_[idx].signerCn)) != QMessageBox::Yes)
            return;
        SignatureLibrary::addTrustedCert(sigs_[idx].certDer);
        refreshSignatures();
    });

    rebuildOutline();
    rebuildPages();
    refreshSignatures();
    side_->setCurrentIndex(outline_->topLevelItemCount() ? 0 : 1);
    notes_->reload();
}

void DocumentTab::watchFile()
{
    const QStringList old = watcher_->files();
    if (old.size() == 1 && old.first() == doc_->path()) return;
    if (!old.isEmpty()) watcher_->removePaths(old);
    if (!doc_->path().isEmpty()) watcher_->addPath(doc_->path());
}

// Another program rewrote the file (typically a LaTeX run): show the new version, keep the position.
void DocumentTab::reloadFromDisk()
{
    watchFile();
    try {
        view_->flushInk();
        if (doc_->isModified()) {
            emit statusMessage(tr("The file changed on disk. Not reloaded because you have unsaved changes."));
            return;
        }
        if (doc_->reloadIfChanged()) emit statusMessage(tr("Reloaded: the file changed on disk"));
    } catch (const PdfError &) {
        if (++reloadTries_ < 8) reloadTimer_.start();    // probably still being written
    }
}

QString DocumentTab::title() const
{
    QString n = doc_->path().isEmpty() ? tr("Untitled") : QFileInfo(doc_->path()).fileName();
    return doc_->isModified() ? n + " *" : n;
}

template <class F> void DocumentTab::guarded(F &&f)
{
    try {
        view_->flushInk();
        f();
    } catch (const PdfError &e) {
        QMessageBox::warning(this, tr("PDFable"), e.message());
    }
}

void DocumentTab::rebuildOutline()
{
    outline_->clear();
    fillOutline(nullptr, outline_, doc_->outline());
    if (outline_->topLevelItemCount() <= 12) outline_->expandToDepth(0);
}

void DocumentTab::rebuildPages()
{
    rebuilding_ = true;
    pages_->clear();
    for (int i = 0; i < doc_->pageCount(); ++i) {
        auto *it = new QListWidgetItem(QString::number(i + 1));
        it->setData(Qt::UserRole, i);
        it->setSizeHint(QSize(150, 172));
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        pages_->addItem(it);
    }
    rebuilding_ = false;
    QTimer::singleShot(0, this, &DocumentTab::requestThumbs);
}

void DocumentTab::requestThumbs()
{
    if (!pages_->isVisible()) return;
    QRect vr = pages_->viewport()->rect();
    for (int i = 0; i < pages_->count(); ++i) {
        QListWidgetItem *it = pages_->item(i);
        if (it->data(Qt::UserRole + 1).toBool()) continue;
        if (!pages_->visualItemRect(it).intersects(vr)) continue;
        QSizeF ps = doc_->pageSize(i);
        double s = qMin(240.0 / ps.width(), 320.0 / ps.height());
        view_->worker()->request(RenderJob{1, i, s, QRect(0, 0, int(ps.width() * s), int(ps.height() * s)), doc_->generation()});
    }
}

void DocumentTab::pagesMoved()
{
    if (rebuilding_) return;
    QVector<int> order;
    for (int i = 0; i < pages_->count(); ++i) order.push_back(pages_->item(i)->data(Qt::UserRole).toInt());
    rearrange(order);
}

void DocumentTab::rearrange(const QVector<int> &order)
{
    guarded([&] { doc_->rearrange(order); });
    if (pages_->count() != doc_->pageCount()) rebuildPages();
}

QVector<int> DocumentTab::selectedPages() const
{
    QVector<int> out;
    if (pages_->isVisible())
        for (QListWidgetItem *it : pages_->selectedItems()) out.push_back(pages_->row(it));
    if (out.isEmpty() && doc_->pageCount() > 0) out.push_back(view_->currentPage());
    std::sort(out.begin(), out.end());
    return out;
}

void DocumentTab::rotatePages(int delta)
{
    QVector<int> sel = selectedPages();
    guarded([&] { doc_->rotatePages(sel, delta); });
}

void DocumentTab::deletePages()
{
    QVector<int> sel = selectedPages();
    if (sel.size() >= doc_->pageCount()) {
        QMessageBox::information(this, tr("Delete pages"), tr("A document needs at least one page."));
        return;
    }
    QVector<int> order;
    for (int i = 0; i < doc_->pageCount(); ++i)
        if (!sel.contains(i)) order.push_back(i);
    guarded([&] { doc_->rearrange(order); });
}

void DocumentTab::extractPages()
{
    QVector<int> sel = selectedPages();
    QString base = doc_->path().isEmpty() ? QString("pages.pdf") : QFileInfo(doc_->path()).absolutePath() + "/" + QFileInfo(doc_->path()).completeBaseName() + "_pages.pdf";
    QString out = QFileDialog::getSaveFileName(this, tr("Extract %n page(s)", nullptr, sel.size()), base, tr("PDF (*.pdf)"));
    if (out.isEmpty()) return;
    guarded([&] {
        doc_->extractPages(sel, out);
        emit statusMessage(tr("Extracted %1 page(s) to %2").arg(sel.size()).arg(out));
    });
}

void DocumentTab::insertPdf()
{
    QString f = QFileDialog::getOpenFileName(this, tr("Insert pages from"), QFileInfo(doc_->path()).absolutePath(), tr("PDF (*.pdf)"));
    if (f.isEmpty()) return;
    int at = selectedPages().last() + 1;
    guarded([&] { doc_->insertPdf(f, at); });
}

void DocumentTab::insertBlank()
{
    int cur = selectedPages().last();
    guarded([&] { doc_->insertBlank(cur + 1, doc_->pageSize(cur)); });
}

void DocumentTab::undo() { guarded([&] { if (doc_->canUndo()) doc_->undo(); }); }
void DocumentTab::redo() { guarded([&] { if (doc_->canRedo()) doc_->redo(); }); }

bool DocumentTab::save()
{
    if (doc_->path().isEmpty()) return saveAs();
    bool ok = false;
    guarded([&] {
        doc_->save(doc_->path());
        ok = true;
        emit statusMessage(tr("Saved %1").arg(doc_->path()));
    });
    return ok;
}

bool DocumentTab::saveAs(bool optimize)
{
    QString out = QFileDialog::getSaveFileName(this, optimize ? tr("Save optimised copy") : tr("Save as"), doc_->path(), tr("PDF (*.pdf)"));
    if (out.isEmpty()) return false;
    bool ok = false;
    guarded([&] {
        doc_->save(out, optimize);
        ok = true;
        emit statusMessage(tr("Saved %1").arg(out));
    });
    return ok;
}

bool DocumentTab::maybeSave()
{
    try { view_->flushInk(); } catch (const PdfError &) {}
    if (!doc_->isModified()) return true;
    auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to %1?").arg(title().remove(" *")),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    return r == QMessageBox::Discard || save();
}

void DocumentTab::print()
{
    view_->flushInk();
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, this);
    dlg.setMinMax(1, doc_->pageCount());
    if (dlg.exec() != QDialog::Accepted) return;
    int from = printer.fromPage() > 0 ? printer.fromPage() - 1 : 0;
    int to = printer.toPage() > 0 ? printer.toPage() - 1 : doc_->pageCount() - 1;
    QPainter p(&printer);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int i = from; i <= to && i < doc_->pageCount(); ++i) {
        if (i > from) printer.newPage();
        QSizeF ps = doc_->pageSize(i);
        double s = 220.0 / 72.0;
        QImage img;
        try { img = doc_->render(i, s, QRect(0, 0, int(ps.width() * s), int(ps.height() * s))); } catch (const PdfError &) {}
        if (img.isNull()) continue;
        QRectF target = p.viewport();
        QSizeF sz = QSizeF(img.size()).scaled(target.size(), Qt::KeepAspectRatio);
        p.drawImage(QRectF(QPointF((target.width() - sz.width()) / 2, (target.height() - sz.height()) / 2), sz), img);
    }
    QApplication::restoreOverrideCursor();
}

void DocumentTab::showSearch()
{
    searchBar_->show();
    QString sel = view_->selectedText().simplified();
    if (!sel.isEmpty() && sel.size() < 80) searchEdit_->setText(sel);
    searchEdit_->setFocus();
    searchEdit_->selectAll();
}

void DocumentTab::toggleSidebar()
{
    side_->setVisible(!side_->isVisible());
    updateNotesActive();
}

void DocumentTab::updateNotesActive() { notes_->setActive(side_->isVisible() && side_->currentWidget() == notes_); }

void DocumentTab::showNotes()
{
    side_->show();
    side_->setCurrentWidget(notes_);
    updateNotesActive();
}

void DocumentTab::showSignatures()
{
    side_->show();
    if (side_->indexOf(sigPanel_) >= 0) side_->setCurrentWidget(sigPanel_);
}

void DocumentTab::refreshSignatures()
{
    sigTree_->clear();
    int sigTab = side_->indexOf(sigPanel_);
    if (sigTab < 0) return;
    sigs_ = doc_->signatures(SignatureLibrary::trustedCerts());
    sigCount_ = sigs_.size();
    side_->setTabText(sigTab, sigCount_ ? tr("Signatures (%1)").arg(sigCount_) : tr("Signatures"));
    if (sigs_.isEmpty()) {
        new QTreeWidgetItem(sigTree_, {tr("This document has no digital signatures.")});
        return;
    }
    for (const SignatureInfo &s : std::as_const(sigs_)) {
        QString who = s.signerCn.isEmpty() ? s.name : s.signerCn;
        QString head;
        if (s.isDocTimestamp) head = tr("🕓 Document timestamp");
        else if (!s.parsed || !s.digestOk) head = tr("✖ INVALID signature of %1").arg(who);
        else if (!s.coversWholeFile) head = tr("⚠ Signed by %1; document changed afterwards").arg(who);
        else if (!s.trusted) head = tr("⚠ Signed by %1; identity not verified").arg(who);
        else head = tr("✔ Signed by %1").arg(who);
        auto *top = new QTreeWidgetItem(sigTree_, {head});
        auto add = [&](const QString &t) { auto *c = new QTreeWidgetItem(top, {t}); c->setToolTip(0, t); };
        if (!s.error.isEmpty()) add(s.error);
        if (s.parsed && s.digestOk)
            add(s.coversWholeFile ? tr("The document has not been modified since it was signed.")
                                  : tr("The signed revision is intact, but content was added after this signature."));
        if (!s.isDocTimestamp)
            add(s.trusted ? tr("The signer's certificate is trusted.")
                          : tr("Certificate not trusted: %1").arg(s.selfSigned ? tr("self-signed") : s.trustError));
        if (s.docMdp) add(tr("Certified document (permission level %1)").arg(s.docMdp));
        if (s.hasTimestamp) add(tr("Timestamp: %1").arg(QLocale().toString(s.timestampTime.toLocalTime(), QLocale::ShortFormat)));
        else if (s.claimedTime.isValid()) add(tr("Signing time (signer's clock): %1").arg(QLocale().toString(s.claimedTime, QLocale::ShortFormat)));
        if (!s.issuerCn.isEmpty()) add(tr("Issued by: %1").arg(s.issuerCn));
        if (s.notAfter.isValid()) add(tr("Certificate valid until %1").arg(QLocale().toString(s.notAfter.date(), QLocale::ShortFormat)));
        if (!s.reason.isEmpty()) add(tr("Reason: %1").arg(s.reason));
        if (!s.location.isEmpty()) add(tr("Location: %1").arg(s.location));
        add(tr("Format: %1, page %2").arg(s.subFilter).arg(s.page + 1));
        top->setExpanded(true);
    }
}
