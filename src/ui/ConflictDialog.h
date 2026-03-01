/**
 * @file ConflictDialog.h
 * @brief Dialog for managing sync conflicts
 *
 * Provides UI for viewing and resolving file synchronization conflicts.
 */

#ifndef CONFLICTDIALOG_H
#define CONFLICTDIALOG_H

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

/**
 * @struct ConflictInfo
 * @brief Information about a file conflict
 */
struct ConflictInfo {
    QString fileName;          ///< Original file name
    QString localPath;         ///< Local file path
    QString remotePath;        ///< Remote file path
    QDateTime localModified;   ///< Local modification time
    QDateTime remoteModified;  ///< Remote modification time
    qint64 localSize;          ///< Local file size in bytes
    qint64 remoteSize;         ///< Remote file size in bytes
    QString conflictCopyPath;  ///< Path to conflict copy
};

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
     * @brief Resolution strategy for conflicts
     */
    enum ResolutionStrategy {
        KeepLocal,   ///< Keep local version, overwrite remote
        KeepRemote,  ///< Keep remote version, overwrite local
        KeepBoth,    ///< Keep both versions (create copy)
        Manual       ///< Manual resolution by user
    };
    Q_ENUM(ResolutionStrategy)

    /**
     * @brief Construct the conflict dialog
     * @param parent Parent widget
     */
    explicit ConflictDialog(QWidget* parent = nullptr);

    ~ConflictDialog() override;

    /**
     * @brief Add a conflict to the list
     * @param conflict Conflict information
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
     * @param fileName Name of the resolved file
     * @param strategy Resolution strategy used
     */
    void conflictResolved(const QString& fileName, ResolutionStrategy strategy);

    /**
     * @brief Emitted when all conflicts are resolved
     */
    void allConflictsResolved();

   private slots:
    void onResolveSelectedClicked();
    void onResolveAllClicked();
    void onSelectionChanged();
    void onOpenLocalClicked();
    void onOpenRemoteClicked();

   private:
    void setupUi();
    void updateButtonStates();
    QString formatFileSize(qint64 bytes) const;

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
    QPushButton* m_openRemoteButton;

    // Resolution group
    QGroupBox* m_resolutionGroup;
    QComboBox* m_resolutionCombo;
    QPushButton* m_resolveSelectedButton;
    QPushButton* m_resolveAllButton;

    // Dialog buttons
    QPushButton* m_closeButton;
};

#endif  // CONFLICTDIALOG_H
