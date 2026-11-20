#pragma once

#include <functional>

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /**
     * @brief Opt-in settings of the read-only MCP server (ragoshell-mcp).
     *
     * Stored in its own file (<settings dir>/mcp.json) so the GUI, which
     * rewrites ragoshell.json on every save, can never drop or widen them.
     * A missing or unreadable file means "disabled, nothing exposed".
     */
    struct McpConfig
    {
        static constexpr int kDefaultMaxDocuments = 50;
        static constexpr int kHardMaxDocuments = 1000;
        static constexpr int kDefaultMaxTimeMs = 15000;
        static constexpr int kHardMaxTimeMs = 120000;
        static constexpr int kDefaultMaxResultBytes = 512 * 1024;
        static constexpr int kDefaultSampleSize = 100;
        static constexpr int kHardMaxSampleSize = 1000;

        bool enabled = false;
        QStringList allowedConnections;   ///< connection UUIDs
        int maxDocuments = kDefaultMaxDocuments;
        int maxTimeMs = kDefaultMaxTimeMs;
        int maxResultBytes = kDefaultMaxResultBytes;

        /// mcp.json next to ragoshell.json (~/.ragoshell/ unless RAGOSHELL_CONFIG_DIR is set)
        static QString defaultPath();

        /// Defaults when the file is missing; false with 'error' set when it
        /// exists but cannot be parsed (the caller must then stay disabled).
        static bool load(const QString &path, McpConfig &out, QString *error = nullptr);
        bool save(const QString &path, QString *error = nullptr) const;

        /// Read-modify-write of the file on disk under a lock file (the app and
        /// the command line may both change it). Refuses, without writing, when
        /// the existing file cannot be read or parsed: a broken file is never replaced.
        static bool modify(const QString &path, const std::function<void(McpConfig &)> &change,
                           QString *error = nullptr);

        bool isAllowed(const QString &uuid) const { return allowedConnections.contains(uuid); }

        /// Drops allowed uuids of connections that no longer exist (deleted in
        /// the app or by sync), so a uuid that comes back is not exposed unasked.
        void keepOnly(const QStringList &existingUuids);

        /// Limits clamped to their hard maximums (a hand-edited file cannot raise them).
        McpConfig normalized() const;
    };
}
