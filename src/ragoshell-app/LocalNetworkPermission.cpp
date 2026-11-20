#include "ragoshell-app/LocalNetworkPermission.h"

#include <memory>

#include <QDebug>
#include <QDir>
#include <QPointer>
#include <QTimer>

#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/LocalNetwork.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace RagoShell
{
    namespace
    {
#ifdef Q_OS_ANDROID
        constexpr int kLastApiWithLegacyStoragePermission = 28;
        constexpr int kFirstApiWithLocalNetworkPermission = 37;
        constexpr int kPollMilliseconds = 400;
        constexpr int kGiveUpMilliseconds = 120000;

        const QString &localNetworkPermissionName()
        {
            static const QString name = QStringLiteral("android.permission.ACCESS_LOCAL_NETWORK");
            return name;
        }

        bool androidPermissionGranted(const QString &perm)
        {
            const QJniObject context = QNativeInterface::QAndroidApplication::context();
            if (!context.isValid())
                return true;
            const QJniObject name = QJniObject::fromString(perm);
            const jint result = context.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
                                                         name.object<jstring>());
            return result == 0;   // PackageManager.PERMISSION_GRANTED
        }

        void requestAndroidPermission(const QString &perm, jint requestCode)
        {
            QNativeInterface::QAndroidApplication::runOnAndroidMainThread([perm, requestCode] {
                const QJniObject activity = QNativeInterface::QAndroidApplication::context();
                if (!activity.isValid())
                    return;
                QJniEnvironment env;
                jclass stringClass = env->FindClass("java/lang/String");
                const QJniObject name = QJniObject::fromString(perm);
                jobjectArray names = env->NewObjectArray(1, stringClass, name.object<jstring>());
                activity.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V",
                                          names, requestCode);
                env->DeleteLocalRef(names);
            });
        }
#endif
    }

    void whenLocalNetworkAllowed(const QStringList &hosts, std::function<void()> proceed)
    {
#ifdef Q_OS_ANDROID
        const bool needed = QNativeInterface::QAndroidApplication::sdkVersion()
                            >= kFirstApiWithLocalNetworkPermission;
        if (needed && anyLocalNetworkHost(hosts)
            && !androidPermissionGranted(localNetworkPermissionName())) {
            qInfo().noquote() << QStringLiteral("Asking for the local network permission (Android %1+) to reach: %2")
                             .arg(kFirstApiWithLocalNetworkPermission).arg(hosts.join(QLatin1String(", ")));
            requestAndroidPermission(localNetworkPermissionName(), 7301);

            // The answer arrives on the Java side; poll until it is there (or the user ignores the dialog).
            auto *timer = new QTimer(QCoreApplication::instance());
            auto waited = std::make_shared<int>(0);
            QObject::connect(timer, &QTimer::timeout, timer, [timer, waited, proceed] {
                *waited += kPollMilliseconds;
                const bool granted = androidPermissionGranted(localNetworkPermissionName());
                if (!granted && *waited < kGiveUpMilliseconds)
                    return;
                if (!granted)
                    qWarning().noquote() << QStringLiteral("The local network permission was not granted; connections to "
                                                "local addresses will time out.");
                timer->deleteLater();
                proceed();
            });
            timer->start(kPollMilliseconds);
            return;
        }
#else
        Q_UNUSED(hosts)
#endif
        proceed();
    }

    void ensureLegacyStoragePermission(const QString &pathOrUri, bool writeAccess)
    {
#ifdef Q_OS_ANDROID
        if (QNativeInterface::QAndroidApplication::sdkVersion() > kLastApiWithLegacyStoragePermission)
            return;
        const QString trimmed = pathOrUri.trimmed();
        if (trimmed.isEmpty()
            || trimmed.startsWith(QLatin1String("content://"), Qt::CaseInsensitive)
            || trimmed.startsWith(QLatin1String(":/"))
            || trimmed.startsWith(QLatin1String("qrc:"), Qt::CaseInsensitive)) {
            return;
        }
        const QString cleanTarget = QDir::cleanPath(trimmed);
        const QString cleanDataDir = QDir::cleanPath(appDataDir());
        if (cleanTarget == cleanDataDir
            || cleanTarget.startsWith(cleanDataDir + QLatin1Char('/'))) {
            return;
        }
        const QString perm = writeAccess
            ? QStringLiteral("android.permission.WRITE_EXTERNAL_STORAGE")
            : QStringLiteral("android.permission.READ_EXTERNAL_STORAGE");
        if (!androidPermissionGranted(perm))
            requestAndroidPermission(perm, writeAccess ? 7302 : 7303);
#else
        Q_UNUSED(pathOrUri)
        Q_UNUSED(writeAccess)
#endif
    }
}
