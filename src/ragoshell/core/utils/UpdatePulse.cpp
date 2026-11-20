#include "ragoshell/core/utils/UpdatePulse.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/Minisign.h"

namespace RagoShell::UpdatePulse
{
    namespace
    {
        struct ParsedSemVer
        {
            int major = 0;
            int minor = 0;
            int patch = 0;
            QString prerelease;
            bool valid = false;
        };

        ParsedSemVer parseSemVer(const QString &raw)
        {
            ParsedSemVer out;
            QString clean = raw.trimmed();
            if (clean.startsWith(QLatin1Char('v')) || clean.startsWith(QLatin1Char('V')))
                clean.remove(0, 1);

            const int plusIdx = clean.indexOf(QLatin1Char('+'));
            if (plusIdx >= 0)
                clean.truncate(plusIdx);

            const int dashIdx = clean.indexOf(QLatin1Char('-'));
            if (dashIdx >= 0) {
                out.prerelease = clean.mid(dashIdx + 1).trimmed();
                clean.truncate(dashIdx);
            }

            const QStringList nums = clean.split(QLatin1Char('.'));
            if (nums.isEmpty() || nums.size() > 4)
                return out;

            bool ok0 = false;
            out.major = nums.value(0).toInt(&ok0);
            if (!ok0 || out.major < 0)
                return out;

            bool ok1 = true;
            out.minor = nums.size() >= 2 ? nums.at(1).toInt(&ok1) : 0;
            bool ok2 = true;
            out.patch = nums.size() >= 3 ? nums.at(2).toInt(&ok2) : 0;
            if (!ok1 || !ok2 || out.minor < 0 || out.patch < 0)
                return out;

            out.valid = true;
            return out;
        }

        QString pickLocalizedText(const QJsonValue &val, const QString &uiLanguage)
        {
            if (val.isString())
                return val.toString().trimmed();
            if (!val.isObject())
                return QString();

            const QJsonObject map = val.toObject();
            const QString normLang = uiLanguage.trimmed();
            if (!normLang.isEmpty()) {
                if (map.contains(normLang))
                    return map.value(normLang).toString().trimmed();
                QString hyphenated = normLang;
                hyphenated.replace(QLatin1Char('_'), QLatin1Char('-'));
                if (map.contains(hyphenated))
                    return map.value(hyphenated).toString().trimmed();
                const QString base = normLang.section(QRegularExpression(QStringLiteral("[_-]")), 0, 0);
                if (!base.isEmpty() && map.contains(base))
                    return map.value(base).toString().trimmed();
            }
            if (map.contains(QLatin1String("en")))
                return map.value(QLatin1String("en")).toString().trimmed();
            return map.isEmpty() ? QString() : map.begin().value().toString().trimmed();
        }

        bool isNumericIdentifier(const QString &id)
        {
            if (id.isEmpty())
                return false;
            for (const QChar ch : id) {
                if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
                    return false;
            }
            return true;
        }

        // Numeric identifiers of any length, without converting (no overflow): fewer significant digits is smaller.
        int compareNumericIdentifiers(const QString &a, const QString &b)
        {
            qsizetype ia = 0;
            qsizetype ib = 0;
            while (ia < a.size() - 1 && a.at(ia) == QLatin1Char('0'))
                ++ia;
            while (ib < b.size() - 1 && b.at(ib) == QLatin1Char('0'))
                ++ib;
            const qsizetype lenA = a.size() - ia;
            const qsizetype lenB = b.size() - ib;
            if (lenA != lenB)
                return lenA < lenB ? -1 : 1;
            const int cmp = QStringView(a).mid(ia).compare(QStringView(b).mid(ib));
            return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
        }

