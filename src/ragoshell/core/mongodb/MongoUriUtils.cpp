#include "ragoshell/core/mongodb/MongoUriUtils.h"

#include <vector>
#include <string>

#include <QUrl>
#include <QString>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/domain/HostAndPort.h"


namespace RagoShell
{
    namespace
    {
        std::string percentEncode(const std::string &value)
        {
            return QUrl::toPercentEncoding(QString::fromStdString(value)).toStdString();
        }

        // URI value for a non-default read preference; empty for PRIMARY.
        std::string readPreferenceName(ReplicaSetSettings::ReadPreference pref)
        {
            using RP = ReplicaSetSettings::ReadPreference;
            switch (pref) {
            case RP::PRIMARY_PREFERRED:   return "primaryPreferred";
            case RP::SECONDARY:           return "secondary";
            case RP::SECONDARY_PREFERRED: return "secondaryPreferred";
            case RP::NEAREST:             return "nearest";
            case RP::PRIMARY:
            default:                      return {};
            }
        }

        // Mechanisms whose credentials live outside MongoDB (certificate,
        // Kerberos, LDAP): drivers require authSource=$external for them.
        bool usesExternalAuthSource(const std::string &mechanism)
        {
            return mechanism == "MONGODB-X509" || mechanism == "GSSAPI" ||
                   mechanism == "PLAIN";
        }
    }

    QStringList supportedAuthMechanisms()
    {
        QStringList mechanisms{ QStringLiteral("DEFAULT"),
                                QStringLiteral("SCRAM-SHA-256"), QStringLiteral("SCRAM-SHA-1"),
                                QStringLiteral("MONGODB-X509") };
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        mechanisms << QStringLiteral("GSSAPI");
#endif
        mechanisms << QStringLiteral("PLAIN");
        return mechanisms;
    }

    std::string clientAppName(const std::string &suffix)
    {
        return QString::fromLatin1(BrandInfo::productId).toLower().toStdString() + "-" + suffix;
    }

    std::string buildMongoUri(const ConnectionSettings &settings)
    {
        // SRV: hosts, replica set name and TLS default come from DNS (SRV/TXT),
        // so only the seedlist hostname is written.
        const bool srv = settings.isSrv();
        const bool replicaSet = settings.isReplicaSet();
        std::string uri = srv ? "mongodb+srv://" : "mongodb://";

        if (settings.hasEnabledPrimaryCredential()) {
            CredentialSettings const* const cred = settings.primaryCredential();
            if (!cred->userName().empty()) {
                // X.509 authenticates with the client certificate; drivers
                // reject a password, and the subject name is optional.
                if (cred->mechanism() == "MONGODB-X509")
                    uri += percentEncode(cred->userName()) + '@';
                else
                    uri += percentEncode(cred->userName()) + ':' +
                           percentEncode(cred->userPassword()) + '@';
            }
        }

        if (srv) {
            uri += settings.serverHost();
        }
        else if (replicaSet) {
            auto const& members = settings.replicaSetSettings()->membersToHostAndPort();
            bool first = true;
            for (auto const& hp : members) {
                if (!first)
                    uri += ',';
                uri += hp.toString();
                first = false;
            }
        }
        else {
            uri += settings.hostAndPort().toString();
        }

        uri += '/';

        std::vector<std::string> params;
        if (replicaSet) {
            std::string setName = settings.replicaSetSettings()->setNameUserEntered();
            if (setName.empty())
                setName = settings.replicaSetSettings()->cachedSetName();
            if (!setName.empty())
                params.push_back("replicaSet=" + percentEncode(setName));
            const std::string readPref =
                readPreferenceName(settings.replicaSetSettings()->readPreference());
            if (!readPref.empty())
                params.push_back("readPreference=" + readPref);
        }
        if (settings.hasEnabledPrimaryCredential()) {
            CredentialSettings const* const cred = settings.primaryCredential();
            if (usesExternalAuthSource(cred->mechanism()))
                params.push_back("authSource=$external");
            else if (!cred->userName().empty() && !cred->databaseName().empty())
                params.push_back("authSource=" + percentEncode(cred->databaseName()));
            // Omit DEFAULT and legacy MONGODB-CR (removed in MongoDB 4.0)
            // so drivers negotiate SCRAM-SHA-256 / SCRAM-SHA-1 automatically.
            if (!cred->mechanism().empty() &&
                cred->mechanism() != "MONGODB-CR" &&
                cred->mechanism() != "DEFAULT")
                params.push_back("authMechanism=" + percentEncode(cred->mechanism()));
        }
        params.push_back("appname=" + percentEncode(clientAppName(PROJECT_VERSION)));
        params.push_back("serverSelectionTimeoutMS=5000");
        if (settings.sslSettings()->sslEnabled()) {
            params.push_back("tls=true");
            if (settings.sslSettings()->allowInvalidHostnames())
                params.push_back("tlsAllowInvalidHostnames=true");
        }

        for (std::size_t i = 0; i < params.size(); ++i) {
            uri += (i == 0 ? '?' : '&');
            uri += params[i];
        }

        return uri;
    }

    mongocxx::options::client buildMongoClientOptions(const ConnectionSettings &settings)
    {
        mongocxx::options::client opts;
        const SslSettings *ssl = settings.sslSettings();
        if (ssl && ssl->sslEnabled()) {
            mongocxx::options::tls tls;
            tls.allow_invalid_certificates(ssl->allowInvalidCertificates());
            if (!ssl->allowInvalidCertificates() && !ssl->caFile().empty())
                tls.ca_file(ssl->caFile());
            if (ssl->usePemFile() && !ssl->pemKeyFile().empty()) {
                tls.pem_file(ssl->pemKeyFile());
                if (!ssl->pemPassPhrase().empty())
                    tls.pem_password(ssl->pemPassPhrase());
            }
            if (ssl->useAdvancedOptions() && !ssl->crlFile().empty())
                tls.crl_file(ssl->crlFile());
            opts.tls_opts(tls);
        }
        return opts;
    }
}
