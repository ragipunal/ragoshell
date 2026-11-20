// ragoshell-mcp: local, read-only Model Context Protocol server over stdio.
// Serves only the saved connections allowed in ~/.ragoshell/mcp.json.
// See docs/McpServer.md.

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

#include <mongocxx/instance.hpp>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/mcp/McpBackend.h"
#include "ragoshell/core/mcp/McpConfig.h"
#include "ragoshell/core/mcp/McpGuards.h"
#include "ragoshell/core/mcp/McpServer.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppPaths.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define rago_dup _dup
#define rago_dup2 _dup2
#define rago_fdopen _fdopen
#define rago_fileno _fileno
#else
#include <unistd.h>
#define rago_dup dup
#define rago_dup2 dup2
#define rago_fdopen fdopen
#define rago_fileno fileno
#endif

using namespace RagoShell;

namespace
{
    enum ExitCode { Ok = 0, Usage = 1, NotEnabled = 2, ConfigError = 3 };

    QTextStream &out()
    {
        static QTextStream stream(stdout);
        return stream;
    }

    QTextStream &err()
    {
        static QTextStream stream(stderr);
        return stream;
    }

    /// Saved connection by uuid, or by name when the name is unique.
    ConnectionSettings *findConnection(SettingsManager &settings, const QString &ref, QString &error)
    {
        if (ConnectionSettings *byUuid = settings.getConnectionSettingsByUuid(ref))
            return byUuid;
        ConnectionSettings *match = nullptr;
        for (ConnectionSettings *c : settings.connections()) {
            if (QString::fromStdString(c->getReadableName()).compare(ref, Qt::CaseInsensitive) != 0)
                continue;
            if (match) {
                error = QStringLiteral("Connection name '%1' is ambiguous; use the uuid (see --status).").arg(ref);
                return nullptr;
            }
            match = c;
        }
        if (!match)
            error = QStringLiteral("No saved connection '%1' (see --status).").arg(ref);
        return match;
    }

    int printStatus(SettingsManager &settings, const McpConfig &config, const QString &configPath)
    {
        out() << BrandInfo::productName << " MCP server: " << (config.enabled ? "enabled" : "disabled")
              << "  (" << configPath << ")\n"
              << "Limits: maxDocuments=" << config.maxDocuments << ", maxTimeMS=" << config.maxTimeMs
              << ", maxResultBytes=" << config.maxResultBytes << "\n"
              << "Saved connections ([x] = exposed to MCP clients):\n";
        for (ConnectionSettings *c : settings.connections()) {
            out() << "  [" << (config.isAllowed(c->uuid()) ? 'x' : ' ') << "] " << c->uuid() << "  "
                  << QString::fromStdString(c->getReadableName()) << "  ("
                  << QString::fromStdString(c->getFullAddress()) << ")";
            const QString unsupported = McpGuards::unsupportedReason(*c);
            if (!unsupported.isEmpty())
                out() << "  -- " << unsupported;
            out() << "\n";
        }
        out().flush();
        return Ok;
    }

