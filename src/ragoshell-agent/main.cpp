// ragoshell-agent: local Mod C bridge for the WASM build
// (docs/LocalAgent.md, roadmap 8.3). Binds 127.0.0.1 only, requires an
// Origin allowlist hit + a pairing token, and routes GatewayProtocol frames
// to a per-session NativeMongoTransport — credentials never leave the
// machine.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QWebSocket>
#include <QWebSocketServer>

// Transport headers first: their mongocxx forward decl marks v_noabi inline;
// including instance.hpp first would declare it non-inline and conflict.
#include "ragoshell/core/cloud/SyncCrypto.h"
#include "ragoshell/core/transport/GatewayProtocol.h"
#include "ragoshell/core/transport/GatewaySession.h"
#include "ragoshell/core/transport/NativeMongoTransport.h"
#include "ragoshell/core/utils/AppPaths.h"

#include <mongocxx/instance.hpp>

namespace
{
    QString tokenFilePath()
    {
        return RagoShell::appDataDir() + QStringLiteral("agent-token");
    }

    // Reads the pairing token from disk, or mints a fresh 32-byte hex one
    // with owner-only permissions (T4: the token itself is only printed to
    // stdout — the operator pastes it into the browser UI once). Returns {}
    // on RNG failure — the caller must abort rather than run with an empty
    // token (empty would pass the constant-time compare).
    QString loadOrMintToken(QString *error)
    {
        QFile f(tokenFilePath());
        if (f.exists()) {
            const QFileDevice::Permissions p = f.permissions();
            if (p & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                     | QFileDevice::ReadOther | QFileDevice::WriteOther)) {
                // A world-readable pairing token is equivalent to running
                // with no auth — repair or refuse (8.6 review L6).
                if (!f.setPermissions(QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner)) {
                    *error = QStringLiteral(
                        "%1 is readable by group/others and could not be "
                        "restricted — chmod 600 it").arg(tokenFilePath());
                    return {};
                }
            }
            if (f.open(QIODevice::ReadOnly)) {
                const QString token = QString::fromUtf8(f.readAll()).trimmed();
                if (token.size() == 64)
                    return token;
            }
        }
        const QString token =
            QString::fromLatin1(RagoShell::SyncCrypto::randomBytes(32).toHex());
        if (token.size() != 64) {
            *error = QStringLiteral("secure RNG failed; refusing to start");
            return {};
        }
        QDir().mkpath(RagoShell::appDataDir());
        QFile out(tokenFilePath());
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            // Restrict before writing — between open() and this call the file
            // is still empty, so the umask window leaks nothing. If the
            // restriction fails the file must not be written at umask perms.
            if (!out.setPermissions(QFileDevice::ReadOwner
                                    | QFileDevice::WriteOwner)) {
                out.close();
                out.remove();
                *error = QStringLiteral("cannot restrict %1 to owner-only")
                             .arg(tokenFilePath());
                return {};
            }
            out.write(token.toUtf8());
            out.close();
        } else {
            *error = QStringLiteral("cannot write %1: %2").arg(tokenFilePath(),
                                                               out.errorString());
        }
        return token;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    mongocxx::instance instance{};

    const QString configuredPort = RagoShell::productEnv("AGENT_PORT");
    bool portOk = false;
    const int port = configuredPort.isEmpty()
        ? RagoShell::GatewayProtocol::kDefaultAgentPort
        : configuredPort.toInt(&portOk);
    if (!configuredPort.isEmpty() && (!portOk || port <= 0 || port > 65535)) {
        QTextStream(stderr) << "ragoshell-agent: invalid RAGOSHELL_AGENT_PORT '"
                            << configuredPort << "'\n";
        return 2;
    }

    QString tokenError;
    const QString token = loadOrMintToken(&tokenError);
    if (!tokenError.isEmpty())
        QTextStream(stderr) << "ragoshell-agent: " << tokenError << "\n";
    if (token.isEmpty())
        return 1;

    // Origin allowlist (docs/LocalAgent.md §2): product web origins plus
    // loopback dev servers on any port ("…:" prefix entries match any port).
    const QStringList allowedOrigins{
        QStringLiteral("https://ragoshell.com.tr"),
        QStringLiteral("https://app.ragoshell.com.tr"),
        QStringLiteral("https://www.ragoshell.com.tr"),
        // Exact entries for default-port loopback (browsers strip :80/:443)
        // plus "…:" prefixes matching a dev server on any port.
        QStringLiteral("http://localhost"),
        QStringLiteral("https://localhost"),
        QStringLiteral("http://127.0.0.1"),
        QStringLiteral("https://127.0.0.1"),
        QStringLiteral("http://localhost:"),
        QStringLiteral("https://localhost:"),
        QStringLiteral("http://127.0.0.1:"),
        QStringLiteral("https://127.0.0.1:")};

    QWebSocketServer server(QStringLiteral("RagoShell Agent"),
                            QWebSocketServer::NonSecureMode, &app);
    if (!server.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        QTextStream(stderr) << "ragoshell-agent: cannot bind 127.0.0.1:" << port
                            << " — " << server.errorString() << "\n";
        return 1;
    }

    int sessions = 0;
    const QString requiredHost =
        QStringLiteral("127.0.0.1:%1").arg(server.serverPort());
    QObject::connect(&server, &QWebSocketServer::newConnection, &app, [&] {
        QWebSocket *socket = server.nextPendingConnection();
        if (sessions >= RagoShell::GatewayProtocol::kMaxSessions) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("too many sessions"));
            socket->deleteLater();
            return;
        }
        ++sessions;
        auto *session = new RagoShell::GatewaySession(
            socket, allowedOrigins, requiredHost, token,
            [] { return std::make_unique<RagoShell::NativeMongoTransport>(); },
            &app);
        QObject::connect(session, &RagoShell::GatewaySession::finished,
                         &app, [&sessions, session] {
                             --sessions;
                             session->deleteLater();
                         });
    });

    QTextStream(stdout) << "ragoshell-agent listening on ws://" << requiredHost
                        << "\npairing token: " << token
                        << "\n(token also stored in " << tokenFilePath() << ")\n";
    QTextStream(stdout).flush();
    return app.exec();
}
