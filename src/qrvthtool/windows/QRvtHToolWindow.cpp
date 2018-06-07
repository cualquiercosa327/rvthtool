/***************************************************************************
 * RVT-H Tool (librvth)                                                    *
 * QRvtHToolWindow.cpp: Main window.                                       *
 *                                                                         *
 * Copyright (c) 2018 by David Korth.                                      *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

#include "QRvtHToolWindow.hpp"

#include "librvth/rvth.h"
#include "RvtHModel.hpp"

// Qt includes.
#include <QtWidgets/QFileDialog>

/** QRvtHToolWindowPrivate **/

#include "ui_QRvtHToolWindow.h"
class QRvtHToolWindowPrivate
{
	public:
		explicit QRvtHToolWindowPrivate(QRvtHToolWindow *q);
		~QRvtHToolWindowPrivate();

	protected:
		QRvtHToolWindow *const q_ptr;
		Q_DECLARE_PUBLIC(QRvtHToolWindow)
	private:
		Q_DISABLE_COPY(QRvtHToolWindowPrivate)

	public:
		Ui::QRvtHToolWindow ui;

		// RVT-H Reader disk image
		RvtH *rvth;
		RvtHModel *model;

		// Filename.
		QString filename;
		QString displayFilename;	// filename without subdirectories

		/**
		 * Update the RVT-H Reader disk image's QTreeView.
		 */
		void updateLstBankList(void);

		/**
		 * Update the window title.
		 */
		void updateWindowTitle(void);
};

QRvtHToolWindowPrivate::QRvtHToolWindowPrivate(QRvtHToolWindow *q)
	: q_ptr(q)
	, rvth(nullptr)
	, model(new RvtHModel(q))
{
	// Connect the RvtHModel slots.
	QObject::connect(model, &RvtHModel::layoutChanged,
			 q, &QRvtHToolWindow::rvthModel_layoutChanged);
	QObject::connect(model, &RvtHModel::rowsInserted,
			 q, &QRvtHToolWindow::rvthModel_rowsInserted);
}

QRvtHToolWindowPrivate::~QRvtHToolWindowPrivate()
{
	// NOTE: Delete the MemCardModel first to prevent issues later.
	delete model;
	if (rvth) {
		rvth_close(rvth);
	}
}

/**
 * Update the RVT-H Reader disk image's QTreeView.
 */
void QRvtHToolWindowPrivate::updateLstBankList(void)
{
	if (!rvth) {
		// Set the group box's title.
		ui.grpBankList->setTitle(QRvtHToolWindow::tr("No RVT-H Reader disk image loaded."));
	} else {
		// Show the filename.
		ui.grpBankList->setTitle(displayFilename);
	}

	// Show the QTreeView headers if an RVT-H Reader disk image is loaded.
	ui.lstBankList->setHeaderHidden(!rvth);

	// Resize the columns to fit the contents.
	int num_sections = model->columnCount();
	for (int i = 0; i < num_sections; i++)
		ui.lstBankList->resizeColumnToContents(i);
	ui.lstBankList->resizeColumnToContents(num_sections);
}

/**
 * Update the window title.
 */
void QRvtHToolWindowPrivate::updateWindowTitle(void)
{
	QString windowTitle;
	if (rvth) {
		windowTitle += displayFilename;
		windowTitle += QLatin1String(" - ");
	}
	windowTitle += QApplication::applicationName();

	Q_Q(QRvtHToolWindow);
	q->setWindowTitle(windowTitle);
}

/** QRvtHToolWindow **/

QRvtHToolWindow::QRvtHToolWindow(QWidget *parent)
	: super(parent)
	, d_ptr(new QRvtHToolWindowPrivate(this))
{
	Q_D(QRvtHToolWindow);
	d->ui.setupUi(this);

	// Make sure the window is deleted on close.
	this->setAttribute(Qt::WA_DeleteOnClose, true);

#ifdef Q_OS_MAC
	// Remove the window icon. (Mac "proxy icon")
	// TODO: Use the memory card file?
	this->setWindowIcon(QIcon());
#endif /* Q_OS_MAC */

#ifdef Q_OS_WIN
	// Hide the QMenuBar border on Win32.
	// FIXME: This causes the menu bar to be "truncated" when using
	// the Aero theme on Windows Vista and 7.
#if 0
	this->Ui_QRvtHToolWindow::menuBar->setStyleSheet(
		QLatin1String("QMenuBar { border: none }"));
#endif
#endif

	// Set up the main splitter sizes.
	// We want the card info panel to be 160px wide at startup.
	// TODO: Save positioning settings somewhere?
	static const int BankInfoPanelWidth = 256;
	QList<int> sizes;
	sizes.append(this->width() - BankInfoPanelWidth);
	sizes.append(BankInfoPanelWidth);
	d->ui.splitterMain->setSizes(sizes);

	// Set the main splitter stretch factors.
	// We want the QTreeView to stretch, but not the card info panel.
	d->ui.splitterMain->setStretchFactor(0, 1);
	d->ui.splitterMain->setStretchFactor(1, 0);

	// Initialize lstBankList's item delegate.
	// TODO
	//d->ui.lstBankList->setItemDelegate(new RvtHItemDelegate(this));

	// Set the models.
	d->ui.lstBankList->setModel(d->model);

	// Initialize the UI.
	d->updateLstBankList();
	d->updateWindowTitle();
}

