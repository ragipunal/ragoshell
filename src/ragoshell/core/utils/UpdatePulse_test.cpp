#include "gtest/gtest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/UpdatePulse.h"

#include <openssl/evp.h>

#include <array>
#include <memory>

using namespace RagoShell::UpdatePulse;

TEST(UpdatePulseTest, ComparesSemanticVersionsAndPreReleases)
{
    EXPECT_EQ(compareVersions(QStringLiteral("2.0.0"), QStringLiteral("2.0.0")), 0);
    EXPECT_EQ(compareVersions(QStringLiteral("v2.0.0"), QStringLiteral("2.0.0")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("2.0.0"), QStringLiteral("2.0.1")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("2.0.9"), QStringLiteral("2.1.0")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("2.2.0"), QStringLiteral("2.10.0")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("2.1.0-rc.1"), QStringLiteral("2.1.0")), 0);
    EXPECT_GT(compareVersions(QStringLiteral("3.0.0"), QStringLiteral("2.99.99")), 0);
}

TEST(UpdatePulseTest, ComparesPreReleasesBySemVerPrecedence)
{
    // numeric identifiers compare numerically, not as text
    EXPECT_GT(compareVersions(QStringLiteral("2.1.0-rc.10"), QStringLiteral("2.1.0-rc.2")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("2.1.0-rc.2"), QStringLiteral("2.1.0-rc.10")), 0);
    EXPECT_GT(compareVersions(QStringLiteral("2.1.0"), QStringLiteral("2.1.0-rc.10")), 0);
    EXPECT_EQ(compareVersions(QStringLiteral("2.1.0-rc.10"), QStringLiteral("v2.1.0-rc.10")), 0);
    // numbers wider than int must not overflow
    EXPECT_GT(compareVersions(QStringLiteral("2.1.0-rc.99999999999999999999"), QStringLiteral("2.1.0-rc.9")), 0);
    // numeric < alphanumeric
    EXPECT_LT(compareVersions(QStringLiteral("2.1.0-1"), QStringLiteral("2.1.0-alpha")), 0);
    EXPECT_GT(compareVersions(QStringLiteral("2.1.0-alpha"), QStringLiteral("2.1.0-1")), 0);
    // hyphens stay inside the pre-release
    EXPECT_GT(compareVersions(QStringLiteral("2.1.0-rc-2"), QStringLiteral("2.1.0-rc-1")), 0);

    // the example chain of SemVer 2.0.0 section 11, each strictly lower than the next
    const QStringList chain = {
        QStringLiteral("1.0.0-alpha"), QStringLiteral("1.0.0-alpha.1"), QStringLiteral("1.0.0-alpha.beta"),
        QStringLiteral("1.0.0-beta"),  QStringLiteral("1.0.0-beta.2"),  QStringLiteral("1.0.0-beta.11"),
        QStringLiteral("1.0.0-rc.1"),  QStringLiteral("1.0.0"),
    };
    for (qsizetype i = 0; i + 1 < chain.size(); ++i) {
        EXPECT_LT(compareVersions(chain.at(i), chain.at(i + 1)), 0) << qPrintable(chain.at(i)) << " < " << qPrintable(chain.at(i + 1));
        EXPECT_GT(compareVersions(chain.at(i + 1), chain.at(i)), 0) << qPrintable(chain.at(i + 1)) << " > " << qPrintable(chain.at(i));
    }
}

