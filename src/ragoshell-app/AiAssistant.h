#pragma once

#include <memory>

#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/utils/AiQueryAssistant.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace RagoShell
{
    class ConnectionSettings;
    struct SchemaJobOutcome;

    /**
     * @brief QML "AiAssistant": natural language → one read-only query for a
     * collection. The collection's schema comes from the schema catalog when
     * it has one (refreshed in the background once stale), otherwise the
     * SchemaService samples it and stores it there (schemaText shows exactly what will
     * be sent); Generate then calls the selected provider's
     * API. When a model is out of quota, rate limited, overloaded or retired,
     * the next model of the same provider is tried (Preferences › "try
     * another model") and notices say so; models that failed are skipped
     * until their quota or rate limit clears.
     */
    class AiAssistant : public QObject
    {
        Q_OBJECT
        QML_ELEMENT

        Q_PROPERTY(QString serverUuid READ serverUuid NOTIFY targetChanged FINAL)
        Q_PROPERTY(QString database READ database NOTIFY targetChanged FINAL)
        Q_PROPERTY(QString collection READ collection NOTIFY targetChanged FINAL)
        /// Enabled in Preferences and an API key is available.
        Q_PROPERTY(bool ready READ ready NOTIFY readyChanged FINAL)
        Q_PROPERTY(bool sampling READ sampling NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString schemaText READ schemaText NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool retryable READ retryable NOTIFY stateChanged FINAL)
        /// The error is about the key, the model or the quota (Preferences help).
        Q_PROPERTY(bool settingsError READ settingsError NOTIFY stateChanged FINAL)
        /// Epoch ms before which Retry will hit the same limit again; 0 = now.
        Q_PROPERTY(qint64 retryAtMs READ retryAtMs NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString script READ script NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString explanation READ explanation NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool writesData READ writesData NOTIFY stateChanged FINAL)
        /// Selected provider id / display name and model (Preferences).
        Q_PROPERTY(QString provider READ provider NOTIFY readyChanged FINAL)
        Q_PROPERTY(QString providerName READ providerName NOTIFY readyChanged FINAL)
        Q_PROPERTY(QString model READ model NOTIFY readyChanged FINAL)
        /// Model being asked or that gave the current answer.
        Q_PROPERTY(QString activeModel READ activeModel NOTIFY stateChanged FINAL)
        /// Warnings about skipped / failed models for the current request.
        Q_PROPERTY(QStringList notices READ notices NOTIFY stateChanged FINAL)
        /// Another provider with a key that may still work after a quota
        /// error ("" when none); asking it is the user's choice.
        Q_PROPERTY(QString alternativeProvider READ alternativeProvider NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString alternativeProviderName READ alternativeProviderName NOTIFY stateChanged FINAL)
        /// Collections of the database with a schema in the catalog (this
        /// one included), and how many the database has (-1 = not listed).
        Q_PROPERTY(int analyzedCollections READ analyzedCollections NOTIFY stateChanged FINAL)
        Q_PROPERTY(int databaseCollections READ databaseCollections NOTIFY stateChanged FINAL)

    public:
        explicit AiAssistant(QObject *parent = nullptr);
        ~AiAssistant() override;

        QString serverUuid() const { return _serverUuid; }
        QString database() const { return _database; }
        QString collection() const { return _collection; }
        bool ready() const;
        bool sampling() const { return _sampling; }
        bool busy() const { return _sampling || _reply; }
        QString schemaText() const;
        QString error() const { return _error; }
        bool retryable() const { return _retryable; }
        bool settingsError() const;
        qint64 retryAtMs() const { return _retryAtMs; }
        QString script() const { return _suggestion.ok ? _suggestion.script : QString(); }
        QString explanation() const { return _suggestion.explanation; }
        bool writesData() const { return _suggestion.writesData; }
        QString provider() const;
        QString providerName() const;
        QString model() const;
        QString activeModel() const { return _activeModel; }
        QStringList notices() const { return _notices; }
        QString alternativeProvider() const { return _alternative; }
        QString alternativeProviderName() const;
        int analyzedCollections() const;
        int databaseCollections() const;

        /// Targets a connected server's collection and samples its schema.
        Q_INVOKABLE bool configure(const QString &serverUuid, const QString &database,
                                   const QString &collection);
        /// Re-reads Preferences (enabled, provider, key, model, sample values).
        Q_INVOKABLE void refreshSettings();
        /// Sends the question; samples first when that has not finished yet.
        Q_INVOKABLE void ask(const QString &question);
        /// Like ask(), but first forgets the provider's failed models.
        Q_INVOKABLE void retry(const QString &question);
        /// Asks once with another provider (alternativeProvider) without
        /// changing Preferences.
        Q_INVOKABLE void askWithProvider(const QString &question, const QString &provider);
        Q_INVOKABLE void cancel();
        /// Queues the database's other collections for analysis, so they can
        /// be described for $lookup (the schema collector's explicit request).
        Q_INVOKABLE void analyzeDatabase();

    Q_SIGNALS:
        void targetChanged();
        void readyChanged();
        void stateChanged();

    private:
        /// The other analyzed collections of the database, ranked for the
        /// current question (docs/SchemaIntelligence.md).
        void refreshRelated();
        void onSnapshotReady(const QString &connectionId, const QString &database, const QString &collection);
        /// A SchemaService job for the target collection: interactive (the
        /// dialog waits) or a quiet refresh of a stale catalog snapshot.
        quint64 enqueueSample(bool interactive);
        void sample();
        void onJobFinished(quint64 ticket, const SchemaJobOutcome &outcome);
        /// quiet: a background refresh of a schema already in use (no busy
        /// state, failures ignored).
        void applySample(const SchemaResult &result, bool quiet = false);
        bool useCatalog();
        void setSchema(const SchemaResult &result);
        void start(const QString &question, AiQueryAssistant::Provider provider);
        void send();
        void fail(const QString &error, bool retryable);
        void onFinished(QNetworkReply *reply);
        QString apiKey(AiQueryAssistant::Provider provider) const;
        class CloudClient *cloudClient() const;
        QString modelFor(AiQueryAssistant::Provider provider) const;
        AiQueryAssistant::Provider selectedProvider() const;
        QString findAlternative() const;

        QString _serverUuid;
        QString _database;
        QString _collection;
        std::shared_ptr<ConnectionSettings> _connection;
        AiQueryAssistant::Context _context;
        bool _schemaReady = false;
        bool _schemaHasValues = false;   ///< the schema in use kept its values
        bool _sampling = false;
        quint64 _sampleTicket = 0;    ///< SchemaService job the dialog waits for
        quint64 _refreshTicket = 0;   ///< quiet refresh of a stale catalog schema
        QString _pendingQuestion;

        // The request in flight: its provider, the models it may try and
        // those already tried.
        AiQueryAssistant::Provider _activeProvider = AiQueryAssistant::Provider::Anthropic;
        QString _question;
        QStringList _candidates;
        QStringList _tried;
        QString _activeModel;

        QNetworkAccessManager *_network = nullptr;
        QPointer<QNetworkReply> _reply;
        QString _error;
        bool _retryable = false;
        qint64 _retryAtMs = 0;
        QStringList _notices;
        QString _alternative;
        AiQueryAssistant::Suggestion _suggestion;
    };

    /**
     * @brief QML "AiModelCatalog": fetches a provider's current model list
     * (Preferences › Refresh) and keeps it in the settings, since providers
     * add and retire models often.
     */
    class AiModelCatalog : public QObject
    {
        Q_OBJECT
        QML_ELEMENT

        Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)

    public:
        explicit AiModelCatalog(QObject *parent = nullptr);
        ~AiModelCatalog() override;

        bool busy() const;
        QString error() const { return _error; }

        /// An empty key uses the provider's environment variable.
        Q_INVOKABLE void refresh(const QString &provider, const QString &apiKey);

    Q_SIGNALS:
        void stateChanged();
        /// The list was stored; aiModelsFor(provider) now returns it.
        void refreshed(const QString &provider, const QStringList &models);

    private:
        QNetworkAccessManager *_network = nullptr;
        QPointer<QNetworkReply> _reply;
        QString _error;
    };
}