        // SemVer 2.0.0 §11.4: dot-separated identifiers from left to right; numeric ones compare numerically,
        // alphanumeric ones in ASCII order, a numeric identifier is lower than an alphanumeric one, and when all
        // identifiers so far are equal the shorter set is lower (1.0.0-alpha < 1.0.0-alpha.1).
        int comparePrerelease(const QString &a, const QString &b)
        {
            const QStringList aIds = a.split(QLatin1Char('.'));
            const QStringList bIds = b.split(QLatin1Char('.'));
            const qsizetype count = std::min(aIds.size(), bIds.size());
            for (qsizetype i = 0; i < count; ++i) {
                const QString &x = aIds.at(i);
                const QString &y = bIds.at(i);
                const bool xNum = isNumericIdentifier(x);
                const bool yNum = isNumericIdentifier(y);
                int cmp = 0;
                if (xNum && yNum)
                    cmp = compareNumericIdentifiers(x, y);
                else if (xNum != yNum)
                    cmp = xNum ? -1 : 1;
                else
                    cmp = x.compare(y, Qt::CaseSensitive);
                if (cmp != 0)
                    return cmp < 0 ? -1 : 1;
            }
            if (aIds.size() != bIds.size())
                return aIds.size() < bIds.size() ? -1 : 1;
            return 0;
        }
    }

    int compareVersions(const QString &lhs, const QString &rhs)
    {
        const ParsedSemVer a = parseSemVer(lhs);
        const ParsedSemVer b = parseSemVer(rhs);
        if (!a.valid && !b.valid)
            return 0;
        if (!a.valid)
            return -1;
        if (!b.valid)
            return 1;

        if (a.major != b.major)
            return a.major < b.major ? -1 : 1;
        if (a.minor != b.minor)
            return a.minor < b.minor ? -1 : 1;
        if (a.patch != b.patch)
            return a.patch < b.patch ? -1 : 1;

        // Release > pre-release (e.g. 2.1.0 > 2.1.0-rc.1)
        if (a.prerelease.isEmpty() && !b.prerelease.isEmpty())
            return 1;
        if (!a.prerelease.isEmpty() && b.prerelease.isEmpty())
            return -1;
        if (a.prerelease == b.prerelease)
            return 0;
        return comparePrerelease(a.prerelease, b.prerelease);
    }