TEST(UpdatePulseTest, HashesInstallIdAndOmitsUsageWhenDisabled)
{
    TelemetrySnapshot snap;
    snap.appVersion = QStringLiteral("2.0.0");
    snap.platform = QStringLiteral("macos");
    snap.arch = QStringLiteral("arm64");
    snap.formFactor = QStringLiteral("desktop");
    snap.osVersion = QStringLiteral("15.4");
    snap.uiLanguage = QStringLiteral("tr");
    snap.anonymousId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
    snap.launchCount = 12;
    snap.savedConnections = 3;
    snap.sshConnections = 1;
    snap.queriesExecuted = 48;
    snap.dbVersions = {QStringLiteral("7.0.14-ent"), QStringLiteral("6.0.5"), QStringLiteral("7.0.2")};

    const QByteArray withStats = buildRequestPayload(snap, true);
    const QJsonObject objWith = QJsonDocument::fromJson(withStats).object();
    EXPECT_EQ(objWith.value(QStringLiteral("platform")).toString(), QStringLiteral("macos"));
    ASSERT_TRUE(objWith.contains(QStringLiteral("usage")));
    const QJsonObject usage = objWith.value(QStringLiteral("usage")).toObject();
    const QString installId = usage.value(QStringLiteral("installId")).toString();
    EXPECT_EQ(installId.size(), 32);
    EXPECT_NE(installId, snap.anonymousId);
    EXPECT_FALSE(QString::fromUtf8(withStats).contains(snap.anonymousId));

    const QJsonArray dbs = usage.value(QStringLiteral("dbVersions")).toArray();
    ASSERT_EQ(dbs.size(), 2);
    EXPECT_EQ(dbs.at(0).toString(), QStringLiteral("6.0"));
    EXPECT_EQ(dbs.at(1).toString(), QStringLiteral("7.0"));

    const QByteArray withoutStats = buildRequestPayload(snap, false);
    const QJsonObject objWithout = QJsonDocument::fromJson(withoutStats).object();
    EXPECT_FALSE(objWithout.contains(QStringLiteral("usage")));
}

TEST(UpdatePulseTest, ParsesPlatformDownloadLocalizedNotesAndMandatoryThreshold)
{
    const QByteArray response = R"json({
        "latestVersion": "2.2.0",
        "minSupportedVersion": "2.1.0",
        "publishedAt": "2026-10-30",
        "releaseNotes": {
            "en": "Critical TLS fix and MongoDB 8.0 improvements.",
            "tr": "Kritik TLS düzeltmesi ve MongoDB 8.0 iyileştirmeleri."
        },
        "allDownloadsUrl": "https://github.com/ragipunal/ragoshell/releases/tag/v2.2.0",
        "downloads": {
            "macos": {
                "url": "https://github.com/ragipunal/ragoshell/releases/download/v2.2.0/RagoShell-macOS-arm64.dmg",
                "label": { "tr": "macOS İndir (.dmg)", "en": "Download for macOS (.dmg)" }
            },
            "ios": {
                "url": "https://apps.apple.com/app/id0000000000",
                "label": "App Store"
            }
        }
    })json";

    // 2.0.0 < minSupportedVersion (2.1.0) -> mandatory = true!
    const CheckResult oldClient = parseCheckResponse(
        response, QStringLiteral("2.0.0"), QStringLiteral("macos"), QStringLiteral("tr"));
    ASSERT_TRUE(oldClient.ok);
    EXPECT_TRUE(oldClient.updateAvailable);
    EXPECT_TRUE(oldClient.mandatory);
    EXPECT_EQ(oldClient.releaseNotes, QStringLiteral("Kritik TLS düzeltmesi ve MongoDB 8.0 iyileştirmeleri."));
    EXPECT_EQ(oldClient.downloadLabel, QStringLiteral("macOS İndir (.dmg)"));
    EXPECT_EQ(oldClient.downloadUrl,
              QStringLiteral("https://github.com/ragipunal/ragoshell/releases/download/v2.2.0/RagoShell-macOS-arm64.dmg"));

    // 2.1.5 >= minSupportedVersion (2.1.0), < latestVersion (2.2.0) -> optional update!
    const CheckResult midClient = parseCheckResponse(
        response, QStringLiteral("2.1.5"), QStringLiteral("ios"), QStringLiteral("en"));
    ASSERT_TRUE(midClient.ok);
    EXPECT_TRUE(midClient.updateAvailable);
    EXPECT_FALSE(midClient.mandatory);
    EXPECT_EQ(midClient.downloadUrl, QStringLiteral("https://apps.apple.com/app/id0000000000"));
    EXPECT_EQ(midClient.downloadLabel, QStringLiteral("App Store"));

    // 2.2.0 == latestVersion -> up to date, never mandatory even if server sets mandatory=true
    const CheckResult currentClient = parseCheckResponse(
        response, QStringLiteral("2.2.0"), QStringLiteral("macos"), QStringLiteral("tr"));
    ASSERT_TRUE(currentClient.ok);
    EXPECT_FALSE(currentClient.updateAvailable);
    EXPECT_FALSE(currentClient.mandatory);
}

