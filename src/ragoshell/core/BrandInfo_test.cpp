#include "gtest/gtest.h"

#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/utils/Minisign.h"
#include "ragoshell/core/utils/UpdatePulse.h"

using namespace RagoShell;

TEST(BrandInfoTest, ExposesConsistentSingleSourceIdentifiers)
{
    const QString name = QString::fromUtf8(BrandInfo::productName);
    const QString id = QString::fromUtf8(BrandInfo::productId);
    const QString bundle = QString::fromUtf8(BrandInfo::bundleId);
    const QString dom = QString::fromUtf8(BrandInfo::domain);

    ASSERT_FALSE(name.isEmpty());
    ASSERT_FALSE(id.isEmpty());
    ASSERT_FALSE(bundle.isEmpty());
    ASSERT_FALSE(dom.isEmpty());

    EXPECT_EQ(name.toLower(), id);
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]*$")).match(id).hasMatch());
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[a-z][a-z0-9_]*(\\.[a-z][a-z0-9_]*)+$")).match(bundle).hasMatch());
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[a-z0-9-]+(\\.[a-z0-9-]+)+$")).match(dom).hasMatch());
}

TEST(BrandInfoTest, ProductAndDonateUrlsAreStrictHttpsWithoutCredentialsOrPlaceholders)
{
    const QString dom = QString::fromUtf8(BrandInfo::domain);
    const QString prodUrl = QString::fromUtf8(BrandInfo::productUrl);
    const QString donUrl = QString::fromUtf8(BrandInfo::donateUrl);

    EXPECT_TRUE(UpdatePulse::isSafeDownloadUrl(prodUrl)) << prodUrl.toStdString();
    EXPECT_TRUE(UpdatePulse::isSafeDownloadUrl(donUrl)) << donUrl.toStdString();

    const QUrl parsedProd(prodUrl, QUrl::StrictMode);
    const QUrl parsedDon(donUrl, QUrl::StrictMode);
    ASSERT_TRUE(parsedProd.isValid());
    ASSERT_TRUE(parsedDon.isValid());

    EXPECT_EQ(parsedProd.scheme(), QStringLiteral("https"));
    EXPECT_EQ(parsedProd.host(), dom);
    EXPECT_TRUE(parsedProd.userInfo().isEmpty());

    EXPECT_EQ(parsedDon.scheme(), QStringLiteral("https"));
    EXPECT_FALSE(parsedDon.host().isEmpty());
    EXPECT_TRUE(parsedDon.userInfo().isEmpty());
    EXPECT_FALSE(donUrl.contains(QStringLiteral("example.com"), Qt::CaseInsensitive));
    EXPECT_FALSE(donUrl.contains(QStringLiteral("PENDING"), Qt::CaseInsensitive));
    EXPECT_FALSE(donUrl.contains(QStringLiteral("TODO"), Qt::CaseInsensitive));
}

TEST(BrandInfoTest, EmbeddedEntitlementPubkeysSlotIsEmptyOrValidP256Ring)
{
    bool ok = false;
    QString err;
    const EntitlementVerifier verifier = EntitlementVerifier::fromEmbeddedDefaults(&ok, &err);
    EXPECT_TRUE(ok) << err.toStdString();

    const QString rawRing = QString::fromUtf8(BrandInfo::entitlementPubkeys).trimmed();
    if (rawRing.isEmpty())
    {
        EXPECT_EQ(verifier.keyCount(), 0);
    }
    else
    {
        EXPECT_GT(verifier.keyCount(), 0);
    }
}

TEST(BrandInfoTest, CloudLegalSourceAndManifestUrlsFollowDomainAndStrictHttps)
{
    const QString dom = QString::fromUtf8(BrandInfo::domain);
    const QString apiUrl = QString::fromUtf8(BrandInfo::apiUrl);
    const QString portalUrl = QString::fromUtf8(BrandInfo::portalUrl);
    const QString manifestUrl = QString::fromUtf8(BrandInfo::releasesManifestUrl);
    const QString privacyUrl = QString::fromUtf8(BrandInfo::privacyUrl);
    const QString termsUrl = QString::fromUtf8(BrandInfo::termsUrl);
    const QString sourceRepoUrl = QString::fromUtf8(BrandInfo::sourceRepoUrl);

    for (const QString &u : {apiUrl, portalUrl, manifestUrl, privacyUrl, termsUrl, sourceRepoUrl})
    {
        EXPECT_TRUE(UpdatePulse::isSafeDownloadUrl(u)) << u.toStdString();
    }

    EXPECT_EQ(QUrl(apiUrl).host(), QStringLiteral("api.") + dom);
    EXPECT_EQ(QUrl(portalUrl).host(), QStringLiteral("app.") + dom);
    EXPECT_EQ(QUrl(manifestUrl).host(), QStringLiteral("dl.") + dom);
    EXPECT_EQ(QUrl(privacyUrl).host(), dom);
    EXPECT_EQ(QUrl(termsUrl).host(), dom);

    const QString rawMinisign = QString::fromUtf8(BrandInfo::minisignPubkey).trimmed();
    if (!rawMinisign.isEmpty())
    {
        Minisign::PublicKey parsed;
        QString err;
        EXPECT_TRUE(Minisign::parsePublicKey(rawMinisign, &parsed, &err)) << err.toStdString();
    }
}
