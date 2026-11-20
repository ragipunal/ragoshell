#pragma once

#include <memory>

#include <QAbstractListModel>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    class ConnectionSettings;

    struct SchemaJobOutcome;

    /**
     * @brief QML "SchemaAnalyzer": one row per field path of a sampled
     * collection (presence, type mix, frequent values, numeric/date range).
     * Sampling is an Interactive job of the SchemaService, which also puts
     * the result in the schema catalog the AI assistant uses.
     */
    class SchemaAnalyzer : public QAbstractListModel
    {
        Q_OBJECT
        QML_ELEMENT

        Q_PROPERTY(QString serverUuid READ serverUuid NOTIFY targetChanged FINAL)
        Q_PROPERTY(QString database READ database NOTIFY targetChanged FINAL)
        Q_PROPERTY(QString collection READ collection NOTIFY targetChanged FINAL)
        Q_PROPERTY(int sampleSize READ sampleSize WRITE setSampleSize NOTIFY sampleSizeChanged FINAL)
        Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)
        Q_PROPERTY(qint64 sampledDocuments READ sampledDocuments NOTIFY stateChanged FINAL)
        Q_PROPERTY(int count READ rowCount NOTIFY stateChanged FINAL)

    public:
        enum Roles {
            PathRole = Qt::UserRole + 1,
            NameRole,
            DepthRole,
            PresenceRole,
            PresentCountRole,
            TypesRole,
            ItemTypesRole,
            ValuesRole,
            MinValueRole,
            MaxValueRole
        };

        explicit SchemaAnalyzer(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString serverUuid() const { return _serverUuid; }
        QString database() const { return _database; }
        QString collection() const { return _collection; }
        int sampleSize() const { return _sampleSize; }
        void setSampleSize(int size);
        bool busy() const { return _busy; }
        QString error() const { return _error; }
        qint64 sampledDocuments() const { return _result.sampledDocuments; }

        /// Targets a connected server's collection and starts an analysis.
        Q_INVOKABLE bool configure(const QString &serverUuid, const QString &database,
                                   const QString &collection);
        Q_INVOKABLE void analyze();

    Q_SIGNALS:
        void targetChanged();
        void sampleSizeChanged();
        void stateChanged();

    private:
        void onJobFinished(quint64 ticket, const SchemaJobOutcome &outcome);

        QString _serverUuid;
        QString _database;
        QString _collection;
        std::shared_ptr<ConnectionSettings> _connection;
        int _sampleSize = 1000;
        bool _busy = false;
        QString _error;
        quint64 _ticket = 0;   ///< the SchemaService job this analyzer waits for
        SchemaResult _result;
    };
}
