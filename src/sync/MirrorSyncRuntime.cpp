/**
 * @file MirrorSyncRuntime.cpp
 * @brief Implementation of mirror-sync runtime facade
 */

#include "MirrorSyncRuntime.h"

#include "FullSync.h"
#include "LocalChangeWatcher.h"
#include "MirrorSyncController.h"
#include "RemoteChangeWatcher.h"
#include "SyncActionQueue.h"
#include "SyncActionThread.h"

MirrorSyncRuntime::MirrorSyncRuntime(LocalChangeWatcher* localWatcher,
                                     RemoteChangeWatcher* remoteWatcher,
                                     ChangeProcessor* changeProcessor,
                                     SyncActionQueue* syncActionQueue,
                                     SyncActionThread* syncActionThread, FullSync* fullSync,
                                     QObject* parent)
    : QObject(parent),
      m_localWatcher(localWatcher),
      m_remoteWatcher(remoteWatcher),
      m_changeProcessor(changeProcessor),
      m_syncActionQueue(syncActionQueue),
      m_syncActionThread(syncActionThread),
      m_fullSync(fullSync),
      m_controller(new MirrorSyncController(localWatcher, remoteWatcher, changeProcessor,
                                            syncActionThread, fullSync, this)) {
    if (m_syncActionQueue) {
        connect(m_syncActionQueue, &SyncActionQueue::countChanged, this,
                &MirrorSyncRuntime::pendingActionsChanged);
    }

    if (m_changeProcessor) {
        connect(m_changeProcessor, &ChangeProcessor::stateChanged, this,
                &MirrorSyncRuntime::processorStateChanged);
    }
}

MirrorSyncRuntime::~MirrorSyncRuntime() = default;

void MirrorSyncRuntime::setSyncFolder(const QString& syncFolder) {
    if (m_localWatcher) {
        m_localWatcher->setSyncFolder(syncFolder);
    }
    if (m_syncActionThread) {
        m_syncActionThread->setSyncFolder(syncFolder);
    }
    if (m_changeProcessor) {
        m_changeProcessor->setSyncFolder(syncFolder);
    }
    if (m_fullSync) {
        m_fullSync->setSyncFolder(syncFolder);
    }
}

void MirrorSyncRuntime::setChangeToken(const QString& token) {
    if (m_remoteWatcher) {
        m_remoteWatcher->setChangeToken(token);
    }
}

void MirrorSyncRuntime::setConflictResolutionStrategy(ConflictResolutionStrategy strategy) {
    if (m_changeProcessor) {
        m_changeProcessor->setConflictResolutionStrategy(strategy);
    }
}

void MirrorSyncRuntime::setPeriodicLocalFullSyncInterval(int intervalMs) {
    if (m_controller) {
        m_controller->setPeriodicLocalFullSyncInterval(intervalMs);
    }
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
}