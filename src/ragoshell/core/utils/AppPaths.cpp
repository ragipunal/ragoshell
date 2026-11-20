#include "ragoshell/core/utils/AppPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/WasmPersist.h"

namespace RagoShell
{
    namespace
    {
        constexpr qint64 kMaxCredentialFileBytes = 4 * 1024 * 1024; // 4 MiB cap

        QString sanitizeCredentialFileName(const QUrl &url, const QString &sourcePath)
        {
            QString raw = url.isLocalFile() ? QFileInfo(sourcePath).fileName()
                                            : QUrl::fromPercentEncoding(url.toString().toUtf8());
            const QStringList segments = raw.split(QRegularExpression(QStringLiteral("[/\\\\:]")),
                                                   Qt::SkipEmptyParts);
            QString leaf = segments.isEmpty() ? QString() : segments.last();
            leaf.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                         QStringLiteral("_"));
            while (leaf.startsWith(QLatin1Char('.')) || leaf.startsWith(QLatin1Char('_')))
                leaf.remove(0, 1);
            if (leaf.size() > 64)
                leaf = leaf.right(64);
            while (leaf.startsWith(QLatin1Char('.')) || leaf.startsWith(QLatin1Char('_')))
                leaf.remove(0, 1);
            return leaf.isEmpty() ? QStringLiteral("credential.pem") : leaf;
        }
    }

    namespace
    {
        QString productIdLower() { return QString::fromLatin1(BrandInfo::productId).toLower(); }
    }

    QString productEnvName(const char *suffix)
    {
        return productIdLower().toUpper() + QLatin1Char('_') + QLatin1String(suffix);
    }

    QString productEnv(const char *suffix)
    {
        return qEnvironmentVariable(productEnvName(suffix).toLatin1().constData());
    }

    int productEnvInt(const char *suffix)
    {
        bool ok = false;
        const int value = productEnv(suffix).trimmed().toInt(&ok, 0);
        return ok ? value : 0;
    }

    QString appFileName(const char *extension)
    {
        return productIdLower() + QLatin1Char('.') + QLatin1String(extension);
    }

    QString defaultAppDataDir()
    {
#if defined(Q_OS_WASM)
        // IDBFS mount point registered by wasm/persist-pre.js — the only path
        // that survives a reload in the browser (docs/WebStorage.md §1).
        return QStringLiteral("/persist/.%1/").arg(productIdLower());
#elif defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
        return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
               + QLatin1Char('/');
#else
        return QStringLiteral("%1/.%2/").arg(QDir::homePath(), productIdLower());
#endif
    }

    bool appDataDirOverridden()
    {
        return !productEnv("CONFIG_DIR").isEmpty();
    }

    QString appDataDir()
    {
        const QString overridden = productEnv("CONFIG_DIR");
        if (!overridden.isEmpty())
            return QDir::cleanPath(overridden) + QLatin1Char('/');
        return defaultAppDataDir();
    }

    QString appLogDir()
    {
        return appDataDir() + QStringLiteral("logs/");
    }

    QString appCredentialsDir()
    {
        return appDataDir() + QStringLiteral("credentials/");
    }

    QString importCredentialFileToDir(const QUrl &url,
                                      const QString &credentialsDir,
                                      QString *errorOut)
    {
        auto fail = [errorOut](const QString &reason) {
            if (errorOut)
                *errorOut = reason;
            return QString();
        };

        const QString sourcePath = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (sourcePath.trimmed().isEmpty() || credentialsDir.trimmed().isEmpty())
            return fail(QStringLiteral("Empty credential file path."));

        QFile src(sourcePath);
        if (!src.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Cannot read '%1': %2").arg(sourcePath, src.errorString()));

        const QByteArray payload = src.read(kMaxCredentialFileBytes + 1);
        src.close();
        if (payload.isEmpty())
            return fail(QStringLiteral("Credential file is empty."));
        if (payload.size() > kMaxCredentialFileBytes)
            return fail(QStringLiteral("Credential file exceeds the 4 MiB limit."));

        const QString cleanDir = QDir::cleanPath(credentialsDir);
        if (!QDir().mkpath(cleanDir))
            return fail(QStringLiteral("Cannot create credentials directory."));
        QFile::setPermissions(cleanDir,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

        const QByteArray digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().left(16);
        const QString safeLeaf = sanitizeCredentialFileName(url, sourcePath);
        const QString entryName = QStringLiteral("%1_%2")
                                      .arg(QString::fromLatin1(digest), safeLeaf);
        const QString destPath = QDir::cleanPath(QDir(cleanDir).filePath(entryName));
        if (!destPath.startsWith(cleanDir + QLatin1Char('/')))
            return fail(QStringLiteral("Resolved credential path escaped target directory."));

        // Same content already imported: reuse the path (Windows QSaveFile can fail
        // rewriting an owner-only file that still has a lingering handle).
        {
            QFile existing(destPath);
            if (existing.open(QIODevice::ReadOnly) && existing.readAll() == payload) {
                if (errorOut)
                    errorOut->clear();
                return destPath;
            }
        }

        QSaveFile out(destPath);
        out.setDirectWriteFallback(true);
        if (!out.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("Cannot open destination '%1': %2").arg(destPath, out.errorString()));
        out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (out.write(payload) != payload.size() || !out.commit())
            return fail(QStringLiteral("Cannot write destination '%1': %2").arg(destPath, out.errorString()));

        QFile::setPermissions(destPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        requestPersistSync();
        if (errorOut)
            errorOut->clear();
        return destPath;
    }
}
