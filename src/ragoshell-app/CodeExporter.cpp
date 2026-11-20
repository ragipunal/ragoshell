#include "ragoshell-app/CodeExporter.h"

#include <algorithm>
#include <memory>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/utils/QueryCodeExport.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    namespace
    {
        // Options worth keeping in sample code; app-specific ones (appname,
        // timeouts) are dropped.
        const QStringList kKeptUriOptions = {
            QStringLiteral("replicaSet"), QStringLiteral("tls"), QStringLiteral("authSource"),
            QStringLiteral("authMechanism"), QStringLiteral("readPreference"),
        };
        constexpr auto kPasswordPlaceholder = "<password>";
    }

    CodeExporter::CodeExporter(QObject *parent)
        : QObject(parent)
    {
    }

    QStringList CodeExporter::languages() const
    {
        return QueryCodeExport::languageNames();
    }

    QString CodeExporter::connectionUriFor(const QString &serverUuid) const
    {
        QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek();
        std::unique_ptr<ConnectionSettings> conn = backend ? backend->cloneConnection(serverUuid) : nullptr;
        if (!conn)
            return QStringLiteral("mongodb://localhost:27017");

        // Never put the real password into code meant for copying.
        if (conn->hasEnabledPrimaryCredential())
            conn->primaryCredential()->setUserPassword(kPasswordPlaceholder);
        QString uri = QString::fromStdString(buildMongoUri(*conn));
        uri.replace(QStringLiteral("%3Cpassword%3E"), QLatin1String(kPasswordPlaceholder));

        const int q = uri.indexOf(QLatin1Char('?'));
        if (q < 0)
            return uri;
        QStringList kept;
        for (const QString &param : uri.mid(q + 1).split(QLatin1Char('&'))) {
            if (kKeptUriOptions.contains(param.section(QLatin1Char('='), 0, 0)))
                kept << param;
        }
        uri.truncate(q);
        return kept.isEmpty() ? uri : uri + QLatin1Char('?') + kept.join(QLatin1Char('&'));
    }

    QVariantMap CodeExporter::exportScript(const QString &script, const QString &database,
                                           const QString &serverUuid, int language) const
    {
        const int lang = std::clamp(language, 0, static_cast<int>(QueryCodeExport::Language::CSharp));
        const auto result = QueryCodeExport::exportScript(
            script, database, connectionUriFor(serverUuid), static_cast<QueryCodeExport::Language>(lang));
        return {
            {QStringLiteral("ok"), result.ok},
            {QStringLiteral("error"), result.error},
            {QStringLiteral("code"), result.code},
        };
    }
}
