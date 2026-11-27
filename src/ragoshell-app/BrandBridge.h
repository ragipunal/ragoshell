#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/utils/UpdatePulse.h"
#include "ragoshell/core/utils/WasmMode.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    /**
     * @brief QML singleton "Brand": productName, url, donateUrl, and related constants
     *        from BrandInfo.h (docs/Brand.md). Never translate these.
     */
    class BrandBridge : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(Brand)
        QML_SINGLETON

        Q_PROPERTY(QString productName READ productName CONSTANT FINAL)
        Q_PROPERTY(QString productId READ productId CONSTANT FINAL)
        Q_PROPERTY(QString bundleId READ bundleId CONSTANT FINAL)
        Q_PROPERTY(QString domain READ domain CONSTANT FINAL)
        Q_PROPERTY(QString url READ url CONSTANT FINAL)
        Q_PROPERTY(QString productUrl READ productUrl CONSTANT FINAL)
        Q_PROPERTY(QString downloadUrl READ downloadUrl CONSTANT FINAL)
        Q_PROPERTY(QString donateUrl READ donateUrl CONSTANT FINAL)
        Q_PROPERTY(QString apiUrl READ apiUrl CONSTANT FINAL)
        Q_PROPERTY(QString portalUrl READ portalUrl CONSTANT FINAL)
        Q_PROPERTY(QString privacyUrl READ privacyUrl CONSTANT FINAL)
        Q_PROPERTY(QString termsUrl READ termsUrl CONSTANT FINAL)
        Q_PROPERTY(QString sourceRepoUrl READ sourceRepoUrl CONSTANT FINAL)
        Q_PROPERTY(bool hasMinisignPubkey READ hasMinisignPubkey CONSTANT FINAL)
        Q_PROPERTY(bool hasEntitlementPubkeys READ hasEntitlementPubkeys CONSTANT FINAL)
        Q_PROPERTY(bool isStoreBuild READ isStoreBuild CONSTANT FINAL)

    public:
        explicit BrandBridge(QObject *parent = nullptr) : QObject(parent) {}

        static BrandBridge *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<BrandBridge>::get();
        }

        QString productName() const
        {
            return QString::fromUtf8(BrandInfo::productName);
        }
        QString productId() const
        {
            return QString::fromUtf8(BrandInfo::productId);
        }
        QString bundleId() const
        {
            return QString::fromUtf8(BrandInfo::bundleId);
        }
        QString domain() const
        {
            return QString::fromUtf8(BrandInfo::domain);
        }
        QString url() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::productUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString productUrl() const
        {
            return url();
        }
        QString downloadUrl() const
        {
            return WasmMode::resolveDownloadPageUrl(QString::fromUtf8(BrandInfo::productUrl));
        }
        QString donateUrl() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::donateUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString apiUrl() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::apiUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString portalUrl() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::portalUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString privacyUrl() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::privacyUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString termsUrl() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::termsUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString sourceRepoUrl() const
        {
            const QString raw = QString::fromUtf8(BrandInfo::sourceRepoUrl).trimmed();
            return UpdatePulse::isSafeDownloadUrl(raw) ? raw : QString();
        }
        QString minisignPubkey() const
        {
            return QString::fromUtf8(BrandInfo::minisignPubkey).trimmed();
        }
        bool hasMinisignPubkey() const
        {
            return !minisignPubkey().isEmpty();
        }
        QString entitlementPubkeys() const
        {
            return QString::fromUtf8(BrandInfo::entitlementPubkeys).trimmed();
        }
        bool hasEntitlementPubkeys() const
        {
            return !entitlementPubkeys().isEmpty();
        }
        bool isStoreBuild() const
        {
            return BrandInfo::isStoreBuild;
        }

        /// True only for strict https:// URLs without embedded credentials (<= 1024 chars).
        Q_INVOKABLE bool isSafeHttpsUrl(const QString &candidate) const
        {
            return UpdatePulse::isSafeDownloadUrl(candidate);
        }
    };
}