QRvtHToolWindow::~QRvtHToolWindow()
{
	delete d_ptr;
}

/**
 * Open an RVT-H Reader disk image.
 * @param filename Filename.
 */
void QRvtHToolWindow::openRvtH(const QString &filename)
{
	Q_D(QRvtHToolWindow);

	if (d->rvth) {
		d->model->setRvtH(nullptr);
		rvth_close(d->rvth);
	}

	// Open the specified RVT-H Reader disk image.
#ifdef _WIN32
	d->rvth = rvth_open(filename.toUtf8().constData(), nullptr);
#else /* !_WIN32 */
	d->rvth = rvth_open(reinterpret_cast<const wchar_t*>(filename.utf16()), nullptr);
#endif
	if (!d->rvth) {
		// FIXME: Show an error message?
		return;
	}

	d->filename = filename;
	d->model->setRvtH(d->rvth);

	// Extract the filename from the path.
	d->displayFilename = filename;
	int lastSlash = d->displayFilename.lastIndexOf(QChar(L'/'));
	if (lastSlash >= 0) {
		d->displayFilename.remove(0, lastSlash + 1);
	}

	// Update the UI.
	d->updateLstBankList();
	d->updateWindowTitle();

	// FIXME: If a file is opened from the command line,
	// QTreeView sort-of selects the first file.
	// (Signal is emitted, but nothing is highlighted.)
}

/**
 * Close the currently-opened RVT-H Reader disk image.
 */
void QRvtHToolWindow::closeRvtH(void)
{
	Q_D(QRvtHToolWindow);
	if (!d->rvth) {
		// Not open...
		return;
	}

	d->model->setRvtH(nullptr);
	rvth_close(d->rvth);
	d->rvth = nullptr;

	// Clear the filenames.
	d->filename.clear();
	d->displayFilename.clear();

	// Update the UI.
	d->updateLstBankList();
	d->updateWindowTitle();
}

/**
 * Widget state has changed.
 * @param event State change event.
 */
void QRvtHToolWindow::changeEvent(QEvent *event)
{
	Q_D(QRvtHToolWindow);

	switch (event->type()) {
		case QEvent::LanguageChange:
			// Retranslate the UI.
			d->ui.retranslateUi(this);
			d->updateLstBankList();
			d->updateWindowTitle();
			break;

		default:
			break;
	}

	// Pass the event to the base class.
	super::changeEvent(event);
}

/** UI widget slots. **/

/**
 * Open a memory card image.
 */
void QRvtHToolWindow::on_actionOpen_triggered(void)
{
	Q_D(QRvtHToolWindow);

	// TODO: Remove the space before the "*.raw"?
	// On Linux, Qt shows an extra space after the filter name, since
	// it doesn't show the extension. Not sure about Windows...
	const QString gcnFilter = tr("GameCube Memory Card Image") + QLatin1String(" (*.raw)");
	const QString vmuFilter = tr("Dreamcast VMU Image") + QLatin1String(" (*.bin)");
	const QString allFilter = tr("All Files") + QLatin1String(" (*)");

	// NOTE: Using a QFileDialog instead of QFileDialog::getOpenFileName()
	// causes a non-native appearance on Windows. Hence, we should use
	// QFileDialog::getOpenFileName().
	const QString filters = tr("Disk Image Files") + QLatin1String(" (*.img);;") +
		tr("All Files") + QLatin1String(" (*)");

	// Get the filename.
	// TODO: d->lastPath()
	QString filename = QFileDialog::getOpenFileName(this,
			tr("Open RVT-H Reader Disk Image"),	// Dialog title
			QString() /*d->lastPath()*/,		// Default filename
			filters);				// Filters

	if (!filename.isEmpty()) {
		// Filename is selected.
		// Open the RVT-H Reader disk image.
		openRvtH(filename);
	}
}

/**
 * Close the currently-opened memory card image.
 */
void QRvtHToolWindow::on_actionClose_triggered(void)
{
	Q_D(QRvtHToolWindow);
	if (!d->rvth)
		return;

	closeRvtH();
}

/**
 * Exit the program.
 * TODO: Separate close/exit for Mac OS X?
 */
void QRvtHToolWindow::on_actionExit_triggered(void)
{
	this->closeRvtH();
	this->close();
}

/**
 * Show the About dialog.
 */
void QRvtHToolWindow::on_actionAbout_triggered(void)
{
	// TODO
	//AboutDialog::ShowSingle(this);
}

/** RvtHModel slots. **/

void QRvtHToolWindow::rvthModel_layoutChanged(void)
{
	// Update the QTreeView columns, etc.
	// FIXME: This doesn't work the first time a file is added...
	// (possibly needs a dataChanged() signal)
	Q_D(QRvtHToolWindow);
	d->updateLstBankList();
}

void QRvtHToolWindow::rvthModel_rowsInserted(void)
{
	// A new file entry was added to the GcnCard.
	// Update the QTreeView columns.
	// FIXME: This doesn't work the first time a file is added...
	Q_D(QRvtHToolWindow);
	d->updateLstBankList();
}