TEST(UpdatePulseTest, RejectsUnsafeDownloadSchemesAndFallsBackToHttpsReleasesPage)
{
    const QByteArray malicious = R"json({
        "latestVersion": "2.1.0",
        "downloadUrl": "javascript:alert(1)",
        "allDownloadsUrl": "file:///etc/passwd",
        "downloads": {
            "windows": { "url": "http://insecure.example.com/setup.exe" }
        }
    })json";

    const CheckResult res = parseCheckResponse(
        malicious, QStringLiteral("2.0.0"), QStringLiteral("windows"), QStringLiteral("en"));
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(res.updateAvailable);
    EXPECT_EQ(res.downloadUrl, defaultReleasesUrl());
    EXPECT_EQ(res.allDownloadsUrl, defaultReleasesUrl());

    EXPECT_FALSE(isSafeDownloadUrl(QStringLiteral("http://127.0.0.1:8787/test.dmg")));
    EXPECT_FALSE(isSafeDownloadUrl(QStringLiteral("https://user:pass@example.com/a.dmg")));
    EXPECT_TRUE(isSafeEndpointUrl(QStringLiteral("http://127.0.0.1:8787/v1/release-check")));
    EXPECT_FALSE(isSafeEndpointUrl(QStringLiteral("http://example.com/v1/release-check")));
}

TEST(UpdatePulseTest, InstallHashIsSaltedWithTheProductName)
{
    // SHA-256("<id>:ragoshell-pulse-v1"), first 32 hex digits: the server counts installs by this value.
    EXPECT_EQ(deriveInstallHash(QStringLiteral("550e8400-e29b-41d4-a716-446655440000")),
              QStringLiteral("7d0af1ad773c76262f51324036f0b3ca"));
    EXPECT_EQ(deriveInstallHash(QStringLiteral("  550e8400-e29b-41d4-a716-446655440000 ")),
              QStringLiteral("7d0af1ad773c76262f51324036f0b3ca"));
    EXPECT_TRUE(deriveInstallHash(QStringLiteral("   ")).isEmpty());
}

TEST(UpdatePulseTest, EndpointDefaultsToTheProductApiAndAcceptsOnlySafeOverrides)
{
    const QByteArray saved = qgetenv("RAGOSHELL_UPDATE_URL");
    const bool wasSet = qEnvironmentVariableIsSet("RAGOSHELL_UPDATE_URL");
    qunsetenv("RAGOSHELL_UPDATE_URL");

    EXPECT_EQ(defaultReleasesUrl(), QStringLiteral("https://dl.ragoshell.com.tr/releases/"));
    const QString fallback = QStringLiteral("https://api.ragoshell.com.tr/v1/release-check");
    EXPECT_EQ(endpointUrl(), fallback);
    EXPECT_EQ(endpointUrl(QStringLiteral("http://127.0.0.1:8787/v1/release-check")),
              QStringLiteral("http://127.0.0.1:8787/v1/release-check"));
    EXPECT_EQ(endpointUrl(QStringLiteral("http://example.com/v1/release-check")), fallback);

    qputenv("RAGOSHELL_UPDATE_URL", "http://localhost:9/v1/release-check");
    EXPECT_EQ(endpointUrl(), QStringLiteral("http://localhost:9/v1/release-check"));

    if (wasSet)
        qputenv("RAGOSHELL_UPDATE_URL", saved);
    else
        qunsetenv("RAGOSHELL_UPDATE_URL");
}

TEST(UpdatePulseTest, NoServiceOrABrokenAnswerIsNotAnUpdate)
{
    // What the client sees when the service is missing: an empty body, an HTML error page, or JSON without a version.
    for (const QByteArray &body : {QByteArray(), QByteArray("<html>502 Bad Gateway</html>"),
                                   QByteArray("{}"), QByteArray(R"({"latestVersion": ""})")}) {
        const CheckResult res = parseCheckResponse(
            body, QStringLiteral("2.1.0"), QStringLiteral("windows"), QStringLiteral("en"));
        EXPECT_FALSE(res.ok) << body.constData();
        EXPECT_FALSE(res.updateAvailable) << body.constData();
        EXPECT_FALSE(res.mandatory) << body.constData();
        EXPECT_FALSE(res.error.isEmpty()) << body.constData();
    }
}

