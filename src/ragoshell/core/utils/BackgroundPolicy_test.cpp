#include <gtest/gtest.h>

#include <algorithm>

#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/BackgroundPolicy.h"

using namespace RagoShell;

TEST(BackgroundPolicyTest, desktop_keeps_working_without_focus_but_not_suspended)
{
    using BackgroundPolicy::allowed;
    EXPECT_TRUE(allowed(Qt::ApplicationActive, false, true, false));
    EXPECT_TRUE(allowed(Qt::ApplicationInactive, false, true, false));    // another window has focus
    EXPECT_TRUE(allowed(Qt::ApplicationHidden, false, true, false));      // minimized
    EXPECT_FALSE(allowed(Qt::ApplicationSuspended, false, true, false));
}

TEST(BackgroundPolicyTest, mobile_only_works_in_front)
{
    using BackgroundPolicy::allowed;
    EXPECT_TRUE(allowed(Qt::ApplicationActive, false, true, true));
    EXPECT_FALSE(allowed(Qt::ApplicationInactive, false, true, true));
    EXPECT_FALSE(allowed(Qt::ApplicationHidden, false, true, true));
    EXPECT_FALSE(allowed(Qt::ApplicationSuspended, false, true, true));
}

TEST(BackgroundPolicyTest, a_metered_network_waits_only_when_the_user_asked)
{
    using BackgroundPolicy::allowed;
    EXPECT_FALSE(allowed(Qt::ApplicationActive, true, true, false));
    EXPECT_TRUE(allowed(Qt::ApplicationActive, true, false, false));
    EXPECT_TRUE(allowed(Qt::ApplicationActive, false, true, false));
}

TEST(BackgroundPolicyTest, a_late_clock_tick_means_the_computer_slept)
{
    using BackgroundPolicy::sleptBetweenTicks;
    EXPECT_FALSE(sleptBetweenTicks(5100, 5000));        // normal jitter
    EXPECT_FALSE(sleptBetweenTicks(20000, 5000));       // a busy machine, not a sleep
    EXPECT_TRUE(sleptBetweenTicks(300000, 5000));       // five minutes
}

TEST(SettingsManagerLimitsTest, the_page_size_cap_is_one_thousand)
{
    EXPECT_EQ(SettingsManager::kMaxBatchSize, 1000);
}
