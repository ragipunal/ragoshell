#pragma once

#include <QNetworkInformation>
#include <QObject>
#include <QTimer>

namespace RagoShell
{
    /**
     * @brief Watches what decides whether background work may run and whether
     *        connections may have broken.
     *
     *  - backgroundAllowedChanged(): BackgroundPolicy::allowed() from the application
     *    state, the network's "metered" flag and the user's setting (checked every
     *    few seconds too, so a changed setting takes effect without a restart).
     *  - connectionsMayHaveBroken(): the network came back after being gone, or the
     *    computer woke from sleep (a clock tick that came far too late).
     */
    class BackgroundMonitor : public QObject
    {
        Q_OBJECT

    public:
        explicit BackgroundMonitor(QObject *parent = nullptr);

        bool backgroundAllowed() const { return _allowed; }
        bool metered() const;

    Q_SIGNALS:
        void backgroundAllowedChanged(bool allowed);
        void connectionsMayHaveBroken();

    private:
        void tick();
        void recompute();

        QTimer _timer;
        qint64 _lastTickMs = 0;
        bool _allowed = true;
        bool _wasOffline = false;
        bool _wasSuspended = false;

        static constexpr int kTickMs = 5000;
    };
}