TEST(UpdatePulseTest, VerifiesSignedManifestAndBlocksTamperedOrUnsignedMandatoryUpdate)
{
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), &EVP_PKEY_free);
    ASSERT_NE(pkey, nullptr);

    std::array<unsigned char, 8> keyId{{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80}};
    std::array<unsigned char, 32> rawPub{};
    std::size_t pubLen = rawPub.size();
    ASSERT_EQ(EVP_PKEY_get_raw_public_key(pkey.get(), rawPub.data(), &pubLen), 1);

    QByteArray pubStruct;
    pubStruct.append("Ed", 2);
    pubStruct.append(reinterpret_cast<const char *>(keyId.data()), 8);
    pubStruct.append(reinterpret_cast<const char *>(rawPub.data()), 32);
    const QString pubKeyB64 = QString::fromLatin1(pubStruct.toBase64());

    const QByteArray manifest = R"json({
        "schemaVersion": 1,
        "latestVersion": "2.3.0",
        "minSupportedVersion": "2.2.0",
        "mandatory": true,
        "publishedAt": "2026-10-30T12:00:00Z",
        "allDownloadsUrl": "https://dl.ragoshell.com.tr/releases/",
        "releaseNotes": { "en": "Signed security update." },
        "downloads": {
            "macos": {
                "url": "https://dl.ragoshell.com.tr/releases/2.3.0/ragoshell-2.3.0-macos-arm64.dmg",
                "label": "macOS"
            }
        }
    })json";

    unsigned char digest[64]{};
    unsigned int digestLen = 0;
    ASSERT_EQ(EVP_Digest(manifest.constData(),
                         static_cast<std::size_t>(manifest.size()),
                         digest,
                         &digestLen,
                         EVP_blake2b512(),
                         nullptr),
              1);

    auto signRaw = [&](const QByteArray &payload) {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get());
        QByteArray out(64, Qt::Uninitialized);
        std::size_t sigLen = 64;
        EVP_DigestSign(ctx.get(),
                       reinterpret_cast<unsigned char *>(out.data()),
                       &sigLen,
                       reinterpret_cast<const unsigned char *>(payload.constData()),
                       static_cast<std::size_t>(payload.size()));
        return out;
    };

    const QByteArray rawSig = signRaw(QByteArray(reinterpret_cast<const char *>(digest), 64));
    QByteArray sigStruct;
    sigStruct.append("ED", 2);
    sigStruct.append(reinterpret_cast<const char *>(keyId.data()), 8);
    sigStruct.append(rawSig);

    const QString comment = QStringLiteral("release:2.3.0\ttimestamp:2026-10-30T12:00:00Z\tfile:releases.json");
    QByteArray globalPayload = rawSig;
    globalPayload.append(comment.toUtf8());
    const QByteArray globalSig = signRaw(globalPayload);

    const QString minisig = QStringLiteral("untrusted comment: release sig\n%1\ntrusted comment: %2\n%3\n")
                                .arg(QString::fromLatin1(sigStruct.toBase64()),
                                     comment,
                                     QString::fromLatin1(globalSig.toBase64()));

    const CheckResult verified = parseAndVerifySignedManifest(
        manifest, minisig, pubKeyB64, QStringLiteral("2.1.0"), QStringLiteral("macos"), QStringLiteral("en"));
    ASSERT_TRUE(verified.ok) << verified.error.toStdString();
    EXPECT_TRUE(verified.updateAvailable);
    EXPECT_TRUE(verified.mandatory);
    EXPECT_EQ(verified.latestVersion, QStringLiteral("2.3.0"));
    EXPECT_GT(verified.manifestTimestampSec, 0);

    // Anti-rollback: replaying an older valid manifest when minTimestampSec is newer must fail!
    const CheckResult rolledBack = parseAndVerifySignedManifest(
        manifest,
        minisig,
        pubKeyB64,
        QStringLiteral("2.1.0"),
        QStringLiteral("macos"),
        QStringLiteral("en"),
        verified.manifestTimestampSec + 1);
    EXPECT_FALSE(rolledBack.ok);

    // Tampered manifest URL or mandatory flag must be rejected!
    QByteArray tampered = manifest;
    tampered.replace("dl.ragoshell.com.tr/releases/2.3.0", "evil.example.com/releases/2.3.0");
    const CheckResult rejected = parseAndVerifySignedManifest(
        tampered, minisig, pubKeyB64, QStringLiteral("2.1.0"), QStringLiteral("macos"), QStringLiteral("en"));
    EXPECT_FALSE(rejected.ok);
    EXPECT_FALSE(rejected.updateAvailable);
    EXPECT_FALSE(rejected.mandatory);
}

