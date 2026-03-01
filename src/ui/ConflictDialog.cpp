/**
 * @file ConflictDialog.cpp
 * @brief Implementation of the conflict dialog
 */

#include "ConflictDialog.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QUrl>

ConflictDialog::ConflictDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Sync Conflicts");
    setMinimumSize(700, 500);
    resize(800, 600);
    setModal(false);

    setupUi();
}

ConflictDialog::~ConflictDialog() = default;

void ConflictDialog::setupUi() {
    m_mainLayout = new QVBoxLayout(this);

    // Description
    m_descriptionLabel = new QLabel(
        "The following files have conflicts between local and remote versions.\n"
        "Choose how to resolve each conflict.",
        this);
    m_mainLayout->addWidget(m_descriptionLabel);

    // Conflict table
    m_conflictTable = new QTableWidget(this);
    m_conflictTable->setColumnCount(4);
    m_conflictTable->setHorizontalHeaderLabels(
        {"File Name", "Local Modified", "Remote Modified", "Status"});
    m_conflictTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_conflictTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_conflictTable->horizontalHeader()->setStretchLastSection(true);
    m_conflictTable->verticalHeader()->setVisible(false);
    m_conflictTable->setMinimumHeight(200);

    connect(m_conflictTable, &QTableWidget::itemSelectionChanged, this,
            &ConflictDialog::onSelectionChanged);

    m_mainLayout->addWidget(m_conflictTable);

    // Details group
    m_detailsGroup = new QGroupBox("Conflict Details", this);
    QVBoxLayout* detailsLayout = new QVBoxLayout(m_detailsGroup);

    QHBoxLayout* localLayout = new QHBoxLayout();
    m_localInfoLabel = new QLabel("Local: Select a conflict to view details", this);
    m_openLocalButton = new QPushButton("Open Local", this);
    m_openLocalButton->setEnabled(false);
    localLayout->addWidget(m_localInfoLabel, 1);
    localLayout->addWidget(m_openLocalButton);
    detailsLayout->addLayout(localLayout);

    QHBoxLayout* remoteLayout = new QHBoxLayout();
    m_remoteInfoLabel = new QLabel("Remote: Select a conflict to view details", this);
    m_openRemoteButton = new QPushButton("Open Remote", this);
    m_openRemoteButton->setEnabled(false);
    remoteLayout->addWidget(m_remoteInfoLabel, 1);
    remoteLayout->addWidget(m_openRemoteButton);
    detailsLayout->addLayout(remoteLayout);

    m_mainLayout->addWidget(m_detailsGroup);

    connect(m_openLocalButton, &QPushButton::clicked, this, &ConflictDialog::onOpenLocalClicked);
    connect(m_openRemoteButton, &QPushButton::clicked, this, &ConflictDialog::onOpenRemoteClicked);

    // Resolution group
    m_resolutionGroup = new QGroupBox("Resolution", this);
    QHBoxLayout* resolutionLayout = new QHBoxLayout(m_resolutionGroup);

    resolutionLayout->addWidget(new QLabel("Strategy:", this));

    m_resolutionCombo = new QComboBox(this);
    m_resolutionCombo->addItem("Keep local version", KeepLocal);
    m_resolutionCombo->addItem("Keep remote version", KeepRemote);
    m_resolutionCombo->addItem("Keep both versions (create copy)", KeepBoth);
    resolutionLayout->addWidget(m_resolutionCombo);

    resolutionLayout->addStretch();

    m_resolveSelectedButton = new QPushButton("Resolve Selected", this);
    m_resolveSelectedButton->setEnabled(false);
    resolutionLayout->addWidget(m_resolveSelectedButton);

    m_resolveAllButton = new QPushButton("Resolve All", this);
    m_resolveAllButton->setEnabled(false);
    resolutionLayout->addWidget(m_resolveAllButton);

    m_mainLayout->addWidget(m_resolutionGroup);

    connect(m_resolveSelectedButton, &QPushButton::clicked, this,
            &ConflictDialog::onResolveSelectedClicked);
    connect(m_resolveAllButton, &QPushButton::clicked, this, &ConflictDialog::onResolveAllClicked);

    // Close button
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    m_closeButton = new QPushButton("Close", this);
    buttonLayout->addWidget(m_closeButton);
    m_mainLayout->addLayout(buttonLayout);

    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void ConflictDialog::addConflict(const ConflictInfo& conflict) {
    m_conflicts.append(conflict);

    int row = m_conflictTable->rowCount();
    m_conflictTable->insertRow(row);

    QTableWidgetItem* nameItem = new QTableWidgetItem(conflict.fileName);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    m_conflictTable->setItem(row, 0, nameItem);

    QTableWidgetItem* localItem =
        new QTableWidgetItem(conflict.localModified.toString("yyyy-MM-dd hh:mm:ss"));
    localItem->setFlags(localItem->flags() & ~Qt::ItemIsEditable);
    m_conflictTable->setItem(row, 1, localItem);

    QTableWidgetItem* remoteItem =
        new QTableWidgetItem(conflict.remoteModified.toString("yyyy-MM-dd hh:mm:ss"));
    remoteItem->setFlags(remoteItem->flags() & ~Qt::ItemIsEditable);
    m_conflictTable->setItem(row, 2, remoteItem);

    QTableWidgetItem* statusItem = new QTableWidgetItem("Unresolved");
    statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
    statusItem->setForeground(Qt::red);
    m_conflictTable->setItem(row, 3, statusItem);

    updateButtonStates();
}

void ConflictDialog::clearConflicts() {
    m_conflicts.clear();
    m_conflictTable->setRowCount(0);
    updateButtonStates();
}

int ConflictDialog::conflictCount() const { return m_conflicts.count(); }

void ConflictDialog::onResolveSelectedClicked() {
    int row = m_conflictTable->currentRow();
    if (row < 0 || row >= m_conflicts.count()) {
        return;
    }

    ResolutionStrategy strategy =
        static_cast<ResolutionStrategy>(m_resolutionCombo->currentData().toInt());

    ConflictInfo& conflict = m_conflicts[row];

    emit conflictResolved(conflict.fileName, strategy);

    // Update table
    QTableWidgetItem* statusItem = m_conflictTable->item(row, 3);
    statusItem->setText("Resolved");
    statusItem->setForeground(Qt::darkGreen);

    // Remove from list
    m_conflicts.removeAt(row);
    m_conflictTable->removeRow(row);

    updateButtonStates();

    if (m_conflicts.isEmpty()) {
        emit allConflictsResolved();
    }
}

void ConflictDialog::onResolveAllClicked() {
    if (m_conflicts.isEmpty()) {
        return;
    }

    QMessageBox::StandardButton reply =
        QMessageBox::question(this, "Resolve All Conflicts",
                              QString("Are you sure you want to resolve all %1 conflicts "
                                      "using the selected strategy?")
                                  .arg(m_conflicts.count()),
                              QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    ResolutionStrategy strategy =
        static_cast<ResolutionStrategy>(m_resolutionCombo->currentData().toInt());

    while (!m_conflicts.isEmpty()) {
        const ConflictInfo& conflict = m_conflicts.first();
        emit conflictResolved(conflict.fileName, strategy);
        m_conflicts.removeFirst();
    }

    m_conflictTable->setRowCount(0);
    updateButtonStates();

    emit allConflictsResolved();

    QMessageBox::information(this, "Conflicts Resolved", "All conflicts have been resolved.");
}

void ConflictDialog::onSelectionChanged() {
    int row = m_conflictTable->currentRow();

    if (row >= 0 && row < m_conflicts.count()) {
        const ConflictInfo& conflict = m_conflicts[row];

        m_localInfoLabel->setText(QString("Local: %1 (%2)")
                                      .arg(conflict.localPath)
                                      .arg(formatFileSize(conflict.localSize)));

        m_remoteInfoLabel->setText(QString("Remote: %1 (%2)")
                                       .arg(conflict.remotePath)
                                       .arg(formatFileSize(conflict.remoteSize)));

        m_openLocalButton->setEnabled(true);
        m_openRemoteButton->setEnabled(true);
        m_resolveSelectedButton->setEnabled(true);
    } else {
        m_localInfoLabel->setText("Local: Select a conflict to view details");
        m_remoteInfoLabel->setText("Remote: Select a conflict to view details");
        m_openLocalButton->setEnabled(false);
        m_openRemoteButton->setEnabled(false);
        m_resolveSelectedButton->setEnabled(false);
    }
}

void ConflictDialog::onOpenLocalClicked() {
    int row = m_conflictTable->currentRow();
    if (row >= 0 && row < m_conflicts.count()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_conflicts[row].localPath));
    }
}

