#include "ragoshell-app/SchemaAnalyzer.h"

#include <algorithm>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kMaxSampleSize = 100000;
        constexpr int kMaxTimeMs = 30000;

        QString countsText(const std::vector<std::pair<QString, qint64>> &counts, qint64 total)
        {
            QStringList parts;
            for (const auto &[name, n] : counts) {
                if (total <= 0) {
                    parts << name;
                    continue;
                }
                // A rare type must not read as "0%".
                const int pct = qRound(100.0 * n / total);
                parts << (pct == 0 ? QStringLiteral("%1 <1%").arg(name)
                                   : QStringLiteral("%1 %2%").arg(name).arg(pct));
            }
            return parts.join(QStringLiteral("  ·  "));
        }
    }

    SchemaAnalyzer::SchemaAnalyzer(QObject *parent)
        : QAbstractListModel(parent)
    {
        if (SchemaService *service = SchemaService::instance())
            connect(service, &SchemaService::finished, this, &SchemaAnalyzer::onJobFinished);
    }

    int SchemaAnalyzer::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(_result.fields.size());
    }

    QVariant SchemaAnalyzer::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(_result.fields.size()))
            return QVariant();
        const SchemaField &f = _result.fields[index.row()];
        switch (role) {
        case PathRole:         return f.path;
        case NameRole:         return f.name;
        case DepthRole:        return f.depth;
        case PresenceRole:     return f.presence;
        case PresentCountRole: return f.presentCount;
        case TypesRole: {
            qint64 total = 0;
            for (const auto &t : f.types)
                total += t.second;
            return countsText(f.types, total);
        }
        case ItemTypesRole: {
            qint64 total = 0;
            for (const auto &t : f.itemTypes)
                total += t.second;
            return countsText(f.itemTypes, total);
        }
        case ValuesRole: {
            QStringList parts;
            for (const auto &[value, n] : f.topValues)
                parts << QStringLiteral("%1 (%2)").arg(value).arg(n);
            if (f.valuesTruncated)
                parts << QStringLiteral("…");
            return parts.join(QStringLiteral(", "));
        }
        case MinValueRole:     return f.minValue;
        case MaxValueRole:     return f.maxValue;
        default:               return QVariant();
        }
    }

    QHash<int, QByteArray> SchemaAnalyzer::roleNames() const
    {
        return {
            {PathRole, "path"},
            {NameRole, "name"},
            {DepthRole, "depth"},
            {PresenceRole, "presence"},
            {PresentCountRole, "presentCount"},
            {TypesRole, "typesText"},
            {ItemTypesRole, "itemTypesText"},
            {ValuesRole, "valuesText"},
            {MinValueRole, "minValue"},
            {MaxValueRole, "maxValue"},
        };
    }

    void SchemaAnalyzer::setSampleSize(int size)
    {
        size = std::clamp(size, 1, kMaxSampleSize);
        if (_sampleSize == size)
            return;
        _sampleSize = size;
        emit sampleSizeChanged();
    }

    bool SchemaAnalyzer::configure(const QString &serverUuid, const QString &database,
                                   const QString &collection)
    {
        QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek();
        std::unique_ptr<ConnectionSettings> conn = backend ? backend->cloneConnection(serverUuid) : nullptr;
        if (!conn || database.isEmpty() || collection.isEmpty())
            return false;
        _connection = std::move(conn);
        _serverUuid = serverUuid;
        _database = database;
        _collection = collection;
        emit targetChanged();
        analyze();
        return true;
    }

    void SchemaAnalyzer::analyze()
    {
        SchemaService *service = SchemaService::instance();
        if (!_connection || !service)
            return;
        auto *settings = AppRegistry::instance().settingsManager();
        QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek();

        SchemaService::Request request;
        request.connectionId = _serverUuid;
        request.connection = _connection;
        request.database = _database;
        request.collection = _collection;
        request.priority = SchemaService::Priority::Interactive;
        request.sampleSize = _sampleSize;
        request.maxTimeMs = kMaxTimeMs;
        request.valuesIncluded = settings && settings->aiIncludeSampleValues();
        request.serverVersion = backend ? backend->serverVersionString(_serverUuid) : QString();
        request.uuidEncoding = settings ? settings->uuidEncoding() : DefaultEncoding;
        request.timeZone = settings ? settings->timeZone() : Utc;

        // Closing the dialog never waits for a slow sample; a newer ticket
        // supersedes this one.
        _ticket = service->enqueue(request);
        _busy = _ticket != 0;
        _error.clear();
        emit stateChanged();
    }

    void SchemaAnalyzer::onJobFinished(quint64 ticket, const SchemaJobOutcome &outcome)
    {
        if (ticket != _ticket)
            return;
        _ticket = 0;
        _busy = false;
        if (outcome.cancelled) {
            emit stateChanged();
            return;
        }
        if (!outcome.result.ok) {
            _error = outcome.result.error;
            emit stateChanged();
            return;
        }
        beginResetModel();
        _result = outcome.result;
        endResetModel();
        emit stateChanged();
    }
}