TEST(UpdatePulseTest, IsolatesCustomServerUpdatesAndEnforcesLockedPolicy)
{
    const QByteArray savedUpd = qgetenv("RAGOSHELL_UPDATE_URL");
    const bool hadUpd = qEnvironmentVariableIsSet("RAGOSHELL_UPDATE_URL");
    const QByteArray savedMan = qgetenv("RAGOSHELL_RELEASES_MANIFEST_URL");
    const bool hadMan = qEnvironmentVariableIsSet("RAGOSHELL_RELEASES_MANIFEST_URL");
    qunsetenv("RAGOSHELL_UPDATE_URL");
    qunsetenv("RAGOSHELL_RELEASES_MANIFEST_URL");

    auto restoreEnv = [&]() {
        if (hadUpd)
            qputenv("RAGOSHELL_UPDATE_URL", savedUpd);
        else
            qunsetenv("RAGOSHELL_UPDATE_URL");
        if (hadMan)
            qputenv("RAGOSHELL_RELEASES_MANIFEST_URL", savedMan);
        else
            qunsetenv("RAGOSHELL_RELEASES_MANIFEST_URL");
    };

    // Custom server active without policy/env updateUrl suppresses default SaaS release-check.
    EXPECT_TRUE(resolveActiveUpdateEndpoint(false, true, true, QString(), false).isEmpty());

    // Invalid policy suppresses all update checks regardless of flags or env vars.
    qputenv("RAGOSHELL_UPDATE_URL", "https://updates.env.example/v1/release-check");
    EXPECT_TRUE(resolveActiveUpdateEndpoint(true, true, false,
                                            QStringLiteral("https://updates.corp.example/v1/release-check"),
                                            true).isEmpty());

    // Unlocked policy yields to explicit RAGOSHELL_UPDATE_URL.
    EXPECT_EQ(resolveActiveUpdateEndpoint(false, true, true,
                                          QStringLiteral("https://updates.corp.example/v1/release-check"),
                                          false),
              QStringLiteral("https://updates.env.example/v1/release-check"));

    // Locked policy ignores RAGOSHELL_UPDATE_URL both when policyUrl is set AND when empty!
    EXPECT_EQ(resolveActiveUpdateEndpoint(false, true, true,
                                          QStringLiteral("https://updates.corp.example/v1/release-check"),
                                          true),
              QStringLiteral("https://updates.corp.example/v1/release-check"));
    EXPECT_TRUE(resolveActiveUpdateEndpoint(false, true, true, QString(), true).isEmpty());

    // Custom HTTPS update endpoint derives its own /releases.json rather than BrandInfo::releasesManifestUrl.
    EXPECT_EQ(releasesManifestUrl(QString(), QStringLiteral("https://updates.corp.example:8443/v1/release-check"), true),
              QStringLiteral("https://updates.corp.example:8443/releases.json"));

    // Locked policy also ignores RAGOSHELL_RELEASES_MANIFEST_URL.
    qputenv("RAGOSHELL_RELEASES_MANIFEST_URL", "https://evil.example/releases.json");
    EXPECT_EQ(releasesManifestUrl(QString(), QStringLiteral("https://updates.corp.example/v1/release-check"), true),
              QStringLiteral("https://updates.corp.example/releases.json"));
    // Unlocked policy allows RAGOSHELL_RELEASES_MANIFEST_URL.
    EXPECT_EQ(releasesManifestUrl(QString(), QStringLiteral("https://updates.corp.example/v1/release-check"), false),
              QStringLiteral("https://evil.example/releases.json"));

    // Default SaaS API endpoint keeps BrandInfo::releasesManifestUrl.
    qunsetenv("RAGOSHELL_RELEASES_MANIFEST_URL");
    const QString defaultUpdateEp = QString::fromUtf8(RagoShell::BrandInfo::apiUrl) + QStringLiteral("/v1/release-check");
    const QString defaultManifest = QString::fromUtf8(RagoShell::BrandInfo::releasesManifestUrl);
    EXPECT_EQ(releasesManifestUrl(QString(), defaultUpdateEp, false), defaultManifest);

    // Endpoint URL validation rejects '@' authority confusion and query/fragment strings.
    EXPECT_FALSE(isSafeEndpointUrl(QStringLiteral("https://@updates.corp.example/v1/release-check")));
    EXPECT_FALSE(isSafeEndpointUrl(QStringLiteral("https://updates.corp.example/releases.json?v=1")));
    EXPECT_FALSE(isSafeEndpointUrl(QStringLiteral("https://updates.corp.example/releases.json#frag")));

    restoreEnv();
}
