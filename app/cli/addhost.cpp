#include "addhost.h"

#include "backend/computermanager.h"
#include "backend/identitymanager.h"
#include "settings/streamingpreferences.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSslCertificate>
#include <QStandardPaths>

#include <cstdio>

#define CLIENT_LOCK_NAME "dusk-client.lock"

namespace CliAddHost
{

static void printError(const QString& message)
{
    fputs(qPrintable(message + '\n'), stderr);
    fflush(stderr);
}

QString clientLockPath()
{
    QSettings settings;
    QString dir;

#ifdef Q_OS_WIN32
    // The registry has no folder of its own to put a lock file in
    if (settings.format() == QSettings::NativeFormat) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    }
    else
#endif
    {
        dir = QFileInfo(settings.fileName()).absolutePath();
    }

    return QDir(dir).filePath(CLIENT_LOCK_NAME);
}

// The age-based staleness of QLockFile (30 s by default) would treat a lock
// held by a long-running client as stale, because nothing refreshes it, and
// add-host would take it from under the running client. Staleness is
// judged by whether the holding process is still alive instead.
static QLockFile* newClientLock(QString& error)
{
    QString path = clientLockPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QString("Cannot create the client config folder for %1").arg(path);
        return nullptr;
    }

    QLockFile* lock = new QLockFile(path);
    lock->setStaleLockTime(0);
    return lock;
}

QLockFile* takeClientLockIfFree()
{
    QString error;
    QLockFile* lock = newClientLock(error);
    if (lock == nullptr) {
        return nullptr;
    }

    if (!lock->tryLock(0)) {
        delete lock;
        return nullptr;
    }

    return lock;
}

int run(const AddHostCommandLineParser& args)
{
    QString error;
    QScopedPointer<QLockFile> lock(newClientLock(error));
    if (lock.isNull()) {
        printError(error);
        return 1;
    }

    if (!lock->tryLock(0)) {
        if (lock->error() == QLockFile::LockFailedError) {
            printError("Another Dusk Stream client is running; close it and try again");
            return 2;
        }
        printError(QString("Cannot take the client lock %1").arg(clientLockPath()));
        return 1;
    }

    QFile certFile(args.getServerCertPath());
    if (!certFile.open(QIODevice::ReadOnly)) {
        printError(QString("Cannot read server certificate %1: %2")
                   .arg(args.getServerCertPath(), certFile.errorString()));
        return 1;
    }

    QList<QSslCertificate> certs = QSslCertificate::fromData(certFile.readAll(), QSsl::Pem);
    if (certs.isEmpty() || certs.first().isNull()) {
        printError(QString("Not a PEM certificate: %1").arg(args.getServerCertPath()));
        return 1;
    }

    // Make sure this client has an identity -- created the usual way on
    // first run -- so Dusk can hand its certificate to the host.
    IdentityManager::get();

    // Store the host through moonlight-qt's own persistence, so it is
    // written exactly as the client writes a host it paired with itself.
    // Destroying the manager waits for its pending flush to finish.
    {
        ComputerManager* manager = new ComputerManager(StreamingPreferences::get());
        manager->addPairedHost(args.getUuid(),
                               args.getName(),
                               NvAddress(args.getAddress(), args.getPort()),
                               certs.first());
        delete manager;
    }

    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        printError(QString("Cannot write the client settings %1").arg(settings.fileName()));
        return 1;
    }

    return 0;
}

}
