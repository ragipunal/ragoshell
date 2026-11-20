#pragma once

#include <Qt>

namespace RagoShell
{
    /// Rules for work nobody is waiting for (the background schema analysis).
    namespace BackgroundPolicy
    {
        /// May background work run now?
        ///  - never while the app is suspended
        ///  - on a phone or tablet only while the app is in front (the system may stop it any moment)
        ///  - not on a metered network when the user chose to wait for an unmetered one
        /// On a desktop an app that is not in front (another window has focus) keeps working.
        bool allowed(Qt::ApplicationState state, bool metered, bool unmeteredOnly, bool mobile);

        /// True on iOS and Android builds.
        constexpr bool isMobilePlatform()
        {
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
            return true;
#else
            return false;
#endif
        }

        /// A clock tick that came much later than its interval means the computer slept.
        bool sleptBetweenTicks(long long gapMs, long long intervalMs);
    }
}
