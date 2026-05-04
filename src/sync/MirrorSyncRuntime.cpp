/**
 * @file MirrorSyncRuntime.cpp
 * @brief Implementation of mirror-sync runtime facade
 */

#include "MirrorSyncRuntime.h"

#include <QMetaObject>
#include <QThread>

#include "FullSync.h"
#include "LocalChangeWatcher.h"
#include "MirrorSyncController.h"
#include "RemoteChangeWatcher.h"
#include "SyncActionQueue.h"
#include "SyncActionThread.h"
#include "SyncDatabase.h"
#include "api/GoogleDriveClient.h"

namespace {
void resetFullSyncCache(QString& phase, int& current, int& total, int& localCount, int& remoteCount,
                        QString& error) {
    phase.clear();
    current = 0;
    total = 0;
    localCount = 0;
    remoteCount = 0;
    error.clear();
}

int indexOfConflict(const QList<ConflictInfo>& conflicts, const QString& localPath) {
    for (int index = 0; index < conflicts.size(); ++index) {
        if (conflicts.at(index).localPath == localPath) {
            return index;
        }
    }

    return -1;
}

template <typename Func>
void invokeBlocking(QObject* target, Func&& func) {
    if (!target) {
        return;
    }

    if (QThread::currentThread() == target->thread()) {
        func();
        return;
    }

    QMetaObject::invokeMethod(target, std::forward<Func>(func), Qt::BlockingQueuedConnection);
}

void moveObjectToThread(QObject* object, QThread* targetThread) {
    if (!object || !targetThread || object->thread() == targetThread) {
        return;
    }

    if (QThread::currentThread() == object->thread()) {
        object->moveToThread(targetThread);
        return;
    }

    QMetaObject::invokeMethod(
        object, [object, targetThread]() { object->moveToThread(targetThread); },
        Qt::BlockingQueuedConnection);
}
}  // namespace

MirrorSyncRuntime::MirrorSyncRuntime(LocalChangeWatcher* localWatcher,
                                     RemoteChangeWatcher* remoteWatcher,
                                     ChangeProcessor* changeProcessor,
                                     SyncActionQueue* syncActionQueue,
                                     SyncActionThread* syncActionThread, FullSync* fullSync,
                                     GoogleDriveClient* mirrorDriveClient,
                                     SyncDatabase* syncDatabase, QObject* parent)
    : QObject(parent),
      m_localWatcher(localWatcher),
      m_remoteWatcher(remoteWatcher),
      m_changeProcessor(changeProcessor),
      m_syncActionQueue(syncActionQueue),
      m_syncActionThread(syncActionThread),
      m_fullSync(fullSync),
      m_mirrorDriveClient(mirrorDriveClient),
      m_syncDatabase(syncDatabase),
      m_controller(new MirrorSyncController(localWatcher, remoteWatcher, changeProcessor,
                                            syncActionThread, fullSync, this)),
      m_workerThread(new QThread(this)),
      m_ownerThread(thread()) {
    m_workerThread->setObjectName(QStringLiteral("MirrorSyncWorker"));
    m_pendingActionCount = m_syncActionQueue ? m_syncActionQueue->count() : 0;
    m_processorState =
        m_changeProcessor ? m_changeProcessor->state() : ChangeProcessor::State::Stopped;
    m_unresolvedConflicts =
        m_changeProcessor ? m_changeProcessor->unresolvedConflicts() : QList<ConflictInfo>{};
    m_fullSyncState = m_fullSync ? m_fullSync->state() : FullSync::State::Idle;

    if (m_syncActionQueue) {
        connect(m_syncActionQueue, &SyncActionQueue::countChanged, this, [this](int count) {
            m_pendingActionCount = count;
            emit pendingActionsChanged(count);
        });
        connect(m_syncActionQueue, &SyncActionQueue::itemEnqueued, this,
                &MirrorSyncRuntime::syncActionQueued);
    }

    if (m_changeProcessor) {
        connect(m_changeProcessor, &ChangeProcessor::stateChanged, this,
                [this](ChangeProcessor::State state) {
                    m_processorState = state;
                    emit processorStateChanged(state);
                });
        connect(m_changeProcessor, &ChangeProcessor::error, this,
                &MirrorSyncRuntime::processorError);
        connect(m_changeProcessor, &ChangeProcessor::conflictDetected, this,
                [this](const ConflictInfo& info) {
                    const int existingIndex =
                        indexOfConflict(m_unresolvedConflicts, info.localPath);
                    if (existingIndex >= 0) {
                        m_unresolvedConflicts[existingIndex] = info;
                    } else {
                        m_unresolvedConflicts.append(info);
                    }
                    emit conflictDetected(info);
                });
        connect(m_changeProcessor, &ChangeProcessor::conflictResolved, this,
                [this](const QString& localPath, ConflictResolutionStrategy strategy) {
                    const int existingIndex = indexOfConflict(m_unresolvedConflicts, localPath);
                    if (existingIndex >= 0) {
                        m_unresolvedConflicts.removeAt(existingIndex);
                    }
                    emit conflictResolved(localPath, strategy);
                });
        connect(m_changeProcessor, &ChangeProcessor::changeProcessed, this,
                &MirrorSyncRuntime::changeProcessed);
    }

    if (m_remoteWatcher) {
        connect(m_remoteWatcher, &RemoteChangeWatcher::changeTokenUpdated, this,
                &MirrorSyncRuntime::changeTokenUpdated);
    }

    if (m_syncActionThread) {
        connect(m_syncActionThread, &SyncActionThread::actionCompleted, this,
                &MirrorSyncRuntime::syncActionCompleted);
        connect(m_syncActionThread, &SyncActionThread::actionFailed, this,
                [this](const SyncActionItem& item, const QString& error) {
                    m_lastSyncActionError = error;
                    emit syncActionFailed(item, error);
                });
        connect(m_syncActionThread, &SyncActionThread::actionProgress, this,
                &MirrorSyncRuntime::syncActionProgress);
        connect(m_syncActionThread, &SyncActionThread::error, this, [this](const QString& error) {
            m_lastSyncActionError = error;
            emit syncActionError(error);
        });
        connect(m_syncActionThread, &SyncActionThread::tokenRefreshRequested, this,
                &MirrorSyncRuntime::tokenRefreshRequested);
    }

    if (m_fullSync) {
        connect(m_fullSync, &FullSync::stateChanged, this, [this](FullSync::State state) {
            m_fullSyncState = state;
            emit fullSyncStateChanged(state);
        });
        connect(m_fullSync, &FullSync::progressUpdated, this,
                [this](const QString& phase, int current, int total) {
                    m_fullSyncPhase = phase;
                    m_fullSyncProgressCurrent = current;
                    m_fullSyncProgressTotal = total;
                    emit fullSyncProgressUpdated(phase, current, total);
                });
        connect(m_fullSync, &FullSync::completed, this, [this](int localCount, int remoteCount) {
            m_fullSyncState = FullSync::State::Complete;
            m_lastFullSyncLocalCount = localCount;
            m_lastFullSyncRemoteCount = remoteCount;
            m_lastFullSyncError.clear();
            emit fullSyncCompleted(localCount, remoteCount);
        });
        connect(m_fullSync, &FullSync::error, this, [this](const QString& error) {
            m_fullSyncState = FullSync::State::Error;
            m_lastFullSyncError = error;
            emit fullSyncError(error);
        });
    }

    if (m_mirrorDriveClient) {
        connect(m_mirrorDriveClient, &GoogleDriveClient::authenticationFailure, this,
                &MirrorSyncRuntime::authenticationFailure);
    }

    moveMirrorGraphToWorker();
}

