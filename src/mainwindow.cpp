/*
  mainwindow.cpp

  This file is part of Hotspot, the Qt GUI for performance analysis.

  Copyright (C) 2016-2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Nate Rogers <nate.rogers@kdab.com>

  Licensees holding valid commercial KDAB Hotspot licenses may use this file in
  accordance with Hotspot Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "startpage.h"
#include "recordpage.h"
#include "resultspage.h"

#include <QFileDialog>
#include <QApplication>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <QStandardPaths>
#include <QProcess>
#include <QInputDialog>
#include <QDesktopServices>
#include <QWidgetAction>
#include <QLineEdit>
#include <QLabel>

#include <KStandardAction>
#include <KConfigGroup>
#include <KRecentFilesAction>

#include "aboutdialog.h"

#include "parsers/perf/perfparser.h"

#include <functional>

namespace {
struct IdeSettings {
    const char * const app;
    const char * const args;
    const char * const name;
    const char * const icon;
};

static const IdeSettings ideSettings[] = {
#if defined(Q_OS_WIN) || defined(Q_OS_OSX)
    {"", "", "", ""} // Dummy content, because we can't have empty arrays.
#else
    { "kdevelop", "%f:%l:%c", QT_TRANSLATE_NOOP("MainWindow", "KDevelop"), "kdevelop" },
    { "kate", "%f --line %l --column %c", QT_TRANSLATE_NOOP("MainWindow", "Kate"), "kate" },
    { "kwrite", "%f --line %l --column %c", QT_TRANSLATE_NOOP("MainWindow", "KWrite"), nullptr },
    { "gedit", "%f +%l:%c", QT_TRANSLATE_NOOP("MainWindow", "gedit"), nullptr },
    { "gvim", "%f +%l", QT_TRANSLATE_NOOP("MainWindow", "gvim"), nullptr },
    { "qtcreator", "%f", QT_TRANSLATE_NOOP("MainWindow", "Qt Creator"), nullptr }
#endif
};
#if defined(Q_OS_WIN) || defined(Q_OS_OSX) // Remove this #if branch when adding real data to ideSettings for Windows/OSX.
    static const int ideSettingsSize = 0;
#else
    static const int ideSettingsSize = sizeof(ideSettings) / sizeof(IdeSettings);
#endif
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_parser(new PerfParser(this)),
    m_config(KSharedConfig::openConfig()),
    m_pageStack(new QStackedWidget(this)),
    m_startPage(new StartPage(this)),
    m_recordPage(new RecordPage(this)),
    m_resultsPage(new ResultsPage(m_parser, this))
{
    ui->setupUi(this);

    m_pageStack->addWidget(m_startPage);
    m_pageStack->addWidget(m_resultsPage);
    m_pageStack->addWidget(m_recordPage);

    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(m_pageStack);
    centralWidget()->setLayout(layout);

    connect(m_startPage, &StartPage::openFileButtonClicked, this, &MainWindow::onOpenFileButtonClicked);
    connect(m_startPage, &StartPage::recordButtonClicked, this, &MainWindow::onRecordButtonClicked);
    connect(m_parser, &PerfParser::progress, m_startPage, &StartPage::onParseFileProgress);
    connect(this, &MainWindow::openFileError, m_startPage, &StartPage::onOpenFileError);
    connect(m_recordPage, &RecordPage::homeButtonClicked, this, &MainWindow::onHomeButtonClicked);
    connect(m_recordPage, &RecordPage::openFile,
            this, static_cast<void (MainWindow::*)(const QString&)>(&MainWindow::openFile));

    connect(m_parser, &PerfParser::parsingFinished,
            this, [this] () {
                m_pageStack->setCurrentWidget(m_resultsPage);
            });
    connect(m_parser, &PerfParser::parsingFailed,
            this, [this] (const QString& errorMessage) {
                emit openFileError(errorMessage);
            });

    auto *recordDataAction = new QAction(this);
    recordDataAction->setText(QStringLiteral("&Record Data"));
    recordDataAction->setIcon(QIcon::fromTheme(QStringLiteral("media-record")));
    recordDataAction->setShortcut(Qt::CTRL + Qt::Key_R);
    ui->fileMenu->addAction(recordDataAction);
    connect(recordDataAction, &QAction::triggered, this, &MainWindow::onRecordButtonClicked);

    connect(m_resultsPage, &ResultsPage::navigateToCode, this, &MainWindow::navigateToCode);
    ui->fileMenu->addAction(KStandardAction::open(this, SLOT(onOpenFileButtonClicked()), this));
    m_recentFilesAction = KStandardAction::openRecent(this, SLOT(openFile(QUrl)), this);
    m_recentFilesAction->loadEntries(m_config->group("RecentFiles"));
    ui->fileMenu->addAction(m_recentFilesAction);
    ui->fileMenu->addAction(KStandardAction::clear(this, SLOT(clear()), this));
    ui->fileMenu->addAction(KStandardAction::close(this, SLOT(close()), this));
    connect(ui->actionAbout_Qt, &QAction::triggered,
            qApp, &QApplication::aboutQt);
    connect(ui->actionAbout_KDAB, &QAction::triggered,
            this, &MainWindow::aboutKDAB);
    connect(ui->actionAbout_Hotspot, &QAction::triggered,
            this, &MainWindow::aboutHotspot);

    setupCodeNavigationMenu();
    setupPathSettingsMenu();

    clear();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_parser->stop();
    QMainWindow::closeEvent(event);
}

void MainWindow::setSysroot(const QString& path)
{
    m_sysroot = path;
    m_resultsPage->setSysroot(path);
}

void MainWindow::setKallsyms(const QString& path)
{
    m_kallsyms = path;
}

void MainWindow::setDebugPaths(const QString& paths)
{
    m_debugPaths = paths;
}

void MainWindow::setExtraLibPaths(const QString& paths)
{
    m_extraLibPaths = paths;
}

void MainWindow::setAppPath(const QString& path)
{
    m_appPath = path;
    m_resultsPage->setAppPath(path);
}

void MainWindow::setArch(const QString& arch)
{
    m_arch = arch;
}

void MainWindow::onOpenFileButtonClicked()
{
    const auto fileName = QFileDialog::getOpenFileName(this, tr("Open File"), QDir::currentPath(),
                                                       tr("Data Files (perf*.data perf.data.*);;All Files (*)"));
    if (fileName.isEmpty()) {
        return;
    }

    openFile(fileName);
}

void MainWindow::onHomeButtonClicked()
{
    clear();
    m_pageStack->setCurrentWidget(m_startPage);
}

void MainWindow::onRecordButtonClicked()
{
    clear();
    m_recordPage->showRecordPage();
    m_pageStack->setCurrentWidget(m_recordPage);
}

void MainWindow::clear()
{
    m_parser->stop();
    m_startPage->showStartPage();
    m_pageStack->setCurrentWidget(m_startPage);
    m_recordPage->stopRecording();
    m_resultsPage->selectSummaryTab();
}


void MainWindow::openFile(const QString& path)
{
    clear();

    QFileInfo file(path);
    setWindowTitle(tr("%1 - Hotspot").arg(file.fileName()));

    m_startPage->showParseFileProgress();
    m_pageStack->setCurrentWidget(m_startPage);

    // TODO: support input files of different types via plugins
    m_parser->startParseFile(path, m_sysroot, m_kallsyms, m_debugPaths,
                             m_extraLibPaths, m_appPath, m_arch);

    m_recentFilesAction->addUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
    m_recentFilesAction->saveEntries(m_config->group("RecentFiles"));
    m_config->sync();
}

void MainWindow::openFile(const QUrl& url)
{
    if (!url.isLocalFile()) {
        emit openFileError(tr("Cannot open remote file %1.").arg(url.toString()));
        return;
    }
    openFile(url.toLocalFile());
}
void MainWindow::aboutKDAB()
{
    AboutDialog dialog(this);
    dialog.setWindowTitle(tr("About KDAB"));
    dialog.setTitle(trUtf8("Klarälvdalens Datakonsult AB (KDAB)"));
    dialog.setText(
        tr("<qt><p>Hotspot is supported and maintained by KDAB</p>"
           "<p>KDAB, the Qt experts, provide consulting and mentoring for developing "
           "Qt applications from scratch and in porting from all popular and legacy "
           "frameworks to Qt. We continue to help develop parts of Qt and are one "
           "of the major contributors to the Qt Project. We can give advanced or "
           "standard trainings anywhere around the globe.</p>"
           "<p>Please visit <a href='https://www.kdab.com'>https://www.kdab.com</a> "
           "to meet the people who write code like this."
           "</p></qt>"));
    dialog.setLogo(QStringLiteral(":/images/kdablogo.png"));
    dialog.setWindowIcon(QPixmap(QStringLiteral(":/images/kdablogo.png")));
    dialog.exec();
}

void MainWindow::aboutHotspot()
{
    AboutDialog dialog(this);
    dialog.setWindowTitle(tr("About Hotspot"));
    dialog.setTitle(tr("Hotspot - the Linux perf GUI for performance analysis"));
    dialog.setText(
        tr("<qt><p>Hotspot is supported and maintained by KDAB</p>"
           "<p>This project is a KDAB R&D effort to create a standalone GUI for performance data. "
           "As the first goal, we want to provide a UI like KCachegrind around Linux perf. "
           "Looking ahead, we intend to support various other performance data formats "
           "under this umbrella.</p>"
           "<p>Hotspot is an open source project:</p>"
           "<ul>"
           "<li><a href=\"https://github.com/KDAB/hotspot\">GitHub project page</a></li>"
           "<li><a href=\"https://github.com/KDAB/hotspot/issues\">Issue Tracker</a></li>"
           "<li><a href=\"https://github.com/KDAB/hotspot/graphs/contributors\">Contributors</a></li>"
           "</ul><p>Patches welcome!</p></qt>"));
    dialog.setLogo(QStringLiteral(":/images/hotspot_logo.png"));
    dialog.setWindowIcon(QIcon::fromTheme(QStringLiteral("hotspot")));
    dialog.adjustSize();
    dialog.exec();
}

void MainWindow::setupPathSettingsMenu()
{
    auto menu = new QMenu(this);
    auto addPathAction = [menu] (const QString& label,
                                 std::function<void(const QString& newValue)> setPath,
                                 QString* value,
                                 const QString& placeHolder,
                                 const QString& tooltip)
    {
        auto action = new QWidgetAction(menu);
        auto container = new QWidget;
        auto layout = new QHBoxLayout;
        layout->addWidget(new QLabel(label));
        auto lineEdit = new QLineEdit;
        lineEdit->setPlaceholderText(placeHolder);
        lineEdit->setText(*value);
        connect(lineEdit, &QLineEdit::textChanged,
                lineEdit, [setPath] (const QString& newValue) { setPath(newValue); });
        layout->addWidget(lineEdit);
        container->setToolTip(tooltip);
        container->setLayout(layout);
        action->setDefaultWidget(container);
        menu->addAction(action);
    };
    addPathAction(tr("Sysroot:"),
                  [this] (const QString& newValue) { setSysroot(newValue); },
                  &m_sysroot,
                  tr("local machine"),
                  tr("Path to the sysroot. Leave empty to use the local machine."));
    addPathAction(tr("Application Path:"),
                  [this] (const QString& newValue) { setAppPath(newValue); },
                  &m_appPath,
                  tr("auto-detect"),
                  tr("Path to the application binary and library."));
    addPathAction(tr("Extra Library Paths:"),
                  [this] (const QString& newValue) { setExtraLibPaths(newValue); },
                  &m_extraLibPaths,
                  tr("empty"),
                  tr("List of colon-separated paths that contain additional libraries."));
    addPathAction(tr("Debug Paths:"),
                  [this] (const QString& newValue) { setDebugPaths(newValue); },
                  &m_debugPaths,
                  tr("auto-detect"),
                  tr("List of colon-separated paths that contain debug information."));
    addPathAction(tr("Kallsyms:"),
                  [this] (const QString& newValue) { setKallsyms(newValue); },
                  &m_kallsyms,
                  tr("auto-detect"),
                  tr("Path to the kernel symbol mapping."));
    addPathAction(tr("Architecture:"),
                  [this] (const QString& newValue) { setArch(newValue); },
                  &m_arch,
                  tr("auto-detect"),
                  tr("System architecture, e.g. x86_64, arm, aarch64 etc."));
    m_startPage->setPathSettingsMenu(menu);
}

void MainWindow::setupCodeNavigationMenu()
{
    // Code Navigation
    QAction *configAction = new QAction(QIcon::fromTheme(QStringLiteral(
                                        "applications-development")),
                                        tr("Code Navigation"), this);
    auto menu = new QMenu(this);
    auto group = new QActionGroup(this);
    group->setExclusive(true);

    const auto settings = m_config->group("CodeNavigation");
    const auto currentIdx = settings.readEntry("IDE", -1);

    for (int i = 0; i < ideSettingsSize; ++i) {
        auto action = new QAction(menu);
        action->setText(tr(ideSettings[i].name));
        if (ideSettings[i].icon)
            action->setIcon(QIcon::fromTheme(QString::fromUtf8(ideSettings[i].icon)));
        action->setCheckable(true);
        action->setChecked(currentIdx == i);
        action->setData(i);
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0) // It's not worth it to reimplement missing findExecutable for Qt4.
        action->setEnabled(!QStandardPaths::findExecutable(QString::fromUtf8(ideSettings[i].app)).isEmpty());
#endif
        group->addAction(action);
        menu->addAction(action);
    }
    menu->addSeparator();

    QAction *action = new QAction(menu);
    action->setText(tr("Custom..."));
    action->setCheckable(true);
    action->setChecked(currentIdx == -1);
    action->setData(-1);
    group->addAction(action);
    menu->addAction(action);

#if defined(Q_OS_WIN) || defined(Q_OS_OSX)
    // This is a workaround for the cases, where we can't safely do assumptions
    // about the install location of the IDE
    action = new QAction(menu);
    action->setText(tr("Automatic (No Line numbers)"));
    action->setCheckable(true);
    action->setChecked(currentIdx == -2);
    action->setData(-2);
    group->addAction(action);
    menu->addAction(action);
#endif

    QObject::connect(group, &QActionGroup::triggered, this, &MainWindow::setCodeNavigationIDE);

    configAction->setMenu(menu);
    ui->settingsMenu->addMenu(menu);
}

void MainWindow::setCodeNavigationIDE(QAction *action)
{
    auto settings = m_config->group("CodeNavigation");

    if (action->data() == -1) {
        const auto customCmd = QInputDialog::getText(
            this, tr("Custom Code Navigation"),
            tr("Specify command to use for code navigation, '%f' will be replaced by the file name, '%l' by the line number and '%c' by the column number."),
            QLineEdit::Normal, settings.readEntry("CustomCommand")
            );
        if (!customCmd.isEmpty()) {
            settings.writeEntry("CustomCommand", customCmd);
            settings.writeEntry("IDE", -1);
        }
        return;
    }

    const auto defaultIde = action->data().toInt();
    settings.writeEntry("IDE", defaultIde);
}

void MainWindow::navigateToCode(const QString &filePath, int lineNumber, int columnNumber)
{
    const auto settings = m_config->group("CodeNavigation");
    const auto ideIdx = settings.readEntry("IDE", -1);

    QString command;
#if !defined(Q_OS_WIN) && !defined(Q_OS_OSX) // Remove this #if branch when adding real data to ideSettings for Windows/OSX.
    if (ideIdx >= 0 && ideIdx < ideSettingsSize) {
        command += QString::fromUtf8(ideSettings[ideIdx].app);
        command += QString::fromUtf8(" ");
        command += QString::fromUtf8(ideSettings[ideIdx].args);
    } else
#endif
    if (ideIdx == -1) {
        command = settings.readEntry("CustomCommand");
    }

    if (!command.isEmpty()) {
        command.replace(QStringLiteral("%f"), filePath);
        command.replace(QStringLiteral("%l"), QString::number(std::max(1, lineNumber)));
        command.replace(QStringLiteral("%c"), QString::number(std::max(1, columnNumber)));

        QProcess::startDetached(command);
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        return;
    }
}
