#include "ragoshell/core/transport/BridgeQuota.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>

namespace RagoShell
{
    namespace
    {
        // About 80 bytes per account: 64 MiB is far beyond any real month
        // and stops a planted huge file from exhausting memory at startup.
        constexpr qint64 kMaxQuotaFileBytes = 64LL * 1024 * 1024;
        constexpr qint64 kMaxDenyFileBytes = 4LL * 1024 * 1024;

        bool isHex64(const QString &s)
        {
            static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
            return re.match(s).hasMatch();
        }

        bool readBoundedRegularFile(const QString &path,
                                    qint64 maxBytes,
                                    const char *label,
                                    QByteArray *out,
                                    bool *missing,
                                    QString *error)
        {
            out->clear();
            if (missing)
                *missing = false;
            const QFileInfo info(path);
            if (info.isSymLink() || info.isJunction()) {
                if (error)
                    *error = QStringLiteral("%1 file must not be a symlink").arg(QLatin1String(label));
                return false;
            }
            if (!info.exists()) {
                if (missing)
                    *missing = true;
                return true;
            }
            if (!info.isFile()) {
                if (error)
                    *error = QStringLiteral("%1 path is not a regular file").arg(QLatin1String(label));
                return false;
            }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                if (error)
                    *error = QStringLiteral("cannot open %1 file: %2").arg(QLatin1String(label), f.errorString());
                return false;
            }
            constexpr qint64 kChunkBytes = 64 * 1024;
            while (out->size() <= maxBytes) {
                const qint64 want = std::min(kChunkBytes, (maxBytes + 1) - out->size());
                const QByteArray chunk = f.read(want);
                if (f.error() != QFileDevice::NoError) {
                    if (error)
                        *error = QStringLiteral("cannot read %1 file: %2").arg(QLatin1String(label), f.errorString());
                    return false;
                }
                if (chunk.isEmpty())
                    break;
                out->append(chunk);
            }
            if (out->size() > maxBytes) {
                if (error)
                    *error = QStringLiteral("%1 file is larger than %2 bytes").arg(QLatin1String(label)).arg(maxBytes);
                return false;
            }
            return true;
        }
    }

    BridgeQuota::BridgeQuota(const QString &filePath, qint64 monthlyLimit)
        : _path(filePath), _limit(std::clamp<qint64>(monthlyLimit, 1, kMaxSafeJsonBytes))
    {
    }

    QString BridgeQuota::monthKey(qint64 nowSec)
    {
        return QDateTime::fromSecsSinceEpoch(nowSec, QTimeZone::UTC)
            .toString(QStringLiteral("yyyy-MM"));
    }

    QString BridgeQuota::accountKey(const QString &account)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(account.trimmed().toLower().toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex());
    }

    void BridgeQuota::rollMonth(qint64 nowSec)
    {
        const QString month = monthKey(nowSec);
        // "YYYY-MM" compares in calendar order. Only a later month resets the
        // counters: a clock stepping back (or forward and back again) must
        // not hand every account a fresh quota; it keeps counting into the
        // current month.
        if (!_month.isEmpty() && month <= _month)
            return;
        _month = month;
        if (!_bytes.isEmpty())
            _bytes.clear();
        _dirty = true;
    }

    bool BridgeQuota::load(QString *error)
    {
        _bytes.clear();
        _month.clear();
        _dirty = false;
        QByteArray raw;
        bool missing = false;
        if (!readBoundedRegularFile(_path, kMaxQuotaFileBytes, "quota", &raw, &missing, error))
            return false;
        if (missing)
            return true;
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral("quota file is not a JSON object");
            return false;
        }
        const QJsonObject root = doc.object();
        static const QRegularExpression monthRe(QStringLiteral("^[0-9]{4}-[0-9]{2}$"));
        const QString month = root.value(QStringLiteral("month")).toString();
        if (root.value(QStringLiteral("version")).toInt() != 1
            || !monthRe.match(month).hasMatch()
            || !root.value(QStringLiteral("accounts")).isObject()) {
            if (error)
                *error = QStringLiteral("quota file has an unknown layout");
            return false;
        }
        const QJsonObject accounts = root.value(QStringLiteral("accounts")).toObject();
        QHash<QString, qint64> bytes;
        bytes.reserve(accounts.size());
        for (auto it = accounts.begin(); it != accounts.end(); ++it) {
            const double v = it.value().toDouble(-1);
            if (!isHex64(it.key()) || !it.value().isDouble() || v < 0
                || v != std::floor(v) || v > double(kMaxSafeJsonBytes)) {
                if (error)
                    *error = QStringLiteral("quota file has an invalid entry");
                return false;
            }
            bytes.insert(it.key(), static_cast<qint64>(v));
        }
        _month = month;
        _bytes = std::move(bytes);
        return true;
    }

    bool BridgeQuota::save(QString *error)
    {
        if (!_dirty)
            return true;
        QJsonObject accounts;
        for (auto it = _bytes.cbegin(); it != _bytes.cend(); ++it)
            accounts.insert(it.key(), double(it.value()));
        const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("month"), _month},
            {QStringLiteral("accounts"), accounts},
        };
        QSaveFile f(_path);
        if (!f.open(QIODevice::WriteOnly)) {
            ++_saveFailures;
            if (error)
                *error = QStringLiteral("cannot write quota file: %1").arg(f.errorString());
            return false;
        }
        // Owner-only: the file says which (hashed) accounts used the bridge.
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        if (!f.commit()) {
            ++_saveFailures;
            if (error)
                *error = QStringLiteral("cannot write quota file: %1").arg(f.errorString());
            return false;
        }
        _saveFailures = 0;
        _dirty = false;
        return true;
    }

    bool BridgeQuota::recordTransfer(const QString &account, qint64 bytes, qint64 nowSec, bool exhaustOnOver)
    {
        if (bytes < 0)
            return false;
        rollMonth(nowSec);
        const QString key = accountKey(account);
        const qint64 used = _bytes.value(key);
        if (bytes > _limit - used) {
            if (exhaustOnOver && used < _limit) {
                _bytes.insert(key, _limit);
                _dirty = true;
            }
            return false;
        }
        if (bytes == 0)
            return true;
        _bytes.insert(key, used + bytes);
        _dirty = true;
        return true;
    }

    bool BridgeQuota::consume(const QString &account, qint64 bytes, qint64 nowSec)
    {
        return recordTransfer(account, bytes, nowSec, false);
    }

    bool BridgeQuota::consumeOrExhaust(const QString &account, qint64 bytes, qint64 nowSec)
    {
        return recordTransfer(account, bytes, nowSec, true);
    }

    bool BridgeQuota::exhausted(const QString &account, qint64 nowSec)
    {
        return used(account, nowSec) >= _limit;
    }

    qint64 BridgeQuota::used(const QString &account, qint64 nowSec)
    {
        rollMonth(nowSec);
        return _bytes.value(accountKey(account));
    }

    BridgeQuota::Totals BridgeQuota::totals(qint64 nowSec)
    {
        rollMonth(nowSec);
        Totals t;
        t.month = _month;
        constexpr qint64 kMax = std::numeric_limits<qint64>::max();
        for (auto it = _bytes.cbegin(); it != _bytes.cend(); ++it) {
            if (it.value() <= 0)
                continue;
            ++t.accounts;
            t.bytes = (it.value() > kMax - t.bytes) ? kMax : (t.bytes + it.value());
            if (it.value() >= _limit)
                ++t.exhausted;
        }
        return t;
    }

    void RefusalWindow::add(qint64 nowSec)
    {
        const qint64 minute = nowSec / 60;
        const auto i = static_cast<size_t>(((minute % 60) + 60) % 60);
        if (_minute[i] != minute) {
            _minute[i] = minute;
            _count[i] = 0;
        }
        if (_count[i] < std::numeric_limits<int>::max())
            ++_count[i];
    }

    int RefusalWindow::lastHour(qint64 nowSec) const
    {
        const qint64 minute = nowSec / 60;
        qint64 sum = 0;
        for (size_t i = 0; i < _minute.size(); ++i) {
            const qint64 age = minute - _minute[i];
            if (_count[i] > 0 && age >= 0 && age < 60)
                sum += _count[i];
        }
        return int(std::min<qint64>(sum, std::numeric_limits<int>::max()));
    }

    QByteArray formatBridgeStatus(const BridgeStatus &s)
    {
        static const QRegularExpression monthRe(QStringLiteral("^[0-9]{4}-[0-9]{2}$"));
        QByteArray out;
        const auto line = [&out](const char *key, qint64 value) {
            out += key;
            out += '=';
            out += QByteArray::number(std::max<qint64>(value, 0));
            out += '\n';
        };
        line("version", 1);
        line("written_at", s.writtenAt);
        out += "month=";
        out += monthRe.match(s.month).hasMatch() ? s.month.toLatin1() : QByteArray("0000-00");
        out += '\n';
        line("sessions", s.sessions);
        line("max_sessions", s.maxSessions);
        line("full_refusals_1h", s.fullRefusals1h);
        line("monthly_bytes", s.monthlyBytes);
        line("monthly_limit", s.monthlyLimit);
        line("accounts", s.accounts);
        line("accounts_exhausted", s.accountsExhausted);
        line("quota_healthy", s.quotaHealthy ? 1 : 0);
        return out;
    }

    bool writeBridgeStatus(const QString &path, const BridgeStatus &status, QString *error)
    {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            if (error)
                *error = QStringLiteral("cannot write status file: %1").arg(f.errorString());
            return false;
        }
        // Aggregates only: readable by the host check running as another user.
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                         | QFileDevice::ReadOther);
        f.write(formatBridgeStatus(status));
        if (!f.commit()) {
            if (error)
                *error = QStringLiteral("cannot write status file: %1").arg(f.errorString());
            return false;
        }
        return true;
    }

    bool AccountDenyList::reload(QString *error)
    {
        if (_path.isEmpty())
            return true;
        QByteArray rawData;
        bool missing = false;
        if (!readBoundedRegularFile(_path, kMaxDenyFileBytes, "deny", &rawData, &missing, error))
            return false;
        if (missing) {
            if (!_loaded) {
                _loaded = true; // never existed: nobody is listed
                return true;
            }
            if (error)
                *error = QStringLiteral("deny file vanished; keeping the last list (%1 accounts); "
                                        "empty the file to clear it")
                             .arg(_accounts.size());
            return false;
        }
        static const QRegularExpression uuidRe(QStringLiteral(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
        QSet<QString> accounts;
        int skipped = 0;
        const QList<QByteArray> lines = rawData.split('\n');
        for (const QByteArray &raw : lines) {
            QString line = QString::fromUtf8(raw);
            const int hash = line.indexOf(QLatin1Char('#'));
            if (hash >= 0)
                line.truncate(hash);
            line = line.trimmed().toLower();
            if (line.isEmpty())
                continue;
            if (uuidRe.match(line).hasMatch())
                accounts.insert(line);
            else
                ++skipped;
        }
        _accounts = std::move(accounts);
        _loaded = true;
        if (skipped > 0 && error)
            *error = QStringLiteral("deny file: %1 line(s) are not account UUIDs and were ignored")
                         .arg(skipped);
        return skipped == 0;
    }
}
