/**
 * @file MirrorSyncController.cpp
 * @brief Implementation of mirror-sync lifecycle control
 */

#include "MirrorSyncController.h"

#include <QTimer>

#include "ChangeProcessor.h"
#include "FullSync.h"
#include "LocalChangeWatcher.h"
#include "RemoteChangeWatcher.h"
#include "SyncActionThread.h"

MirrorSyncController::MirrorSyncController(LocalChangeWatcher* localWatcher,
                                           RemoteChangeWatcher* remoteWatcher,
                                           ChangeProcessor* changeProcessor,
                                           SyncActionThread* syncActionThread, FullSync* fullSync,
                                           QObject* parent)
    : QObject(parent),
      m_localWatcher(localWatcher),
      m_remoteWatcher(remoteWatcher),
      m_changeProcessor(changeProcessor),
      m_syncActionThread(syncActionThread),
      m_fullSync(fullSync),
      m_fullSyncLocalTimer(new QTimer(this)) {
    m_fullSyncLocalTimer->setInterval(DEFAULT_LOCAL_FULL_SYNC_INTERVAL_MS);
    m_fullSyncLocalTimer->setSingleShot(false);

    if (m_fullSync) {
        connect(m_fullSyncLocalTimer, &QTimer::timeout, m_fullSync, &FullSync::fullSyncLocal);
    }
}

MirrorSyncController::~MirrorSyncController() = default;

void MirrorSyncController::setPeriodicLocalFullSyncInterval(int intervalMs) {
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->setInterval(intervalMs);
    }
}

void MirrorSyncController::start() {
    qInfo() << "Starting sync components...";
    if (m_localWatcher) {
        m_localWatcher->start();
    }
    if (m_remoteWatcher) {
        m_remoteWatcher->start();
    }
    if (m_changeProcessor) {
        m_changeProcessor->start();
    }
    if (m_syncActionThread) {
        m_syncActionThread->start();
    }
}

void MirrorSyncController::stop() {
    qInfo() << "Stopping sync components...";
    if (m_syncActionThread) {
        m_syncActionThread->stop();
    }
    if (m_changeProcessor) {
        m_changeProcessor->stop();
    }
    if (m_remoteWatcher) {
        m_remoteWatcher->stop();
    }
    if (m_localWatcher) {
        m_localWatcher->stop();
    }
}

void MirrorSyncController::pause() {
    if (m_fullSync) {
        m_fullSync->cancel();
    }
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->stop();
    }
    if (m_localWatcher) {
        m_localWatcher->pause();
    }
    if (m_remoteWatcher) {
        m_remoteWatcher->pause();
    }
    if (m_changeProcessor) {
        m_changeProcessor->pause();
    }
    if (m_syncActionThread) {
        m_syncActionThread->pause();
    }
}

void MirrorSyncController::resume() {
    if (m_localWatcher) {
        m_localWatcher->resume();
    }
    if (m_remoteWatcher) {
        m_remoteWatcher->resume();
    }
    if (m_changeProcessor) {
        m_changeProcessor->resume();
    }
    if (m_syncActionThread) {
        m_syncActionThread->resume();
    }
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->start();
    }
}

void MirrorSyncController::cancelAndStop() {
    if (m_fullSync) {
        m_fullSync->cancel();
    }
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->stop();
    }
    stop();
}

void MirrorSyncController::startAndScheduleInitialSync(int delayMs) {
    start();
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->start();
    }
    requestFullSync(delayMs);
}

void MirrorSyncController::restartAfterWake(int fullSyncDelayMs) {
    if (m_remoteWatcher) {
        m_remoteWatcher->stop();
        m_remoteWatcher->start();
    }
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->start();
    }
    requestFullSync(fullSyncDelayMs);
}

void MirrorSyncController::requestFullSync(int delayMs) {
    if (!m_fullSync) {
        return;
    }

    if (delayMs <= 0) {
        m_fullSync->fullSync();
        return;
    }

    QTimer::singleShot(delayMs, m_fullSync, &FullSync::fullSync);
}

void MirrorSyncController::clearSessionState() {
    if (m_changeProcessor) {
        m_changeProcessor->clearState();
    }
    if (m_syncActionThread) {
        m_syncActionThread->clearInProgressActions();
    }
    if (m_remoteWatcher) {
        m_remoteWatcher->clearChangeToken();
    }
    if (m_fullSync) {
        m_fullSync->clearPendingState();
    }
}