    QString deriveInstallHash(const QString &anonymousId)
    {
        const QString trimmed = anonymousId.trimmed();
        if (trimmed.isEmpty())
            return QString();
        const QByteArray material =
            QStringLiteral("%1:%2-pulse-v1")
                .arg(trimmed, QString::fromUtf8(BrandInfo::productId))
                .toUtf8();
        return QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(32));
    }

    QStringList sanitizeDbVersions(const QStringList &rawVersions)
    {
        static const QRegularExpression re(QStringLiteral("^(\\d+)\\.(\\d+)"));
        QSet<QString> unique;
        for (const QString &raw : rawVersions) {
            const QRegularExpressionMatch m = re.match(raw.trimmed());
            if (m.hasMatch())
                unique.insert(QStringLiteral("%1.%2").arg(m.captured(1), m.captured(2)));
        }
        QStringList out = unique.values();
        std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
            return compareVersions(a, b) < 0;
        });
        if (out.size() > 16)
            out = out.mid(0, 16);
        return out;
    }

    bool isSafeDownloadUrl(const QString &urlText)
    {
        const QString trimmed = urlText.trimmed();
        if (trimmed.isEmpty() || trimmed.size() > 1024)
            return false;
        const QUrl url(trimmed, QUrl::StrictMode);
        if (!url.isValid() || !url.userInfo().isEmpty())
            return false;
        return url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
               && !url.host().isEmpty();
    }

    bool isSafeEndpointUrl(const QString &urlText)
    {
        const QString trimmed = urlText.trimmed();
        if (trimmed.isEmpty() || trimmed.size() > 1024)
            return false;
        const QUrl url(trimmed, QUrl::StrictMode);
        if (!url.isValid() || !url.userInfo().isEmpty() || url.host().isEmpty())
            return false;
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("https"))
            return true;
        if (scheme == QLatin1String("http")) {
            const QString host = url.host().toLower();
            return host == QLatin1String("127.0.0.1")
                   || host == QLatin1String("localhost")
                   || host == QLatin1String("::1");
        }
        return false;
    }

    QString endpointUrl(const QString &overrideUrl)
    {
        const QString candidate = !overrideUrl.trimmed().isEmpty()
            ? overrideUrl.trimmed()
            : productEnv("UPDATE_URL").trimmed();
        if (!candidate.isEmpty() && isSafeEndpointUrl(candidate))
            return candidate;
        return QString::fromUtf8(BrandInfo::apiUrl) + QStringLiteral("/v1/release-check");
    }

    QString releasesManifestUrl(const QString &overrideUrl, const QString &activeEndpointUrl)
    {
        const QString candidate = !overrideUrl.trimmed().isEmpty()
            ? overrideUrl.trimmed()
            : productEnv("RELEASES_MANIFEST_URL").trimmed();
        if (!candidate.isEmpty() && isSafeEndpointUrl(candidate))
            return candidate;

        if (!activeEndpointUrl.trimmed().isEmpty() && isSafeEndpointUrl(activeEndpointUrl)) {
            const QUrl ep(activeEndpointUrl.trimmed(), QUrl::StrictMode);
            if (ep.path().endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
                return activeEndpointUrl.trimmed();
            const QString host = ep.host().toLower();
            if (host == QLatin1String("127.0.0.1")
                || host == QLatin1String("localhost")
                || host == QLatin1String("::1")) {
                QUrl localManifest = ep;
                localManifest.setPath(QStringLiteral("/releases.json"));
                localManifest.setQuery(QString());
                localManifest.setFragment(QString());
                return localManifest.toString();
            }
        }

        return QString::fromUtf8(BrandInfo::releasesManifestUrl);
    }

    QString defaultReleasesUrl()
    {
        return QStringLiteral("https://dl.%1/releases/").arg(QString::fromUtf8(BrandInfo::domain));
    }

    QString defaultPlatformLabel(const QString &platformId)
    {
        const QString p = platformId.trimmed().toLower();
        if (p == QLatin1String("windows"))
            return QStringLiteral("Windows");
        if (p == QLatin1String("macos"))
            return QStringLiteral("macOS");
        if (p == QLatin1String("linux"))
            return QStringLiteral("Linux");
        if (p == QLatin1String("android"))
            return QStringLiteral("Google Play");
        if (p == QLatin1String("ios"))
            return QStringLiteral("App Store");
        return QString::fromUtf8(BrandInfo::productName);
    }

    QByteArray buildRequestPayload(const TelemetrySnapshot &snapshot, bool includeUsageStats)
    {
        QJsonObject root;
        root.insert(QStringLiteral("schemaVersion"), 1);
        root.insert(QStringLiteral("appVersion"), snapshot.appVersion.trimmed().left(32));
        root.insert(QStringLiteral("platform"), snapshot.platform.trimmed().toLower().left(16));
        root.insert(QStringLiteral("arch"), snapshot.arch.trimmed().toLower().left(24));
        root.insert(QStringLiteral("formFactor"), snapshot.formFactor.trimmed().toLower().left(16));
        root.insert(QStringLiteral("osVersion"), snapshot.osVersion.trimmed().left(32));
        root.insert(QStringLiteral("uiLanguage"), snapshot.uiLanguage.trimmed().left(16));

        if (includeUsageStats) {
            QJsonObject usage;
            const QString installHash = deriveInstallHash(snapshot.anonymousId);
            if (!installHash.isEmpty())
                usage.insert(QStringLiteral("installId"), installHash);
            usage.insert(QStringLiteral("launchCount"), std::max(0, snapshot.launchCount));
            usage.insert(QStringLiteral("lastSessionSec"), std::max(0, snapshot.lastSessionSec));
            usage.insert(QStringLiteral("savedConnections"), std::max(0, snapshot.savedConnections));
            usage.insert(QStringLiteral("sshConnections"), std::max(0, snapshot.sshConnections));
            usage.insert(QStringLiteral("tlsConnections"), std::max(0, snapshot.tlsConnections));
            usage.insert(QStringLiteral("replicaSetConnections"), std::max(0, snapshot.replicaSetConnections));
            usage.insert(QStringLiteral("shellsOpened"), std::max(0, snapshot.shellsOpened));
            usage.insert(QStringLiteral("queriesExecuted"), std::max(0, snapshot.queriesExecuted));
            usage.insert(QStringLiteral("aiEnabled"), snapshot.aiEnabled);
            if (!snapshot.uiTheme.trimmed().isEmpty())
                usage.insert(QStringLiteral("uiTheme"), snapshot.uiTheme.trimmed().left(16));
            if (!snapshot.uiDensity.trimmed().isEmpty())
                usage.insert(QStringLiteral("uiDensity"), snapshot.uiDensity.trimmed().left(16));

            QJsonArray dbArr;
            for (const QString &ver : sanitizeDbVersions(snapshot.dbVersions))
                dbArr.append(ver);
            usage.insert(QStringLiteral("dbVersions"), dbArr);

            root.insert(QStringLiteral("usage"), usage);
        }

        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    CheckResult parseCheckResponse(const QByteArray &jsonBytes,
                                   const QString &currentAppVersion,
                                   const QString &platformId,
                                   const QString &uiLanguage)
    {
        CheckResult res;
        if (jsonBytes.size() > 65536) {
            res.error = QStringLiteral("Update response exceeds maximum allowed size.");
            return res;
        }

        QJsonParseError parseErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            res.error = QStringLiteral("Invalid update response JSON.");
            return res;
        }

        const QJsonObject obj = doc.object();
        const QString latest = obj.value(QStringLiteral("latestVersion")).toString().trimmed();
        if (latest.isEmpty() || compareVersions(latest, QStringLiteral("0.0.1")) < 0) {
            res.error = QStringLiteral("Missing or invalid latestVersion.");
            return res;
        }

        res.ok = true;
        res.latestVersion = latest;
        res.minSupportedVersion = obj.value(QStringLiteral("minSupportedVersion")).toString().trimmed();
        res.publishedAt = obj.value(QStringLiteral("publishedAt")).toString().trimmed().left(32);
        res.releaseNotes = pickLocalizedText(obj.value(QStringLiteral("releaseNotes")), uiLanguage).left(2000);

        const bool newerReleaseExists = compareVersions(currentAppVersion, res.latestVersion) < 0;
        const bool belowMinSupported = !res.minSupportedVersion.isEmpty()
                                       && compareVersions(currentAppVersion, res.minSupportedVersion) < 0;
        const bool serverFlaggedMandatory = obj.value(QStringLiteral("mandatory")).toBool(false);

        res.updateAvailable = newerReleaseExists || belowMinSupported;
        res.mandatory = res.updateAvailable && (belowMinSupported || serverFlaggedMandatory);

        const QString allUrl = obj.value(QStringLiteral("allDownloadsUrl")).toString().trimmed();
        res.allDownloadsUrl = isSafeDownloadUrl(allUrl) ? allUrl : defaultReleasesUrl();

        const QString normPlatform = platformId.trimmed().toLower();
        QString candidateUrl;
        QString candidateLabel;

        const QJsonObject downloads = obj.value(QStringLiteral("downloads")).toObject();
        bool platHasMandatory = false;
        bool platMandatoryVal = false;
        if (downloads.contains(normPlatform) && downloads.value(normPlatform).isObject()) {
            const QJsonObject platObj = downloads.value(normPlatform).toObject();
            candidateUrl = platObj.value(QStringLiteral("url")).toString().trimmed();
            candidateLabel = pickLocalizedText(platObj.value(QStringLiteral("label")), uiLanguage);

            const QString platLatest = platObj.value(QStringLiteral("latestVersion")).toString().trimmed();
            if (!platLatest.isEmpty() && compareVersions(platLatest, QStringLiteral("0.0.1")) >= 0)
                res.latestVersion = platLatest;

            const QString platMin = platObj.value(QStringLiteral("minSupportedVersion")).toString().trimmed();
            if (!platMin.isEmpty() && compareVersions(platMin, QStringLiteral("0.0.1")) >= 0)
                res.minSupportedVersion = platMin;

            if (platObj.value(QStringLiteral("mandatory")).isBool()) {
                platHasMandatory = true;
                platMandatoryVal = platObj.value(QStringLiteral("mandatory")).toBool();
            }
        }

        const bool newerReleaseExists2 = compareVersions(currentAppVersion, res.latestVersion) < 0;
        const bool belowMinSupported2 = !res.minSupportedVersion.isEmpty()
                                        && compareVersions(currentAppVersion, res.minSupportedVersion) < 0;
        const bool effectiveMandatoryFlag = platHasMandatory ? platMandatoryVal : serverFlaggedMandatory;
        res.updateAvailable = newerReleaseExists2 || belowMinSupported2;
        res.mandatory = res.updateAvailable && (belowMinSupported2 || effectiveMandatoryFlag);

        if (candidateUrl.isEmpty())
            candidateUrl = obj.value(QStringLiteral("downloadUrl")).toString().trimmed();
        if (candidateLabel.isEmpty())
            candidateLabel = pickLocalizedText(obj.value(QStringLiteral("downloadLabel")), uiLanguage);

        res.downloadUrl = isSafeDownloadUrl(candidateUrl) ? candidateUrl : res.allDownloadsUrl;
        res.downloadLabel = candidateLabel.isEmpty()
            ? defaultPlatformLabel(normPlatform)
            : candidateLabel.left(64);
        return res;
    }

    CheckResult parseAndVerifySignedManifest(const QByteArray &manifestBytes,
                                             const QString &minisigText,
                                             const QString &pubKeyText,
                                             const QString &currentAppVersion,
                                             const QString &platformId,
                                             const QString &uiLanguage,
                                             qint64 minTimestampSec)
    {
        CheckResult failed;
        if (pubKeyText.trimmed().isEmpty()) {
            failed.error = QStringLiteral("Minisign public key is empty.");
            return failed;
        }
        if (manifestBytes.isEmpty() || manifestBytes.size() > 65536) {
            failed.error = QStringLiteral("Signed manifest is empty or exceeds maximum allowed size.");
            return failed;
        }

        QJsonParseError jsonErr{};
        const QJsonDocument doc = QJsonDocument::fromJson(manifestBytes, &jsonErr);
        if (jsonErr.error != QJsonParseError::NoError || !doc.isObject()) {
            failed.error = QStringLiteral("Signed manifest is not a valid JSON object.");
            return failed;
        }
        const QString topLevelVersion = doc.object().value(QStringLiteral("latestVersion")).toString().trimmed();
        if (topLevelVersion.isEmpty()) {
            failed.error = QStringLiteral("Signed manifest is missing top-level latestVersion.");
            return failed;
        }

        Minisign::VerifyOptions verifyOpts;
        verifyOpts.allowLegacyUnprehashed = false;
        verifyOpts.requireReleaseClaims = true;
        verifyOpts.checkClockSkew = true;
        verifyOpts.minTimestampSec = minTimestampSec;
        verifyOpts.expectedFile = QStringLiteral("releases.json");
        verifyOpts.expectedRelease = topLevelVersion;

        const Minisign::VerifyResult sigRes = Minisign::verify(
            manifestBytes, minisigText, pubKeyText, verifyOpts);
        if (!sigRes.ok) {
            failed.error = sigRes.error;
            return failed;
        }

        CheckResult parsed = parseCheckResponse(manifestBytes, currentAppVersion, platformId, uiLanguage);
        if (!parsed.ok)
            return parsed;
        if (compareVersions(parsed.latestVersion, topLevelVersion) > 0) {
            failed.error = QStringLiteral("Signed manifest platform latestVersion \"%1\" exceeds signed release \"%2\".")
                               .arg(parsed.latestVersion, topLevelVersion);
            return failed;
        }
        parsed.manifestTimestampSec = sigRes.tokens.timestampEpochSec;
        return parsed;
    }
}
