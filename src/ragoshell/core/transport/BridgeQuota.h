#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>

#include <array>

namespace RagoShell
{
    /// Monthly transfer counter of the hosted web gateway's free tier
    /// (docs/HostedGateway.md §9.2/§9.3): bytes per account per calendar
    /// month (UTC), persisted in one local JSON file so a restart does not
    /// reset anyone's quota. Account ids are kept and stored as SHA-256 hex.
    ///
    /// Single-threaded by design, like DeviceSessionLedger: every
    /// GatewaySession lives on the listener's event-loop thread.
    class BridgeQuota
    {
    public:
        /// monthlyLimit <= 0 is treated as 1 byte (every transfer refused):
        /// a misconfigured limit must not become "unlimited".
        BridgeQuota(const QString &filePath, qint64 monthlyLimit);

        /// Reads the counter file. Missing file → empty counters, true.
        /// Unreadable or malformed file → false with *error (fail closed).
        bool load(QString *error);

        /// Writes the counters atomically (temp file + rename) and clears
        /// the dirty flag on success. No-op success when nothing changed.
        bool save(QString *error);

        /// Adds `bytes` for `account` when the total stays within the limit;
        /// returns false (and adds nothing) when it would cross it.
        bool consume(const QString &account, qint64 bytes, qint64 nowSec);

        /// True when the account has no transfer left this month.
        bool exhausted(const QString &account, qint64 nowSec);

        qint64 used(const QString &account, qint64 nowSec);

        /// Aggregates for the status file (§9.7): all accounts together, no ids.
        struct Totals
        {
            QString month;
            qint64 bytes = 0;
            int accounts = 0;
            int exhausted = 0;
        };
        Totals totals(qint64 nowSec);
        qint64 monthlyLimit() const { return _limit; }
        bool isDirty() const { return _dirty; }
        /// False after kMaxSaveFailures failed saves in a row: the counters
        /// then live only in memory and new sessions are refused (§9.3).
        bool healthy() const { return _saveFailures < kMaxSaveFailures; }
        static constexpr int kMaxSaveFailures = 3;
        QString filePath() const { return _path; }

        /// "YYYY-MM" of the UTC calendar month containing nowSec.
        static QString monthKey(qint64 nowSec);
        /// Storage key for an account id (SHA-256 hex of the lowercased id).
        static QString accountKey(const QString &account);

    private:
        void rollMonth(qint64 nowSec);

        QString _path;
        qint64 _limit;
        QString _month;
        QHash<QString, qint64> _bytes; // accountKey → bytes this month
        bool _dirty = false;
        int _saveFailures = 0;
    };

    /// WebSocket connections refused at accept because every bridge slot was taken, per minute
    /// over the last hour (§9.7; threat Q8 of §9.5).
    class RefusalWindow
    {
    public:
        void add(qint64 nowSec);
        int lastHour(qint64 nowSec) const;

    private:
        std::array<qint64, 60> _minute{};
        std::array<int, 60> _count{};
    };

    /// Aggregate, account-free status of the hosted bridge, written once a
    /// minute for the host check (docs/HostedGateway.md §9.7).
    struct BridgeStatus
    {
        qint64 writtenAt = 0;
        QString month;
        int sessions = 0;
        int maxSessions = 0;
        int fullRefusals1h = 0;
        qint64 monthlyBytes = 0;
        qint64 monthlyLimit = 0;
        int accounts = 0;
        int accountsExhausted = 0;
        bool quotaHealthy = true;
    };

    /// "key=value" lines, integers only (the month as YYYY-MM).
    QByteArray formatBridgeStatus(const BridgeStatus &status);
    /// Writes the status atomically, mode 0644 (it holds no account data).
    bool writeBridgeStatus(const QString &path, const BridgeStatus &status, QString *error);

    /// Accounts whose hosted bridge access the operator switched off
    /// (docs/HostedGateway.md §9.4): one account UUID per line, '#' starts a
    /// comment. reload() re-reads the whole file (at most 4 MiB); once a list
    /// has loaded, an unreadable or vanished file keeps the last list — a lost
    /// mount must not unblock every listed account.
    class AccountDenyList
    {
    public:
        explicit AccountDenyList(const QString &filePath = QString()) : _path(filePath) {}

        /// Returns false with *error when the file cannot be read or has bad
        /// lines. A file missing at the first load means an empty list; an
        /// empty file clears the list.
        bool reload(QString *error = nullptr);

        bool contains(const QString &account) const
        {
            return !_accounts.isEmpty() && _accounts.contains(account.trimmed().toLower());
        }
        int size() const { return _accounts.size(); }
        bool enabled() const { return !_path.isEmpty(); }

    private:
        QString _path;
        QSet<QString> _accounts;
        bool _loaded = false;
    };
}