void ConflictDialog::onOpenRemoteClicked() {
    int row = m_conflictTable->currentRow();
    if (row >= 0 && row < m_conflicts.count()) {
        // For remote, we'd typically open in browser
        // This is a placeholder - actual implementation would need the web URL
        QMessageBox::information(this, "Open Remote",
                                 "Opening remote file in Google Drive web interface...\n"
                                 "Path: " +
                                     m_conflicts[row].remotePath);
    }
}

void ConflictDialog::updateButtonStates() {
    bool hasConflicts = !m_conflicts.isEmpty();
    m_resolveAllButton->setEnabled(hasConflicts);

    onSelectionChanged();  // Update selection-dependent buttons
}

QString ConflictDialog::formatFileSize(qint64 bytes) const {
    const qint64 KB = 1024;
    const qint64 MB = 1024 * KB;
    const qint64 GB = 1024 * MB;

    if (bytes >= GB) {
        return QString("%1 GB").arg(bytes / static_cast<double>(GB), 0, 'f', 2);
    } else if (bytes >= MB) {
        return QString("%1 MB").arg(bytes / static_cast<double>(MB), 0, 'f', 2);
    } else if (bytes >= KB) {
        return QString("%1 KB").arg(bytes / static_cast<double>(KB), 0, 'f', 2);
    } else {
        return QString("%1 bytes").arg(bytes);
    }
}
