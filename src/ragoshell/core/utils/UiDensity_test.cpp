#include <gtest/gtest.h>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/utils/UiDensity.h"

using namespace RagoShell;

TEST(UiDensityTest, automatic_means_touch_only_on_tablets)
{
    EXPECT_TRUE(UiDensity::isTouch("Automatic", true));
    EXPECT_FALSE(UiDensity::isTouch("Automatic", false));
    EXPECT_TRUE(UiDensity::isTouch("Touch", false));       // chosen by the user, e.g. a touchscreen laptop
    EXPECT_FALSE(UiDensity::isTouch("Compact", true));     // a tablet user who wants a dense layout
    EXPECT_FALSE(UiDensity::isTouch("", false));           // unknown values count as Automatic
    EXPECT_TRUE(UiDensity::isTouch("nonsense", true));
}

TEST(UiDensityTest, a_touch_target_is_at_least_44)
{
    EXPECT_GE(UiDensity::kTouchTarget, 44);
}

TEST(UiDensityTest, the_size_table_has_a_column_per_form_factor)
{
    const auto desktop = UiDensity::sizes(false, false);
    EXPECT_EQ(desktop.control, 32);
    EXPECT_EQ(desktop.field, 34);
    // "Compact" on a tablet is the desktop column.
    EXPECT_EQ(UiDensity::sizes(false, true).field, desktop.field);

    const auto tablet = UiDensity::sizes(true, true);
    const auto phone = UiDensity::sizes(true, false);
    EXPECT_LT(tablet.field, phone.field);
    EXPECT_LT(tablet.control, phone.control);
    EXPECT_GT(tablet.field, desktop.field);
    EXPECT_GE(phone.control, UiDensity::kMinTouchBox);
    EXPECT_GE(phone.field, UiDensity::kMinTouchBox);
    EXPECT_GE(phone.menuItem, UiDensity::kMinTouchBox);
    // Minimalist 36 dp control box + standard 8 dp row spacing meets the 44 dp finger pitch.
    EXPECT_GE(phone.control + 8, UiDensity::kTouchTarget);
}

TEST(AuthMechanismsTest, the_connection_dialog_offers_scram_x509_and_plain_everywhere)
{
    const QStringList mechanisms = supportedAuthMechanisms();
    for (const char *m : { "SCRAM-SHA-1", "SCRAM-SHA-256", "MONGODB-X509", "PLAIN" })
        EXPECT_TRUE(mechanisms.contains(QLatin1String(m))) << m;
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    EXPECT_FALSE(mechanisms.contains(QLatin1String("GSSAPI")));    // no Kerberos library on phones and tablets
#else
    EXPECT_TRUE(mechanisms.contains(QLatin1String("GSSAPI")));
#endif
}
