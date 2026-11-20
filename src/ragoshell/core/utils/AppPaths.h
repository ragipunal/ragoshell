#pragma once

#include <QString>
#include <QUrl>

namespace RagoShell
{
    /**
     * Folder for everything the app stores about the user, with a trailing
     * slash: settings, saved connections, query history, schema catalog,
     * encryption key, MCP settings and logs.
     *
     *  - $RAGOSHELL_CONFIG_DIR when set (isolated test runs, portable setups)
     *  - desktop: ~/.ragoshell/
     *  - iOS and Android: the app's own data location, because the home
     *    folder is not writable there
     *
     * Computed on every call (a cheap lookup), so nothing depends on static
     * initialization order, and on mobile the application name is known by
     * the time the first caller runs.
     */
    QString appDataDir();

    /// The desktop default folder (~/.ragoshell/) or the mobile data location.
    QString defaultAppDataDir();
    /// True when RAGOSHELL_CONFIG_DIR chooses the folder.
    bool appDataDirOverridden();

    /// "<productId>.<extension>" inside appDataDir(): ragoshell.json, ragoshell.key, ragoshell.log.
    QString appFileName(const char *extension);

    /// RAGOSHELL_<suffix>.
    QString productEnvName(const char *suffix);
    /// Value of RAGOSHELL_<suffix>; empty when it is not set.
    QString productEnv(const char *suffix);
    /// productEnv() as a number, 0 when unset or not a number (like qEnvironmentVariableIntValue).
    int productEnvInt(const char *suffix);

    /// <appDataDir>/logs/ (trailing slash).
    QString appLogDir();

    /// <appDataDir>/credentials/ (trailing slash) for imported mobile SSH keys & TLS certificates.
    QString appCredentialsDir();

    /**
     * Copies a picked SSH private key or TLS certificate/CRL file (including
     * Android SAF content:// URIs and iOS security-scoped file URLs) into
     * `credentialsDir` with owner-only permissions (0700 directory, 0600 file)
     * so C libraries (libssh2, mongoc/OpenSSL) receive a real, persistent POSIX
     * file path. Rejects path traversal, empty files, and files over 4 MiB.
     */
    QString importCredentialFileToDir(const QUrl &url,
                                      const QString &credentialsDir,
                                      QString *errorOut = nullptr);
}
