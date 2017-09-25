/*
  recordpage.cpp

  This file is part of Hotspot, the Qt GUI for performance analysis.

  Copyright (C) 2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
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

#include "recordpage.h"
#include "ui_recordpage.h"

#include "processfiltermodel.h"
#include "processmodel.h"

#include <QDebug>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QListView>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <KUrlRequester>
#include <Solid/Device>
#include <Solid/Processor>
#include <KShell>
#include <KFilterProxySearchLine>
#include <KRecursiveFilterProxyModel>
#include <KColumnResizer>
#include <KComboBox>
#include <KUrlCompletion>
#include <KSharedConfig>
#include <KConfigGroup>

#include "perfrecord.h"

namespace {
bool isIntel()
{
    const auto list = Solid::Device::listFromType(Solid::DeviceInterface::Processor, QString());
    Solid::Device device = list[0];
    if(list.empty() || !device.is<Solid::Processor>()) {
        return false;
    }
    const auto *processor= device.as<Solid::Processor>();
    const auto instructionSets = processor->instructionSets();

    return instructionSets.testFlag(Solid::Processor::IntelMmx) ||
           instructionSets.testFlag(Solid::Processor::IntelSse) ||
           instructionSets.testFlag(Solid::Processor::IntelSse2) ||
           instructionSets.testFlag(Solid::Processor::IntelSse3) ||
           instructionSets.testFlag(Solid::Processor::IntelSsse3) ||
           instructionSets.testFlag(Solid::Processor::IntelSse4) ||
           instructionSets.testFlag(Solid::Processor::IntelSse41) ||
           instructionSets.testFlag(Solid::Processor::IntelSse42);
}

void updateStartRecordingButtonState(const QScopedPointer<Ui::RecordPage> &ui)
{
    bool enabled = false;
    if (ui->stackedWidget->currentWidget() == ui->launchAppPage) {
        enabled = ui->applicationName->url().isValid();
    } else {
        enabled = ui->processesTableView->selectionModel()->hasSelection();
    }
    enabled &= ui->applicationRecordErrorMessage->text().isEmpty();
    ui->startRecordingButton->setEnabled(enabled);
}

void updateStackedSizePolicy(const QScopedPointer<Ui::RecordPage> &ui)
{
    // make the stacked widget size to the current page only
    const auto currentIndex = ui->stackedWidget->currentIndex();
    for (int i = 0, c = ui->stackedWidget->count (); i < c; ++i) {
        // determine the vertical size policy
        QSizePolicy::Policy policy = QSizePolicy::Ignored;
        if (i == currentIndex) {
            policy = QSizePolicy::Expanding;
        }

        // update the size policy
        QWidget* pPage = ui->stackedWidget->widget(i);
        pPage->setSizePolicy(policy, policy);
    }
}

KConfigGroup config()
{
    return KSharedConfig::openConfig()->group("RecordPage");
}

KConfigGroup applicationConfig(const QString& application)
{
    return config().group(QLatin1String("Application ") + application);
}

constexpr const int MAX_COMBO_ENTRIES = 10;

void rememberCombobox(KConfigGroup config, const QString& entryName,
                      const QString& value, QComboBox* combo)
{
    // remove the value if it exists already
    auto idx = combo->findText(value);
    if (idx != -1) {
        combo->removeItem(idx);
    }

    // insert value on front
    combo->insertItem(0, value);
    combo->setCurrentIndex(0);

    // store the values in the config
    QStringList values;
    values.reserve(combo->count());
    for (int i = 0, c = std::min(MAX_COMBO_ENTRIES, combo->count()); i < c; ++i) {
        values << combo->itemText(i);
    }
    config.writeEntry(entryName, values);
}

void restoreCombobox(const KConfigGroup& config, const QString& entryName,
                     QComboBox* combo, const QStringList& defaults = {})
{
    combo->clear();
    const auto& values = config.readEntry(entryName, defaults);
    for (const auto& application : values) {
        combo->addItem(application);
    }
}

void rememberApplication(const QString& application, const QString& appParameters,
                         const QString& workingDir, KComboBox* combo)
{
    // set the app config early, so when we change the combo box below we can
    // restore the options as expected
    auto options = applicationConfig(application);
    options.writeEntry("params", appParameters);
    options.writeEntry("workingDir", workingDir);

    rememberCombobox(config(), QStringLiteral("applications"),
                     application, combo);
}

}

RecordPage::RecordPage(QWidget *parent)
    : QWidget(parent),
      ui(new Ui::RecordPage),
      m_perfRecord(new PerfRecord(this)),
      m_watcher(new QFutureWatcher<ProcDataList>(this))
{
    ui->setupUi(this);

    auto completion = ui->applicationName->completionObject();
    ui->applicationName->comboBox()->setEditable(true);
    // NOTE: workaround until https://phabricator.kde.org/D7966 has landed and we bump the required version
    ui->applicationName->comboBox()->setCompletionObject(completion);
    ui->applicationName->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    // we are only interested in executable files, so set the mime type filter accordingly
    // note that exe's build with PIE are actually "shared libs"...
    ui->applicationName->setMimeTypeFilters({QStringLiteral("application/x-executable"),
                                             QStringLiteral("application/x-sharedlib")});
    ui->workingDirectory->setMode(KFile::Directory | KFile::LocalOnly);
    ui->applicationRecordErrorMessage->setCloseButtonVisible(false);
    ui->applicationRecordErrorMessage->setWordWrap(true);
    ui->applicationRecordErrorMessage->setMessageType(KMessageWidget::Error);
    ui->outputFile->setText(QDir::currentPath() + QDir::separator() + QStringLiteral("perf.data"));
    ui->outputFile->setMode(KFile::File | KFile::LocalOnly);
    ui->eventTypeBox->lineEdit()->setPlaceholderText(tr("perf defaults (usually cycles:Pu)"));

    auto columnResizer = new KColumnResizer(this);
    columnResizer->addWidgetsFromLayout(ui->formLayout);
    columnResizer->addWidgetsFromLayout(ui->formLayout_2);
    columnResizer->addWidgetsFromLayout(ui->formLayout_3);

    connect(ui->homeButton, &QPushButton::clicked, this, &RecordPage::homeButtonClicked);
    connect(ui->applicationName, &KUrlRequester::textChanged, this, &RecordPage::onApplicationNameChanged);
    // NOTE: workaround until https://phabricator.kde.org/D7968 has landed and we bump the required version
    connect(ui->applicationName->comboBox()->lineEdit(), &QLineEdit::textChanged,
            this, &RecordPage::onApplicationNameChanged);
    connect(ui->startRecordingButton, &QPushButton::toggled, this, &RecordPage::onStartRecordingButtonClicked);
    connect(ui->workingDirectory, &KUrlRequester::textChanged, this, &RecordPage::onWorkingDirectoryNameChanged);
    connect(ui->viewPerfRecordResultsButton, &QPushButton::clicked, this, &RecordPage::onViewPerfRecordResultsButtonClicked);
    connect(ui->outputFile, &KUrlRequester::textChanged, this, &RecordPage::onOutputFileNameChanged);
    connect(ui->outputFile, static_cast<void (KUrlRequester::*)(const QString&)>(&KUrlRequester::returnPressed),
            this, &RecordPage::onOutputFileNameSelected);
    connect(ui->outputFile, &KUrlRequester::urlSelected, this, &RecordPage::onOutputFileUrlChanged);

    ui->recordTypeComboBox->addItem(QIcon::fromTheme(QStringLiteral("run-build")),
                                    tr("Launch Application"), QVariant::fromValue(LaunchApplication));
    ui->recordTypeComboBox->addItem(QIcon::fromTheme(QStringLiteral("run-install")),
                                    tr("Attach To Process(es)"), QVariant::fromValue(AttachToProcess));
    connect(ui->recordTypeComboBox, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
        [this](int index){
        switch (index) {
        case LaunchApplication:
            ui->stackedWidget->setCurrentWidget(ui->launchAppPage);
            break;
        case AttachToProcess:
            ui->stackedWidget->setCurrentWidget(ui->attachAppPage);

            updateProcesses();
            break;
        default:
            break;
        }
        updateStartRecordingButtonState(ui);
        updateStackedSizePolicy(ui);
    });
    updateStartRecordingButtonState(ui);
    updateStackedSizePolicy(ui);

    {
        ui->callGraphComboBox->addItem(tr("None"), QVariant::fromValue(QString()));
        ui->callGraphComboBox->setItemData(ui->callGraphComboBox->count() - 1,
                                           tr("<qt>Do not unwind the call stack. This results in tiny data files. "
                                              " But the data can be hard to make use of, when hotspots lie "
                                              " in third party or system libraries not under your direct control.</qt>"),
                                           Qt::ToolTipRole);

        const auto dwarfIdx = ui->callGraphComboBox->count();
        ui->callGraphComboBox->addItem(tr("DWARF"), QVariant::fromValue(QStringLiteral("dwarf")));
        ui->callGraphComboBox->setItemData(dwarfIdx,
                                           tr("<qt>Use the DWARF unwinder, which requires debug information to be available."
                                              " This can result in large data files, but is usually the most portable option to use.</qt>"),
                                           Qt::ToolTipRole);

        ui->callGraphComboBox->addItem(tr("Frame Pointer"), QVariant::fromValue(QStringLiteral("fp")));
        ui->callGraphComboBox->setItemData(ui->callGraphComboBox->count() - 1,
                                           tr("<qt>Use the frame pointer for stack unwinding. This only works when your code was compiled"
                                              " with <tt>-fno-omit-framepointer</tt>, which is usually not the case nowadays."
                                              " As such, only use this option when you know that you have frame pointers available."
                                              " If frame pointers are available, this option is the recommended unwinding option,"
                                              " as it results in smaller data files and has less overhead while recording.</qt>"),
                                           Qt::ToolTipRole);

        if (isIntel()) {
            ui->callGraphComboBox->addItem(tr("Last Branch Record"), QVariant::fromValue(QStringLiteral("lbr")));
            ui->callGraphComboBox->setItemData(ui->callGraphComboBox->count() - 1,
                                               tr("<qt>Use the Last Branch Record (LBR) for stack unwinding. This only works on newer Intel CPUs"
                                                  " but does not require any special compile options. The depth of the LBR is relatively limited,"
                                                  " which makes this option not too useful for many real-world applications.</qt>"),
                                                Qt::ToolTipRole);
        }

        ui->callGraphComboBox->setCurrentIndex(dwarfIdx);
    }

    connect(m_perfRecord, &PerfRecord::recordingFinished,
            this, [this] (const QString& fileLocation) {
                ui->startRecordingButton->setChecked(false);
                ui->applicationRecordErrorMessage->hide();
                m_resultsFile = fileLocation;
                ui->viewPerfRecordResultsButton->setEnabled(true);
                ui->recordTypeComboBox->setEnabled(true);
    });

    connect(m_perfRecord, &PerfRecord::recordingFailed,
            this, [this] (const QString& errorMessage) {
                ui->startRecordingButton->setChecked(false);
                ui->applicationRecordErrorMessage->setText(errorMessage);
                ui->applicationRecordErrorMessage->show();
                ui->viewPerfRecordResultsButton->setEnabled(false);
                ui->recordTypeComboBox->setEnabled(true);
    });

    connect(m_perfRecord, &PerfRecord::recordingOutput,
            this, [this] (const QString& outputMessage) {
                ui->perfResultsTextEdit->insertPlainText(outputMessage);
                ui->perfResultsTextEdit->show();
                ui->perfResultsLabel->show();
                ui->perfResultsTextEdit->moveCursor(QTextCursor::End);
    });

    m_processModel = new ProcessModel(this);
    m_processProxyModel = new ProcessFilterModel(this);
    m_processProxyModel->setSourceModel(m_processModel);
    m_processProxyModel->setDynamicSortFilter(true);

    ui->processesTableView->setModel(m_processProxyModel);
    // hide state
    ui->processesTableView->hideColumn(ProcessModel::StateColumn);
    ui->processesTableView->sortByColumn(ProcessModel::NameColumn, Qt::AscendingOrder);
    ui->processesTableView->setSortingEnabled(true);
    ui->processesTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->processesTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->processesTableView->setSelectionMode(QAbstractItemView::MultiSelection);
    connect(ui->processesTableView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](){
                updateStartRecordingButtonState(ui);
            });

    ui->processesFilterBox->setProxy(m_processProxyModel);

    connect(m_watcher, &QFutureWatcher<ProcDataList>::finished,
            this, &RecordPage::updateProcessesFinished);

    showRecordPage();

    restoreCombobox(config(), QStringLiteral("applications"),
                    ui->applicationName->comboBox());
    restoreCombobox(config(), QStringLiteral("eventType"),
                    ui->eventTypeBox, {ui->eventTypeBox->currentText()});
    const auto callGraph = config().readEntry("callGraph", ui->callGraphComboBox->currentData());
    const auto callGraphIdx = ui->callGraphComboBox->findData(callGraph);
    if (callGraphIdx != -1) {
        ui->callGraphComboBox->setCurrentIndex(callGraphIdx);
    }
}

RecordPage::~RecordPage() = default;

void RecordPage::showRecordPage()
{
    setWindowTitle(tr("Hotspot - Record"));
    m_resultsFile.clear();
    ui->applicationRecordErrorMessage->hide();
    ui->perfResultsTextEdit->clear();
    ui->perfResultsTextEdit->hide();
    ui->perfResultsLabel->hide();
    ui->viewPerfRecordResultsButton->setEnabled(false);
}

void RecordPage::onStartRecordingButtonClicked(bool checked)
{
    if (checked) {
        showRecordPage();
        ui->startRecordingButton->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-stop")));
        ui->startRecordingButton->setText(tr("Stop Recording"));

        QStringList perfOptions;

        const auto callGraphOption = ui->callGraphComboBox->currentData().toString();
        config().writeEntry("callGraph", callGraphOption);
        if (!callGraphOption.isEmpty()) {
            perfOptions << QStringLiteral("--call-graph") << callGraphOption;
        }

        const auto eventType = ui->eventTypeBox->currentText();
        rememberCombobox(config(), QStringLiteral("eventType"),
                         eventType, ui->eventTypeBox);
        if (!eventType.isEmpty()) {
            perfOptions << QStringLiteral("--event") << eventType;
        }

        if (ui->recordTypeComboBox->currentData() == LaunchApplication) {
            const auto applicationName = KShell::tildeExpand(ui->applicationName->text());
            const auto appParameters = ui->applicationParametersBox->text();
            auto workingDir = ui->workingDirectory->text();
            if (workingDir.isEmpty()) {
                workingDir = ui->workingDirectory->placeholderText();
            }
            rememberApplication(applicationName, appParameters, workingDir,
                                ui->applicationName->comboBox());
            m_perfRecord->record(perfOptions, ui->outputFile->url().toLocalFile(),
                                 applicationName, KShell::splitArgs(appParameters),
                                 workingDir);
        } else {
            QItemSelectionModel *selectionModel = ui->processesTableView->selectionModel();
            QStringList pids;

            for (const auto& item : selectionModel->selectedIndexes()) {
                if (item.column() == 0) {
                    pids.append(item.data(ProcessModel::PIDRole).toString());
                }
            }

            m_perfRecord->record(perfOptions, ui->outputFile->url().toLocalFile(), pids);
        }
        ui->recordTypeComboBox->setEnabled(false);
    } else {
        ui->startRecordingButton->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-start")));
        ui->startRecordingButton->setText(tr("Start Recording"));
        m_perfRecord->stopRecording();
        ui->recordTypeComboBox->setEnabled(true);
    }
}

void RecordPage::onApplicationNameChanged(const QString& filePath)
{
    QFileInfo application(QStandardPaths::findExecutable(KShell::tildeExpand(filePath)));

    if (!application.exists()) {
        ui->applicationRecordErrorMessage->setText(tr("Application file cannot be found: %1").arg(filePath));
    } else if (!application.isFile()) {
        ui->applicationRecordErrorMessage->setText(tr("Application file is not valid: %1").arg(filePath));
    } else if (!application.isExecutable()) {
        ui->applicationRecordErrorMessage->setText(tr("Application file is not executable: %1").arg(filePath));
    } else {
        const auto config = applicationConfig(filePath);
        ui->workingDirectory->setText(config.readEntry("workingDir", QString()));
        ui->applicationParametersBox->setText(config.readEntry("params", QString()));
        ui->workingDirectory->setPlaceholderText(application.path());
        ui->applicationRecordErrorMessage->setText({});
    }
    ui->applicationRecordErrorMessage->setVisible(!ui->applicationRecordErrorMessage->text().isEmpty());
    updateStartRecordingButtonState(ui);
}

void RecordPage::onWorkingDirectoryNameChanged(const QString& folderPath)
{
    QFileInfo folder(ui->workingDirectory->url().toLocalFile());

    if (!folder.exists()) {
        ui->applicationRecordErrorMessage->setText(tr("Working directory folder cannot be found: %1").arg(folderPath));
    } else if (!folder.isDir()) {
        ui->applicationRecordErrorMessage->setText(tr("Working directory folder is not valid: %1").arg(folderPath));
    } else if (!folder.isWritable()) {
        ui->applicationRecordErrorMessage->setText(tr("Working directory folder is not writable: %1").arg(folderPath));
    } else {
        ui->applicationRecordErrorMessage->setText({});
    }
    ui->applicationRecordErrorMessage->setVisible(!ui->applicationRecordErrorMessage->text().isEmpty());
    updateStartRecordingButtonState(ui);
}

void RecordPage::onViewPerfRecordResultsButtonClicked()
{
    emit openFile(m_resultsFile);
}

void RecordPage::onOutputFileNameChanged(const QString& /*filePath*/)
{
    const auto perfDataExtension = QStringLiteral(".data");

    QFileInfo file(ui->outputFile->url().toLocalFile());
    QFileInfo folder(file.absolutePath());

    if (!folder.exists()) {
        ui->applicationRecordErrorMessage->setText(tr("Output file directory folder cannot be found: %1").arg(folder.path()));
    } else if (!folder.isDir()) {
        ui->applicationRecordErrorMessage->setText(tr("Output file directory folder is not valid: %1").arg(folder.path()));
    } else if (!folder.isWritable()) {
        ui->applicationRecordErrorMessage->setText(tr("Output file directory folder is not writable: %1").arg(folder.path()));
    } else if (!file.absoluteFilePath().endsWith(perfDataExtension)) {
        ui->applicationRecordErrorMessage->setText(tr("Output file must end with %1").arg(perfDataExtension));
    } else {
        ui->applicationRecordErrorMessage->setText({});
    }
    ui->applicationRecordErrorMessage->setVisible(!ui->applicationRecordErrorMessage->text().isEmpty());
    updateStartRecordingButtonState(ui);
}

void RecordPage::onOutputFileNameSelected(const QString& filePath)
{
    const auto perfDataExtension = QStringLiteral(".data");

    if (!filePath.endsWith(perfDataExtension)) {
        ui->outputFile->setText(filePath + perfDataExtension);
    }
}

void RecordPage::onOutputFileUrlChanged(const QUrl& fileUrl)
{
    onOutputFileNameSelected(fileUrl.toLocalFile());
}

void RecordPage::updateProcesses()
{
    m_watcher->setFuture(QtConcurrent::run(processList, m_processModel->processes()));
}

void RecordPage::updateProcessesFinished()
{
    m_processModel->mergeProcesses(m_watcher->result());

    if (ui->stackedWidget->currentWidget() == ui->attachAppPage) {
        // only update the state when we show the attach app page
        updateStartRecordingButtonState(ui);
        QTimer::singleShot(1000, this, SLOT(updateProcesses()));
    }
}