MirrorSyncRuntime::~MirrorSyncRuntime() {
    shutdown();
}

int MirrorSyncRuntime::pendingActionCount() const {
    return m_pendingActionCount;
}

ChangeProcessor::State MirrorSyncRuntime::processorState() const {
    return m_processorState;
}

QList<ConflictInfo> MirrorSyncRuntime::unresolvedConflicts() const {
    return m_unresolvedConflicts;
}

int MirrorSyncRuntime::unresolvedConflictCount() const {
    return m_unresolvedConflicts.size();
}

QString MirrorSyncRuntime::lastSyncActionError() const {
    return m_lastSyncActionError;
}

FullSync::State MirrorSyncRuntime::fullSyncState() const {
    return m_fullSyncState;
}

QString MirrorSyncRuntime::fullSyncPhase() const {
    return m_fullSyncPhase;
}

int MirrorSyncRuntime::fullSyncProgressCurrent() const {
    return m_fullSyncProgressCurrent;
}

int MirrorSyncRuntime::fullSyncProgressTotal() const {
    return m_fullSyncProgressTotal;
}

int MirrorSyncRuntime::lastFullSyncLocalCount() const {
    return m_lastFullSyncLocalCount;
}

int MirrorSyncRuntime::lastFullSyncRemoteCount() const {
    return m_lastFullSyncRemoteCount;
}

QString MirrorSyncRuntime::lastFullSyncError() const {
    return m_lastFullSyncError;
}

void MirrorSyncRuntime::shutdown() {
    if (m_shutdownComplete) {
        return;
    }

    if (m_workerThread && m_workerThread->isRunning()) {
        if (m_controller) {
            m_controller->cancelAndStop();
        }

        QObject* workerContext = nullptr;
        if (m_changeProcessor) {
            workerContext = m_changeProcessor;
        } else if (m_syncActionThread) {
            workerContext = m_syncActionThread;
        } else if (m_fullSync) {
            workerContext = m_fullSync;
        } else if (m_remoteWatcher) {
            workerContext = m_remoteWatcher;
        } else {
            workerContext = m_localWatcher;
        }

        if (m_syncDatabase && workerContext) {
            invokeBlocking(workerContext,
                           [this]() { m_syncDatabase->closeCurrentThreadConnection(); });
        }

        restoreMirrorGraphToOwnerThread();
        m_workerThread->quit();
        if (!m_workerThread->wait(5000)) {
            qWarning() << "Mirror worker thread did not stop within 5 seconds";
            m_workerThread->quit();
            m_workerThread->wait();
        }
    }

    m_shutdownComplete = true;
}