    int serve(SettingsManager &settings, const McpConfig &config, const QString &configPath,
              const QString &pinned)
    {
        if (!config.enabled) {
            err() << BrandInfo::productName
                  << " MCP server is disabled. Turn it on in " << BrandInfo::productName
                  << " (Preferences > AI tools) or with 'ragoshell-mcp --enable', and expose a connection "
                     "(the connection's settings, or 'ragoshell-mcp --allow <name|uuid>').\n";
            return NotEnabled;
        }

        std::vector<const ConnectionSettings *> connections;
        if (!pinned.isEmpty()) {
            QString error;
            const ConnectionSettings *c = findConnection(settings, pinned, error);
            if (!c || !config.isAllowed(c->uuid())) {
                err() << (c ? QStringLiteral("Connection '%1' is not allowed (ragoshell-mcp --allow %1).").arg(pinned)
                            : error) << "\n";
                return NotEnabled;
            }
            connections.push_back(c);
        } else {
            for (const ConnectionSettings *c : settings.connections()) {
                if (config.isAllowed(c->uuid()))
                    connections.push_back(c);
            }
        }
        if (connections.empty()) {
            err() << "No saved connection is allowed. Tick \"Let AI tools read this connection\" in the "
                     "connection's settings, or use 'ragoshell-mcp --allow <name|uuid>'.\n";
            return NotEnabled;
        }

        // The protocol owns the real stdout; fd 1 is pointed at stderr so
        // output from any library can never corrupt the JSON-RPC stream.
        std::fflush(stdout);
        const int protocolFd = rago_dup(rago_fileno(stdout));
        rago_dup2(rago_fileno(stderr), rago_fileno(stdout));
        FILE *protocol = rago_fdopen(protocolFd, "wb");
        if (!protocol) {
            err() << "Cannot open the protocol stream.\n";
            return ConfigError;
        }

        mongocxx::instance instance{};
        MongoMcpBackend backend(settings.uuidEncoding(), settings.timeZone());
        McpServer server(config, connections, backend);
        err() << "ragoshell-mcp " << PROJECT_VERSION << ": serving " << connections.size()
              << " connection(s), read-only\n";
        err().flush();

        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos)
                continue;
            // The app or the command line may revoke a connection or turn the
            // server off while a client keeps it running: every request sees
            // the current file. One that no longer loads disables serving.
            McpConfig current;
            QString reloadError;
            if (!McpConfig::load(configPath, current, &reloadError)) {
                err() << reloadError << " -- serving nothing until it is fixed.\n";
                err().flush();
                current = McpConfig();
            }
            server.applyConfig(current);
            const QByteArray response = server.handleLine(QByteArray::fromStdString(line));
            if (response.isEmpty())
                continue;
            std::fwrite(response.constData(), 1, static_cast<size_t>(response.size()), protocol);
            std::fputc('\n', protocol);
            std::fflush(protocol);
        }
        std::fclose(protocol);
        return Ok;
    }
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    // JSON-RPC lines must pass through untranslated (no CRLF, no ^Z).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ragoshell-mcp"));
    QCoreApplication::setApplicationVersion(PROJECT_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Read-only MCP server for %1 saved connections (stdio transport).\n"
                       "Without options it serves MCP on stdin/stdout; register it in your AI client.")
            .arg(QLatin1String(RagoShell::BrandInfo::productName)));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption connectionOpt(
        "connection", "Serve only this allowed connection (name or uuid).", "name|uuid");
    const QCommandLineOption statusOpt("status", "Show whether the server is enabled and which connections it exposes.");
    const QCommandLineOption enableOpt("enable", "Enable the MCP server.");
    const QCommandLineOption disableOpt("disable", "Disable the MCP server (keeps the allow-list).");
    const QCommandLineOption allowOpt("allow", "Expose a saved connection to MCP clients.", "name|uuid");
    const QCommandLineOption revokeOpt("revoke", "Stop exposing a saved connection.", "name|uuid");
    const QCommandLineOption configOpt(
        "config", QStringLiteral("MCP settings file (default %1).").arg(McpConfig::defaultPath()), "path");
    parser.addOptions({connectionOpt, statusOpt, enableOpt, disableOpt, allowOpt, revokeOpt, configOpt});
    parser.process(app);

    const QString configPath = parser.isSet(configOpt) ? parser.value(configOpt) : McpConfig::defaultPath();
    McpConfig config;
    QString error;
    if (!McpConfig::load(configPath, config, &error)) {
        err() << error << "\n";
        return ConfigError;
    }

    SettingsManager &settings = *AppRegistry::instance().settingsManager();

    const bool manage = parser.isSet(enableOpt) || parser.isSet(disableOpt) || parser.isSet(allowOpt)
                        || parser.isSet(revokeOpt);
    if (manage) {
        if (parser.isSet(enableOpt) && parser.isSet(disableOpt)) {
            err() << "--enable and --disable are mutually exclusive.\n";
            return Usage;
        }
        QStringList allow, revoke;
        for (const QString &ref : parser.values(allowOpt)) {
            ConnectionSettings *c = findConnection(settings, ref, error);
            if (!c) {
                err() << error << "\n";
                return Usage;
            }
            const QString unsupported = McpGuards::unsupportedReason(*c);
            if (!unsupported.isEmpty()) {
                err() << QString::fromStdString(c->getReadableName()) << ": " << unsupported << "\n";
                return Usage;
            }
            allow << c->uuid();
        }
        for (const QString &ref : parser.values(revokeOpt)) {
            ConnectionSettings *c = findConnection(settings, ref, error);
            // A uuid of a connection deleted in the app can still be revoked.
            revoke << (c ? c->uuid() : ref);
        }
        // Read-modify-write under the same lock as the app (McpConfig::modify).
        const bool enable = parser.isSet(enableOpt), disable = parser.isSet(disableOpt);
        if (!McpConfig::modify(configPath, [&](McpConfig &c) {
                if (enable)
                    c.enabled = true;
                if (disable)
                    c.enabled = false;
                for (const QString &uuid : allow) {
                    if (!c.allowedConnections.contains(uuid))
                        c.allowedConnections.append(uuid);
                }
                for (const QString &uuid : revoke)
                    c.allowedConnections.removeAll(uuid);
            }, &error)) {
            err() << error << "\n";
            return ConfigError;
        }
        McpConfig::load(configPath, config);
        return printStatus(settings, config, configPath);
    }
    if (parser.isSet(statusOpt))
        return printStatus(settings, config, configPath);

    return serve(settings, config, configPath, parser.value(connectionOpt));
}
