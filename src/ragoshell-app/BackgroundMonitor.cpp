#include "ragoshell-app/BackgroundMonitor.h"

#include <QDateTime>
#include <QGuiApplication>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/BackgroundPolicy.h"

namespace RagoShell
{
    BackgroundMonitor::BackgroundMonitor(QObject *parent)
        : QObject(parent)
    {
        // Metered / reachability come from the platform's network backend; without one
        // the network counts as unmetered and always there.
        if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered))
            QNetworkInformation::loadDefaultBackend();
        if (QNetworkInformation *info = QNetworkInformation::instance()) {
            connect(info, &QNetworkInformation::isMeteredChanged, this, [this]() { recompute(); });
            connect(info, &QNetworkInformation::reachabilityChanged, this,
                    [this](QNetworkInformation::Reachability reachability) {
                const bool offline = reachability == QNetworkInformation::Reachability::Disconnected;
                if (_wasOffline && !offline)
                    emit connectionsMayHaveBroken();
                _wasOffline = offline;
            });
            _wasOffline = info->reachability() == QNetworkInformation::Reachability::Disconnected;
        }
        // Coming back after the system suspended the app (mobile): its sockets may be gone.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationSuspended)
                _wasSuspended = true;
            else if (state == Qt::ApplicationActive && _wasSuspended) {
                _wasSuspended = false;
                emit connectionsMayHaveBroken();
            }
            recompute();
        });

        _lastTickMs = QDateTime::currentMSecsSinceEpoch();
        _timer.setInterval(kTickMs);
        connect(&_timer, &QTimer::timeout, this, &BackgroundMonitor::tick);
        _timer.start();
        recompute();
    }

    bool BackgroundMonitor::metered() const
    {
        const QNetworkInformation *info = QNetworkInformation::instance();
        return info && info->supports(QNetworkInformation::Feature::Metered) && info->isMetered();
    }

    void BackgroundMonitor::tick()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();   // wall clock: it keeps counting while asleep
        const qint64 gap = now - _lastTickMs;
        _lastTickMs = now;
        if (BackgroundPolicy::sleptBetweenTicks(gap, kTickMs))
            emit connectionsMayHaveBroken();
        recompute();                                             // also picks up a changed setting
    }

    void BackgroundMonitor::recompute()
    {
        const SettingsManager *settings = AppRegistry::instance().settingsManager();
        const bool allowed = BackgroundPolicy::allowed(
            qGuiApp->applicationState(), metered(), !settings || settings->schemaUnmeteredOnly(),
            BackgroundPolicy::isMobilePlatform());
        if (allowed != _allowed) {
            _allowed = allowed;
            emit backgroundAllowedChanged(allowed);
        }
    }
}
