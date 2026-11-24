#include "ragoshell/core/transport/BridgeQuota.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>

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
    }

    BridgeQuota::BridgeQuota(const QString &filePath, qint64 monthlyLimit)
        : _path(filePath), _limit(monthlyLimit > 0 ? monthlyLimit : 1)
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
        QFile f(_path);
        if (!f.exists())
            return true;
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("cannot open quota file: %1").arg(f.errorString());
            return false;
        }
        if (f.size() > kMaxQuotaFileBytes) {
            if (error)
                *error = QStringLiteral("quota file is larger than %1 bytes").arg(kMaxQuotaFileBytes);
            return false;
        }
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
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
                || v != std::floor(v) || v > double(std::numeric_limits<qint64>::max() / 2)) {
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

    bool BridgeQuota::consume(const QString &account, qint64 bytes, qint64 nowSec)
    {
        if (bytes < 0)
            return false;
        rollMonth(nowSec);
        const QString key = accountKey(account);
        const qint64 used = _bytes.value(key);
        if (bytes > _limit - used)
            return false;
        if (bytes == 0)
            return true;
        _bytes.insert(key, used + bytes);
        _dirty = true;
        return true;
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

    bool AccountDenyList::reload(QString *error)
    {
        if (_path.isEmpty())
            return true;
        const QFileInfo info(_path);
        if (!info.exists()) {
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
        QFile f(_path);
        if (info.size() > kMaxDenyFileBytes || !f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = info.size() > kMaxDenyFileBytes
                    ? QStringLiteral("deny file is larger than %1 bytes").arg(kMaxDenyFileBytes)
                    : QStringLiteral("cannot open deny file: %1").arg(f.errorString());
            return false;
        }
        static const QRegularExpression uuidRe(QStringLiteral(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
        QSet<QString> accounts;
        int skipped = 0;
        const QList<QByteArray> lines = f.readAll().split('\n');
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
