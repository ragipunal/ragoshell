#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace RagoShell
{
    class ConnectionSettings;

    /**
     * @brief Native C++ mongocxx-backed live Server Status & Current Operations controller.
     *
     * Runs all MongoDB admin commands (`serverStatus`, `currentOp`, `killOp`) on a
     * dedicated background thread so network latency never blocks the QML UI thread,
     * with automatic `$ownOps` fallback when connected without `clusterMonitor` privileges.
     */
    class ServerMonitorController : public QObject
    {
        Q_OBJECT

        Q_PROPERTY(QString title READ title CONSTANT FINAL)
        Q_PROPERTY(QString serverUuid READ serverUuid CONSTANT FINAL)
        Q_PROPERTY(QString host READ host NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString version READ version NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString storageEngine READ storageEngine NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString role READ role NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString replSetName READ replSetName NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(qint64 uptimeSec READ uptimeSec NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString uptimeText READ uptimeText NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString lastUpdated READ lastUpdated NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
        Q_PROPERTY(QString errorText READ errorText NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(QString statusNote READ statusNote NOTIFY metricsUpdated FINAL)

        // Auto-refresh interval in ms (0 = Paused)
        Q_PROPERTY(int refreshIntervalMs READ refreshIntervalMs
                   WRITE setRefreshIntervalMs NOTIFY configChanged FINAL)
        // False while the page is not on screen: auto-refresh is suspended
        // (refreshNow() still polls once). Bound to the page's visibility.
        Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)

        // Live KPI metrics
        Q_PROPERTY(double opsTotalRate READ opsTotalRate NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double opsQueryRate READ opsQueryRate NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double opsInsertRate READ opsInsertRate NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double opsUpdateRate READ opsUpdateRate NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double opsDeleteRate READ opsDeleteRate NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double opsGetmoreRate READ opsGetmoreRate NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double opsCommandRate READ opsCommandRate NOTIFY metricsUpdated FINAL)

        Q_PROPERTY(int currentConnections READ currentConnections NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(int availableConnections READ availableConnections NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(qint64 totalCreatedConnections READ totalCreatedConnections NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(int activeClients READ activeClients NOTIFY metricsUpdated FINAL)

        Q_PROPERTY(double netInKbps READ netInKbps NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double netOutKbps READ netOutKbps NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double netRequestsRate READ netRequestsRate NOTIFY metricsUpdated FINAL)

        Q_PROPERTY(double residentMb READ residentMb NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double virtualMb READ virtualMb NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double wtCacheUsedMb READ wtCacheUsedMb NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double wtCacheMaxMb READ wtCacheMaxMb NOTIFY metricsUpdated FINAL)
        Q_PROPERTY(double wtCacheDirtyMb READ wtCacheDirtyMb NOTIFY metricsUpdated FINAL)

        // Rolling 60-point chart history
        Q_PROPERTY(QVariantList historySamples READ historySamples NOTIFY metricsUpdated FINAL)

        // Current Operations (`currentOp`) filters & rows
        Q_PROPERTY(bool activeOnly READ activeOnly WRITE setActiveOnly NOTIFY configChanged FINAL)
        Q_PROPERTY(bool hideSystemOps READ hideSystemOps WRITE setHideSystemOps NOTIFY configChanged FINAL)
        Q_PROPERTY(int minDurationMs READ minDurationMs WRITE setMinDurationMs NOTIFY configChanged FINAL)
        Q_PROPERTY(QString searchFilter READ searchFilter WRITE setSearchFilter NOTIFY configChanged FINAL)
        Q_PROPERTY(QVariantList currentOps READ currentOps NOTIFY operationsUpdated FINAL)
        Q_PROPERTY(int totalRawOpsCount READ totalRawOpsCount NOTIFY operationsUpdated FINAL)

    public:
        ServerMonitorController(const QString &serverUuid,
                                const QString &title,
                                std::unique_ptr<ConnectionSettings> connSettings,
                                QObject *parent = nullptr);
        ~ServerMonitorController() override;

        QString title() const { return _title; }
        QString serverUuid() const { return _serverUuid; }
        QString host() const { return _host; }
        QString version() const { return _version; }
        QString storageEngine() const { return _storageEngine; }
        QString role() const { return _role; }
        QString replSetName() const { return _replSetName; }
        qint64 uptimeSec() const { return _uptimeSec; }
        QString uptimeText() const { return _uptimeText; }
        QString lastUpdated() const { return _lastUpdated; }
        bool busy() const { return _busy; }
        QString errorText() const { return _errorText; }
        QString statusNote() const { return _statusNote; }

        int refreshIntervalMs() const { return _refreshIntervalMs.load(); }
        void setRefreshIntervalMs(int ms);

        double opsTotalRate() const { return _opsTotalRate; }
        double opsQueryRate() const { return _opsQueryRate; }
        double opsInsertRate() const { return _opsInsertRate; }
        double opsUpdateRate() const { return _opsUpdateRate; }
        double opsDeleteRate() const { return _opsDeleteRate; }
        double opsGetmoreRate() const { return _opsGetmoreRate; }
        double opsCommandRate() const { return _opsCommandRate; }

        int currentConnections() const { return _currentConnections; }
        int availableConnections() const { return _availableConnections; }
        qint64 totalCreatedConnections() const { return _totalCreatedConnections; }
        int activeClients() const { return _activeClients; }

        double netInKbps() const { return _netInKbps; }
        double netOutKbps() const { return _netOutKbps; }
        double netRequestsRate() const { return _netRequestsRate; }

        double residentMb() const { return _residentMb; }
        double virtualMb() const { return _virtualMb; }
        double wtCacheUsedMb() const { return _wtCacheUsedMb; }
        double wtCacheMaxMb() const { return _wtCacheMaxMb; }
        double wtCacheDirtyMb() const { return _wtCacheDirtyMb; }

        QVariantList historySamples() const { return _historySamples; }

        bool activeOnly() const { return _activeOnly; }
        void setActiveOnly(bool v);

        bool hideSystemOps() const { return _hideSystemOps; }
        void setHideSystemOps(bool v);

        int minDurationMs() const { return _minDurationMs; }
        void setMinDurationMs(int ms);

        QString searchFilter() const { return _searchFilter; }
        void setSearchFilter(const QString &q);

        QVariantList currentOps() const { return _filteredOps; }
        int totalRawOpsCount() const { return _rawOps.size(); }

        bool active() const { return _active.load(); }
        void setActive(bool on);

        Q_INVOKABLE void refreshNow();
        Q_INVOKABLE void clearHistory();
        Q_INVOKABLE void killOperation(const QString &opidStr);
        Q_INVOKABLE void copyToClipboard(const QString &text) const;
        void updateLocalPortIfMatching(int oldPort, int newPort);

    Q_SIGNALS:
        void metricsUpdated();
        void operationsUpdated();
        void busyChanged();
        void configChanged();
        void activeChanged();
        void operationKilled(bool ok, const QString &message);

    private:
        struct RawCounters
        {
            bool valid = false;
            qint64 sampleMs = 0;
            qint64 insert = 0;
            qint64 query = 0;
            qint64 update = 0;
            qint64 del = 0;
            qint64 getmore = 0;
            qint64 command = 0;
            qint64 bytesIn = 0;
            qint64 bytesOut = 0;
            qint64 numRequests = 0;
        };

        void workerLoop();
        void applyFilteredOps();
        static QString formatUptime(qint64 secs);

        QString _serverUuid;
        QString _title;
        std::unique_ptr<ConnectionSettings> _connSettings;

        // Worker synchronization
        std::mutex _wakeMutex;
        std::condition_variable _wakeCv;
        std::atomic<bool> _stopFlag{false};
        std::atomic<bool> _forcePoll{true};
        std::atomic<bool> _resetClient{false};
        std::atomic<int> _refreshIntervalMs{2000};
        std::atomic<bool> _includeIdleOps{false};
        std::atomic<bool> _active{true};
        QString _pendingKillOpId;
        std::unique_ptr<std::thread> _workerThread;

        // UI-thread state
        bool _busy = false;
        QString _host;
        QString _version;
        QString _storageEngine;
        QString _role = QStringLiteral("Standalone");
        QString _replSetName;
        qint64 _uptimeSec = 0;
        QString _uptimeText = QStringLiteral("—");
        QString _lastUpdated;
        QString _errorText;
        QString _statusNote;

        double _opsTotalRate = 0.0;
        double _opsQueryRate = 0.0;
        double _opsInsertRate = 0.0;
        double _opsUpdateRate = 0.0;
        double _opsDeleteRate = 0.0;
        double _opsGetmoreRate = 0.0;
        double _opsCommandRate = 0.0;

        int _currentConnections = 0;
        int _availableConnections = 0;
        qint64 _totalCreatedConnections = 0;
        int _activeClients = 0;

        double _netInKbps = 0.0;
        double _netOutKbps = 0.0;
        double _netRequestsRate = 0.0;

        double _residentMb = 0.0;
        double _virtualMb = 0.0;
        double _wtCacheUsedMb = 0.0;
        double _wtCacheMaxMb = 0.0;
        double _wtCacheDirtyMb = 0.0;

        RawCounters _prevCounters;
        QVariantList _historySamples;

        bool _activeOnly = true;
        bool _hideSystemOps = true;
        int _minDurationMs = 0;
        QString _searchFilter;
        QVariantList _rawOps;
        QVariantList _filteredOps;
    };
}