void MirrorSyncRuntime::setSyncFolder(const QString& syncFolder) {
    invokeBlocking(m_localWatcher,
                   [this, syncFolder]() { m_localWatcher->setSyncFolder(syncFolder); });
    invokeBlocking(m_syncActionThread,
                   [this, syncFolder]() { m_syncActionThread->setSyncFolder(syncFolder); });
    invokeBlocking(m_changeProcessor,
                   [this, syncFolder]() { m_changeProcessor->setSyncFolder(syncFolder); });
    invokeBlocking(m_fullSync, [this, syncFolder]() { m_fullSync->setSyncFolder(syncFolder); });
}

void MirrorSyncRuntime::setChangeToken(const QString& token) {
    invokeBlocking(m_remoteWatcher, [this, token]() { m_remoteWatcher->setChangeToken(token); });
}

void MirrorSyncRuntime::setConflictResolutionStrategy(ConflictResolutionStrategy strategy) {
    invokeBlocking(m_changeProcessor, [this, strategy]() {
        m_changeProcessor->setConflictResolutionStrategy(strategy);
    });
}

void MirrorSyncRuntime::setPeriodicLocalFullSyncInterval(int intervalMs) {
    if (m_controller) {
        m_controller->setPeriodicLocalFullSyncInterval(intervalMs);
    }
}

void MirrorSyncRuntime::resolveConflict(const QString& localPath,
                                        ConflictResolutionStrategy strategy) {
    invokeBlocking(m_changeProcessor, [this, localPath, strategy]() {
        m_changeProcessor->resolveConflict(localPath, strategy);
    });
}

void MirrorSyncRuntime::start() {
    if (m_controller) {
        m_controller->start();
    }
}

void MirrorSyncRuntime::stop() {
    if (m_controller) {
        m_controller->stop();
    }
}

void MirrorSyncRuntime::pause() {
    if (m_controller) {
        m_controller->pause();
    }
}

void MirrorSyncRuntime::resume() {
    if (m_controller) {
        m_controller->resume();
    }
}

void MirrorSyncRuntime::cancelAndStop() {
    if (m_controller) {
        m_controller->cancelAndStop();
    }
}

void MirrorSyncRuntime::startAndScheduleInitialSync(int delayMs) {
    if (m_controller) {
        m_controller->startAndScheduleInitialSync(delayMs);
    }
}

void MirrorSyncRuntime::restartAfterWake(int fullSyncDelayMs) {
    if (m_controller) {
        m_controller->restartAfterWake(fullSyncDelayMs);
    }
}

void MirrorSyncRuntime::requestFullSync(int delayMs) {
    if (m_controller) {
        m_controller->requestFullSync(delayMs);
    }
}

void MirrorSyncRuntime::clearSessionState() {
    if (m_controller) {
        m_controller->clearSessionState();
    }
    m_unresolvedConflicts.clear();
    m_lastSyncActionError.clear();
    m_fullSyncState = FullSync::State::Idle;
    resetFullSyncCache(m_fullSyncPhase, m_fullSyncProgressCurrent, m_fullSyncProgressTotal,
                       m_lastFullSyncLocalCount, m_lastFullSyncRemoteCount, m_lastFullSyncError);
}

void MirrorSyncRuntime::moveMirrorGraphToWorker() {
    if (!m_workerThread) {
        return;
    }

    moveObjectToThread(m_mirrorDriveClient, m_workerThread);
    moveObjectToThread(m_localWatcher, m_workerThread);
    moveObjectToThread(m_remoteWatcher, m_workerThread);
    moveObjectToThread(m_changeProcessor, m_workerThread);
    moveObjectToThread(m_syncActionThread, m_workerThread);
    moveObjectToThread(m_fullSync, m_workerThread);
    m_workerThread->start();
}

void MirrorSyncRuntime::restoreMirrorGraphToOwnerThread() {
    if (!m_ownerThread) {
        return;
    }

    moveObjectToThread(m_fullSync, m_ownerThread);
    moveObjectToThread(m_syncActionThread, m_ownerThread);
    moveObjectToThread(m_changeProcessor, m_ownerThread);
    moveObjectToThread(m_remoteWatcher, m_ownerThread);
    moveObjectToThread(m_localWatcher, m_ownerThread);
    moveObjectToThread(m_mirrorDriveClient, m_ownerThread);
}