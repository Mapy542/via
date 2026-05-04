/**
 * @file MirrorSyncController.cpp
 * @brief Implementation of mirror-sync lifecycle control
 */

#include "MirrorSyncController.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include "ChangeProcessor.h"
#include "FullSync.h"
#include "LocalChangeWatcher.h"
#include "RemoteChangeWatcher.h"
#include "SyncActionThread.h"

namespace {
template <typename Func>
void invokeAsync(QObject* target, Func&& func) {
    if (!target) {
        return;
    }

    if (QThread::currentThread() == target->thread()) {
        func();
        return;
    }

    QMetaObject::invokeMethod(target, std::forward<Func>(func), Qt::QueuedConnection);
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
}  // namespace

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
    invokeAsync(m_localWatcher, [this]() { m_localWatcher->start(); });
    invokeAsync(m_remoteWatcher, [this]() { m_remoteWatcher->start(); });
    invokeAsync(m_changeProcessor, [this]() { m_changeProcessor->start(); });
    invokeAsync(m_syncActionThread, [this]() { m_syncActionThread->start(); });
}

void MirrorSyncController::stop() {
    qInfo() << "Stopping sync components...";
    invokeBlocking(m_syncActionThread, [this]() { m_syncActionThread->stop(); });
    invokeBlocking(m_changeProcessor, [this]() { m_changeProcessor->stop(); });
    invokeBlocking(m_remoteWatcher, [this]() { m_remoteWatcher->stop(); });
    invokeBlocking(m_localWatcher, [this]() { m_localWatcher->stop(); });
}

void MirrorSyncController::pause() {
    invokeBlocking(m_fullSync, [this]() { m_fullSync->cancel(); });
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->stop();
    }
    invokeBlocking(m_localWatcher, [this]() { m_localWatcher->pause(); });
    invokeBlocking(m_remoteWatcher, [this]() { m_remoteWatcher->pause(); });
    invokeBlocking(m_changeProcessor, [this]() { m_changeProcessor->pause(); });
    invokeBlocking(m_syncActionThread, [this]() { m_syncActionThread->pause(); });
}

void MirrorSyncController::resume() {
    invokeAsync(m_localWatcher, [this]() { m_localWatcher->resume(); });
    invokeAsync(m_remoteWatcher, [this]() { m_remoteWatcher->resume(); });
    invokeAsync(m_changeProcessor, [this]() { m_changeProcessor->resume(); });
    invokeAsync(m_syncActionThread, [this]() { m_syncActionThread->resume(); });
    if (m_fullSyncLocalTimer) {
        m_fullSyncLocalTimer->start();
    }
}

void MirrorSyncController::cancelAndStop() {
    invokeBlocking(m_fullSync, [this]() { m_fullSync->cancel(); });
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
    invokeAsync(m_remoteWatcher, [this]() {
        m_remoteWatcher->stop();
        m_remoteWatcher->start();
    });
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
        invokeAsync(m_fullSync, [this]() { m_fullSync->fullSync(); });
        return;
    }

    QTimer::singleShot(delayMs, this,
                       [this]() { invokeAsync(m_fullSync, [this]() { m_fullSync->fullSync(); }); });
}

void MirrorSyncController::clearSessionState() {
    invokeBlocking(m_changeProcessor, [this]() { m_changeProcessor->clearState(); });
    invokeBlocking(m_syncActionThread, [this]() { m_syncActionThread->clearInProgressActions(); });
    invokeBlocking(m_remoteWatcher, [this]() { m_remoteWatcher->clearChangeToken(); });
    invokeBlocking(m_fullSync, [this]() { m_fullSync->clearPendingState(); });
}