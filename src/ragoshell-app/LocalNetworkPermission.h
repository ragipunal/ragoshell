#pragma once

#include <functional>

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /**
     * Android 17 (API 37) guards connections to the local network (private
     * addresses, .local names) with the ACCESS_LOCAL_NETWORK runtime permission
     * for apps that target API 37. Without it such connections time out.
     *
     * Runs `proceed` at once when none of the hosts is on the local network or
     * on any platform other than Android. Otherwise it asks for the permission
     * when it is not granted yet and runs `proceed` as soon as the user
     * answers; `proceed` also runs when the user refuses, so the connection
     * fails with the usual error and the log says why.
     */
    void whenLocalNetworkAllowed(const QStringList &hosts, std::function<void()> proceed);

    /**
     * Android 9.0 (API 28) legacy external storage permission check.
     * Requests READ_EXTERNAL_STORAGE or WRITE_EXTERNAL_STORAGE at runtime only
     * on Android API <= 28 when `pathOrUri` is a direct local filesystem path
     * outside `appDataDir()`. No-op on Android 10+ (API 29+, Scoped Storage /
     * SAF content:// URIs), for internal app files, and on all other platforms.
     */
    void ensureLegacyStoragePermission(const QString &pathOrUri, bool writeAccess);
}
