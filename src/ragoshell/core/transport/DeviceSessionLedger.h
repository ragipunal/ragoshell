#pragma once

#include <QHash>
#include <QString>

namespace RagoShell
{
    /// Per-principal concurrent-session accounting for the hosted web
    /// gateway (docs/HostedGateway.md §2/H4). One account must never be
    /// able to hold every global session slot open at once.
    ///
    /// Single-threaded by design: all GatewaySession objects live on the
    /// listener's event-loop thread, so no locking is needed.
    class DeviceSessionLedger
    {
    public:
        explicit DeviceSessionLedger(int maxPerKey) : _max(qMax(1, maxPerKey)) {}

        bool tryAcquire(const QString &key)
        {
            if (key.isEmpty())
                return false;
            const int n = _counts.value(key);
            if (n >= _max)
                return false;
            _counts.insert(key, n + 1);
            return true;
        }

        void release(const QString &key)
        {
            const int n = _counts.value(key);
            if (n <= 1)
                _counts.remove(key);
            else
                _counts.insert(key, n - 1);
        }

        int count(const QString &key) const { return _counts.value(key); }
        int maxPerKey() const { return _max; }

    private:
        int _max;
        QHash<QString, int> _counts;
    };
}
