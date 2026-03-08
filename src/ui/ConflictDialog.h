/**
 * @file ConflictDialog.h
 * @brief Dialog for managing sync conflicts
 *
 * Provides UI for viewing and resolving file synchronization conflicts.
 * Uses the canonical ConflictInfo struct and ConflictResolutionStrategy enum
 * from ChangeProcessor.h.
 */

#ifndef CONFLICTDIALOG_H
#define CONFLICTDIALOG_H

#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "sync/ChangeProcessor.h"

/**
 * @class ConflictDialog
 * @brief Dialog for viewing and resolving sync conflicts
 *
 * Allows users to:
 * - View list of conflicting files
 * - Choose resolution strategy (keep local, keep remote, keep both)
 * - Apply resolution to selected or all conflicts
 */
class ConflictDialog : public QDialog {
    Q_OBJECT

   public:
    /**
     * @brief Construct the conflict dialog
     * @param parent Parent widget
     */
    explicit ConflictDialog(QWidget* parent = nullptr);

    ~ConflictDialog() override;

    /**
     * @brief Add a conflict to the list
     * @param conflict Conflict information (from ChangeProcessor)
     */
    void addConflict(const ConflictInfo& conflict);

    /**
     * @brief Clear all conflicts from the list
     */
    void clearConflicts();

    /**
     * @brief Get the number of unresolved conflicts
     * @return Number of conflicts
     */
    int conflictCount() const;

   signals:
    /**
     * @brief Emitted when a conflict is resolved
     * @param localPath Path of the resolved file
     * @param strategy Resolution strategy used
     */
    void conflictResolved(const QString& localPath, ConflictResolutionStrategy strategy);

    /**
     * @brief Emitted when all conflicts are resolved
     */
    void allConflictsResolved();

   private slots:
    void onResolveSelectedClicked();
    void onResolveAllClicked();
    void onSelectionChanged();
    void onOpenLocalClicked();

   private:
    void setupUi();
    void updateButtonStates();

    QList<ConflictInfo> m_conflicts;

    // UI elements
    QVBoxLayout* m_mainLayout;
    QLabel* m_descriptionLabel;
    QTableWidget* m_conflictTable;

    // Details group
    QGroupBox* m_detailsGroup;
    QLabel* m_localInfoLabel;
    QLabel* m_remoteInfoLabel;
    QPushButton* m_openLocalButton;

    // Resolution group
    QGroupBox* m_resolutionGroup;
    QComboBox* m_resolutionCombo;
    QPushButton* m_resolveSelectedButton;
    QPushButton* m_resolveAllButton;

    // Dialog buttons
    QPushButton* m_closeButton;
};

#endif  // CONFLICTDIALOG_H
