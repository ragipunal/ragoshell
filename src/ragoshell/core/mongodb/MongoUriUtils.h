#pragma once

#include <string>

#include <QStringList>

#include <mongocxx/options/client.hpp>

namespace RagoShell
{
    class ConnectionSettings;

    /**
     * @brief Builds a mongodb:// URI from connection settings.
     * Shared by MongoWorker (mongocxx client) and ScriptEngine (the embedded shell).
     */
    std::string buildMongoUri(const ConnectionSettings &settings);

    /**
     * @brief The client name the driver reports to the server (`appname`, seen in
     * server logs and currentOp): "<product id>-<suffix>", e.g. "ragoshell-2.1.0".
     */
    std::string clientAppName(const std::string &suffix);

    /**
     * @brief The authentication mechanisms the connection dialog offers on this
     * platform. GSSAPI (Kerberos) needs the platform's Kerberos library, which
     * the phone and tablet builds do not have.
     */
    QStringList supportedAuthMechanisms();

    /**
     * @brief TLS client options (CA, PEM key, CRL) for a standalone mongocxx
     * client, e.g. import/export and pipeline previews.
     */
    mongocxx::options::client buildMongoClientOptions(const ConnectionSettings &settings);
}
