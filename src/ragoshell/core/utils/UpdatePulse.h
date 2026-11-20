#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace RagoShell::UpdatePulse
{
    /**
     * Aggregated, strictly non-personal metrics for the Welcome-page release
     * check and optional anonymous usage pulse (docs/UpdateAndTelemetry.md).
     * Never contains hostnames, database/collection names, queries, usernames,
     * credentials, or file paths.
     */
    struct TelemetrySnapshot
    {
        QString appVersion;
        QString platform;       ///< "windows", "macos", "linux", "android", "ios"
        QString arch;           ///< e.g. "arm64", "x86_64"
        QString formFactor;     ///< "desktop", "tablet", "phone"
        QString osVersion;      ///< e.g. "15.4", "14", "11"
        QString uiLanguage;     ///< active UI language code (e.g. "tr", "en")

        // Included only when shareAnonymousUsage is enabled:
        QString anonymousId;    ///< hashed with SHA-256 before leaving the device
        int launchCount = 0;
        int lastSessionSec = 0;
        int savedConnections = 0;
        int sshConnections = 0;
        int tlsConnections = 0;
        int replicaSetConnections = 0;
        int shellsOpened = 0;
        int queriesExecuted = 0;
        bool aiEnabled = false;
        QString uiTheme;
        QString uiDensity;
        QStringList dbVersions; ///< sanitized to "major.minor" before serialization
    };

    struct CheckResult
    {
        bool ok = false;
        QString error;
        bool updateAvailable = false;
        bool mandatory = false;
        QString latestVersion;
        QString minSupportedVersion;
        QString publishedAt;
        QString releaseNotes;
        QString downloadUrl;
        QString downloadLabel;
        QString allDownloadsUrl;
        qint64 manifestTimestampSec = 0;
    };

    /// Semantic version comparison (-1 if lhs < rhs, 0 if equal, +1 if lhs > rhs).
    int compareVersions(const QString &lhs, const QString &rhs);

    /// One-way salted SHA-256 hex digest (32 chars) of the local anonymous UUID.
    QString deriveInstallHash(const QString &anonymousId);

    /// Extracts unique sorted "major.minor" MongoDB versions, dropping custom build suffixes.
    QStringList sanitizeDbVersions(const QStringList &rawVersions);

    /// True only for well-formed https:// URLs without embedded credentials (<= 1024 chars).
    bool isSafeDownloadUrl(const QString &urlText);

    /// True for https:// endpoints, plus loopback http:// for local testing via RAGOSHELL_UPDATE_URL.
    bool isSafeEndpointUrl(const QString &urlText);

    /// Effective POST endpoint URL (override -> $RAGOSHELL_UPDATE_URL -> default HTTPS service).
    QString endpointUrl(const QString &overrideUrl = QString());

    /// Effective signed releases.json URL (override -> $RAGOSHELL_RELEASES_MANIFEST_URL -> loopback fallback -> BrandInfo::releasesManifestUrl).
    QString releasesManifestUrl(const QString &overrideUrl = QString(),
                                const QString &activeEndpointUrl = QString());

    /// Official fallback page when a platform-specific binary URL is omitted.
    QString defaultReleasesUrl();

    /// Human-readable platform target label ("Windows", "macOS", "Linux", "Google Play", "App Store").
    QString defaultPlatformLabel(const QString &platformId);

    /// Builds the compact UTF-8 JSON body for POST /v1/release-check.
    QByteArray buildRequestPayload(const TelemetrySnapshot &snapshot, bool includeUsageStats);

    /// Parses and validates the JSON response from POST /v1/release-check.
    CheckResult parseCheckResponse(const QByteArray &jsonBytes,
                                   const QString &currentAppVersion,
                                   const QString &platformId,
                                   const QString &uiLanguage);

    /// Cryptographically verifies raw releases.json bytes against detached .minisig text before parsing.
    CheckResult parseAndVerifySignedManifest(const QByteArray &manifestBytes,
                                             const QString &minisigText,
                                             const QString &pubKeyText,
                                             const QString &currentAppVersion,
                                             const QString &platformId,
                                             const QString &uiLanguage,
                                             qint64 minTimestampSec = 0);
}
