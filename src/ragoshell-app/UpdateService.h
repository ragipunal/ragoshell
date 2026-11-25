#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/utils/UpdatePulse.h"
#include "ragoshell-app/QmlSingleton.h"

class QNetworkReply;

namespace RagoShell
{
    class LanguageManager;
    class PlatformInfo;
    class QmlBackend;
    class SettingsManager;

    /**
     * @brief Unobtrusive Welcome-page update check, mandatory update gate,
     *        and optional anonymous usage pulse (docs/UpdateAndTelemetry.md).
     *
     * Automatic checks run silently on the Welcome page only when internet
     * connectivity is available, never showing error popups when offline.
     */
    class UpdateService : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(UpdateService)
        QML_SINGLETON

        Q_PROPERTY(bool checking READ checking NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool storeBuild READ storeBuild CONSTANT FINAL)
        Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool mandatory READ mandatory NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool bannerVisible READ bannerVisible NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString minSupportedVersion READ minSupportedVersion NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString publishedAt READ publishedAt NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString downloadUrl READ downloadUrl NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString downloadLabel READ downloadLabel NOTIFY stateChanged FINAL)
        Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY stateChanged FINAL)
        Q_PROPERTY(bool linkCopied READ linkCopied NOTIFY stateChanged FINAL)

    public:
        UpdateService(SettingsManager *settings,
                      PlatformInfo *platform,
                      LanguageManager *language,
                      bool headlessRun,
                      QObject *parent = nullptr);

        static UpdateService *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<UpdateService>::get();
        }

        void attachBackend(QmlBackend *backend);
        void recordShellOpened();
        void recordQueryExecuted();
        void flushSessionMetrics();

        bool checking() const { return _checking; }
        bool storeBuild() const;
        bool updateAvailable() const { return _updateAvailable; }
        bool mandatory() const { return _mandatory; }
        bool bannerVisible() const;
        QString latestVersion() const { return _latestVersion; }
        QString minSupportedVersion() const { return _minSupportedVersion; }
        QString publishedAt() const { return _publishedAt; }
        QString releaseNotes() const { return _releaseNotes; }
        QString downloadUrl() const { return _downloadUrl; }
        QString downloadLabel() const { return _downloadLabel; }
        QString statusMessage() const { return _statusMessage; }
        bool statusIsError() const { return _statusIsError; }
        bool linkCopied() const { return _linkCopied; }

        /// Triggered silently when WelcomePage is shown.
        Q_INVOKABLE void onWelcomeShown();
        /// Triggered explicitly by the user from AboutDialog.
        Q_INVOKABLE void checkNow();
        /// Hides the optional Welcome card for the current latestVersion.
        Q_INVOKABLE void dismissOptionalUpdate();
        /// Opens the validated platform-specific https:// download URL.
        Q_INVOKABLE bool openDownload();
        /// Copies the download URL to the system clipboard.
        Q_INVOKABLE void copyDownloadUrl();
        /// Populates synthetic state for --open-view update-banner / mandatory-update.
        Q_INVOKABLE void previewDebugUpdate(bool mandatoryMode);

    public Q_SLOTS:
        void retranslate();

    Q_SIGNALS:
        void stateChanged();
        void checkCompleted(bool ok, bool updateAvailable, bool mandatory);

    private:
        enum class StatusKind
        {
            None,
            Checking,
            UpToDate,
            Available,
            Disabled,
            PolicyDisabled,
            CustomServerSuppressed,
            Offline,
            Failed,
            SignatureFailed
        };

        QString resolveActiveEndpoint(bool manual,
                                      bool *suppressedByPolicyOut = nullptr,
                                      bool *suppressedByServerOut = nullptr) const;
        bool isOnlineForEndpoint(const QString &endpoint) const;
        bool shareAnonymousUsageEnabled() const;
        UpdatePulse::TelemetrySnapshot buildSnapshot() const;
        void startCheck(bool manual);
        void handleReplyFinished(QNetworkReply *reply, const QString &endpoint, bool manual);
        void fetchAndVerifySignedManifest(const QString &endpoint, const QString &pubKey, bool manual);
        void applyVerifiedResult(const UpdatePulse::CheckResult &res, const QByteArray &responseBytes);
        void failCheck(bool manual, StatusKind kind);
        void restoreCachedState();
        void persistCachedResult(const UpdatePulse::CheckResult &res);
        void updateStatusText();

        SettingsManager *_settings = nullptr;
        PlatformInfo *_platform = nullptr;
        LanguageManager *_language = nullptr;
        QNetworkAccessManager _nam;
        QPointer<QNetworkReply> _activeReply;
        QPointer<QNetworkReply> _manifestReply;
        QPointer<QNetworkReply> _minisigReply;
        QElapsedTimer _sessionTimer;
        QTimer _copiedResetTimer;

        bool _headlessRun = false;
        bool _welcomeCheckedThisSession = false;
        bool _pendingOnlineCheck = false;
        bool _checking = false;
        bool _updateAvailable = false;
        bool _mandatory = false;
        bool _statusIsError = false;
        bool _linkCopied = false;
        int _debugPreviewMode = 0; // 0 = none, 1 = optional, 2 = mandatory
        StatusKind _statusKind = StatusKind::None;

        QString _latestVersion;
        QString _minSupportedVersion;
        QString _publishedAt;
        QString _releaseNotes;
        QString _downloadUrl;
        QString _downloadLabel;
        QString _dismissedVersion;
        QString _statusMessage;
        QByteArray _lastResponseBytes;
    };
}
