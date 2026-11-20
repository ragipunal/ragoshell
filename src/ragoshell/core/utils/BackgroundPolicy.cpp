#include "ragoshell/core/utils/BackgroundPolicy.h"

namespace RagoShell
{
    namespace BackgroundPolicy
    {
        bool allowed(Qt::ApplicationState state, bool metered, bool unmeteredOnly, bool mobile)
        {
            if (state == Qt::ApplicationSuspended)
                return false;
            if (mobile && state != Qt::ApplicationActive)
                return false;
            if (unmeteredOnly && metered)
                return false;
            return true;
        }

        bool sleptBetweenTicks(long long gapMs, long long intervalMs)
        {
            return gapMs > 4 * intervalMs && gapMs > 30000;
        }
    }
}
