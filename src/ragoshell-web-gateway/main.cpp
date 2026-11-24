// ragoshell-web-gateway: Mod B self-hosted bridge (docs/WebGateway.md,
// WebAssembly.md Faz 3, roadmap 8.4). One QAbstractHttpServer port serves
// the static WASM bundle with hardening headers AND terminates WebSocket
// upgrades on /ws into the shared GatewaySession + NativeMongoTransport
// stack — the same wire the local agent speaks. TLS termination is the
// reverse proxy's job; SSRF egress filtering runs on every connect op.

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QMimeDatabase>
#include <QTcpServer>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/transport/BridgeQuota.h"
#include "ragoshell/core/transport/DeviceSessionLedger.h"
#include "ragoshell/core/transport/EgressPolicy.h"
#include "ragoshell/core/transport/GatewayProtocol.h"
#include "ragoshell/core/transport/GatewaySession.h"
#include "ragoshell/core/transport/NativeMongoTransport.h"
#include "ragoshell/core/transport/WebRoot.h"

#include <mongocxx/instance.hpp>

#include <csignal>

namespace
{
    // SIGTERM/SIGINT (docker stop) only set this flag — the handler must stay
    // async-signal-safe; a timer on the event loop sees it and quits cleanly
    // so the quota file gets its final write (docs/HostedGateway.md §9.3).
    volatile std::sig_atomic_t g_stopRequested = 0;
    extern "C" void requestStop(int) { g_stopRequested = 1; }

    QString envOr(const char *name, const QString &fallback)
    {
        const QString v = qEnvironmentVariable(name);
        return v.isEmpty() ? fallback : v;
    }

    int envInt(const char *name, int fallback)
    {
        bool ok = false;
        const int v = qEnvironmentVariable(name).toInt(&ok);
        return ok ? v : fallback;
    }

    // Strict variant for the free tier limits: an unset variable gives the
    // default, a set one that does not parse stops the start (review L8 —
    // "1GB" must not quietly become the default).
    bool envLimit(const char *name, qint64 fallback, qint64 *out)
    {
        const QString raw = qEnvironmentVariable(name).trimmed();
        if (raw.isEmpty()) {
            *out = fallback;
            return true;
        }
        bool ok = false;
        *out = raw.toLongLong(&ok);
        if (!ok)
            QTextStream(stderr) << "ragoshell-web-gateway: " << name
                                << " must be a whole number, got '" << raw << "'\n";
        return ok;
    }

    QHttpHeaders securityHeaders()
    {
        QHttpHeaders h;
        h.append(QHttpHeaders::WellKnownHeader::XContentTypeOptions, "nosniff");
        h.append(QHttpHeaders::WellKnownHeader::XFrameOptions, "DENY");
        h.append("Referrer-Policy", "no-referrer");
        h.append("Cross-Origin-Opener-Policy", "same-origin");
        h.append("Cross-Origin-Embedder-Policy", "require-corp");
        h.append("Cross-Origin-Resource-Policy", "same-origin");
        // 'wasm-unsafe-eval' is the minimal allowance Qt WASM needs;
        // 'unsafe-inline' is only for style (Qt injects style attrs).
        h.append("Content-Security-Policy",
                 "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; "
                 "connect-src 'self' wss: ws:; worker-src 'self' blob:; "
                 "img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
                 "font-src 'self'; frame-ancestors 'none'; base-uri 'self'");
        return h;
    }

    // Serves one file under webRoot (already canonical, startup-checked)
    // with the security header set; .br variants are preferred when the
    // client accepts brotli.
    QHttpServerResponse serveFile(const QString &root, const QString &urlPath,
                                  const QHttpServerRequest &req)
    {
        auto deny = [](QHttpServerResponder::StatusCode code) {
            QHttpServerResponse r(code);
            r.setHeaders(securityHeaders());
            return r;
        };

        QString rel = QUrl::fromPercentEncoding(urlPath.toUtf8());
        if (rel.isEmpty() || rel == "/")
            rel = QStringLiteral("/index.html");
        // Path traversal guard: single decode already done; reject any
        // residual "..", NUL or absolute form before touching the disk.
        if (rel.contains(QLatin1String("..")) || rel.contains(QChar(0)))
            return deny(QHttpServerResponder::StatusCode::Forbidden);
        if (!rel.startsWith(QLatin1Char('/')))
            rel.prepend(QLatin1Char('/'));

        // Canonicalize before the prefix check — a symlink inside the web
        // root pointing outside must NOT be served (L3). Missing paths
        // canonicalize to empty → 404.
        const QString candidate = QDir(root).absoluteFilePath(rel.mid(1));
        if (QFileInfo(candidate).canonicalFilePath().isEmpty())
            return deny(QHttpServerResponder::StatusCode::NotFound);
        const QString filePath = RagoShell::WebRoot::containedPath(root, candidate);
        if (filePath.isEmpty())
            return deny(QHttpServerResponder::StatusCode::Forbidden);

        // Prefer the pre-compressed .br sibling only when the client lists
        // `br` as an encoding token with q>0 — a substring match would also
        // honor "brotli" or a refused "br;q=0".
        QString servePath = filePath;
        QByteArray encoding;
        const QStringList encodings = QString::fromUtf8(
            req.headers().value(QHttpHeaders::WellKnownHeader::AcceptEncoding))
            .split(QLatin1Char(','), Qt::SkipEmptyParts);
        bool acceptsBr = false;
        for (const QString &e : encodings) {
            const QStringList parts = e.trimmed().split(QLatin1Char(';'));
            if (parts.first().trimmed().compare(QLatin1String("br"),
                                              Qt::CaseInsensitive) == 0) {
                QString q = QStringLiteral("1");
                if (parts.size() > 1)
                    q = parts.at(1).trimmed().mid(1); // strip 'q'
                acceptsBr = q != QStringLiteral("0") && q != QStringLiteral("0.0");
            }
        }
        // The sibling gets the same containment check as the requested
        // file: opening `filePath + ".br"` directly would follow a symlink
        // that points outside the web root (2026-11-04 review R9).
        const QString brPath = acceptsBr
            ? RagoShell::WebRoot::containedPath(root, filePath + QStringLiteral(".br"))
            : QString();
        if (!brPath.isEmpty()) {
            servePath = brPath;
            encoding = "br";
        }

        QFile file(servePath);
        if (!file.exists() || !file.open(QIODevice::ReadOnly))
            return deny(QHttpServerResponder::StatusCode::NotFound);
        const QByteArray data = file.readAll();

        // MIME from the UNPACKED name; .wasm is mandatory and unknown to
        // QMimeDatabase on some platforms.
        QString mimeName = QMimeDatabase().mimeTypeForFile(
            filePath, QMimeDatabase::MatchExtension).name();
        if (filePath.endsWith(QLatin1String(".wasm")))
            mimeName = QStringLiteral("application/wasm");

        QHttpServerResponse response(mimeName.toUtf8(), data);
        QHttpHeaders h = securityHeaders();
        if (!encoding.isEmpty())
            h.append(QHttpHeaders::WellKnownHeader::ContentEncoding, encoding);
        // index.html always revalidates; fingerprinted assets may cache.
        if (rel.endsWith(QLatin1String("index.html")) || rel == QLatin1String("/"))
            h.append("Cache-Control", "no-cache");
        else
            h.append("Cache-Control", "public, max-age=86400");
        response.setHeaders(std::move(h));
        return response;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    mongocxx::instance instance{};

    const QString bind = envOr("RAGOSHELL_GW_BIND", QStringLiteral("0.0.0.0"));
    const int port = envInt("RAGOSHELL_GW_PORT", 8080);
    const QString wsPath = envOr("RAGOSHELL_GW_WS_PATH", QStringLiteral("/ws"));
    const QString webRoot = envOr("RAGOSHELL_WEB_ROOT", QStringLiteral("./web"));
    const QString token = qEnvironmentVariable("RAGOSHELL_GW_TOKEN");
    const bool openMode = qEnvironmentVariable("RAGOSHELL_GW_OPEN") == QLatin1String("1");
    const bool allowPrivate =
        qEnvironmentVariable("RAGOSHELL_GW_ALLOW_PRIVATE") == QLatin1String("1");
    // Hosted mode (docs/HostedGateway.md): hello carries entitlement+jwt
    // verified against the API key ring — GW_TOKEN/Open are not used.
    const QString authMode = envOr("RAGOSHELL_GW_AUTH", QStringLiteral("token"));
    const bool hostedMode = authMode.compare(QLatin1String("hosted"),
                                           Qt::CaseInsensitive) == 0;
    const bool requireTls =
        hostedMode
            ? qEnvironmentVariable("RAGOSHELL_GW_REQUIRE_TLS") != QLatin1String("0")
            : qEnvironmentVariable("RAGOSHELL_GW_REQUIRE_TLS") == QLatin1String("1");
    // Free tier limits (docs/HostedGateway.md §9), hosted mode only.
    qint64 maxSessions = 0, maxPerAccount = 0, sessionMaxSec = 0, idleSec = 0, monthlyBytes = 0;
    if (!envLimit("RAGOSHELL_GW_MAX_SESSIONS", RagoShell::GatewayProtocol::kMaxSessions, &maxSessions)
        || !envLimit("RAGOSHELL_GW_MAX_PER_ACCOUNT", 1, &maxPerAccount)
        || !envLimit("RAGOSHELL_GW_SESSION_MAX_SEC", 2 * 3600, &sessionMaxSec)
        || !envLimit("RAGOSHELL_GW_IDLE_SEC", 15 * 60, &idleSec)
        || !envLimit("RAGOSHELL_GW_MONTHLY_BYTES", 1000000000LL, &monthlyBytes))
        return 2;
    if (maxSessions < 1 || maxSessions > 10000) {
        QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_MAX_SESSIONS must be 1-10000\n";
        return 2;
    }
    const QString quotaFile = qEnvironmentVariable("RAGOSHELL_GW_QUOTA_FILE");
    const QString denyFile = qEnvironmentVariable("RAGOSHELL_GW_DENY_FILE");
    const QString statusFile = qEnvironmentVariable("RAGOSHELL_GW_STATUS_FILE");

    if (port < 1 || port > 65535) {
        QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_PORT must "
                               "be 1-65535\n";
        return 2;
    }
    if (!hostedMode && token.isEmpty() && !openMode) {
        QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_TOKEN is "
                               "required; set RAGOSHELL_GW_OPEN=1 only for "
                               "trusted-LAN dev\n";
        return 2;
    }
    if (!hostedMode && !token.isEmpty() && token.size() < 32) {
        QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_TOKEN "
                               "must be >= 32 characters\n";
        return 2;
    }
    if (hostedMode) {
        // Out-of-range limits stop the start instead of silently becoming
        // "unlimited" or "always refused".
        if (maxPerAccount < 1 || maxPerAccount > maxSessions) {
            QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_MAX_PER_ACCOUNT "
                                   "must be 1 up to RAGOSHELL_GW_MAX_SESSIONS\n";
            return 2;
        }
        if (sessionMaxSec < 60 || sessionMaxSec > 24 * 3600) {
            QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_SESSION_MAX_SEC "
                                   "must be 60-86400\n";
            return 2;
        }
        if (idleSec < 30 || idleSec > sessionMaxSec) {
            QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_IDLE_SEC must be "
                                   "30 up to RAGOSHELL_GW_SESSION_MAX_SEC\n";
            return 2;
        }
        if (monthlyBytes < 1) {
            QTextStream(stderr) << "ragoshell-web-gateway: RAGOSHELL_GW_MONTHLY_BYTES "
                                   "must be a positive byte count\n";
            return 2;
        }
        if (quotaFile.isEmpty()) {
            QTextStream(stderr) << "ragoshell-web-gateway: hosted mode needs "
                                   "RAGOSHELL_GW_QUOTA_FILE (monthly transfer counters "
                                   "must survive a restart)\n";
            return 2;
        }
        if (!qEnvironmentVariable("RAGOSHELL_GW_MAX_PER_DEVICE").isEmpty())
            QTextStream(stderr) << "ragoshell-web-gateway: WARNING — "
                                   "RAGOSHELL_GW_MAX_PER_DEVICE is ignored; sessions are "
                                   "capped per account (RAGOSHELL_GW_MAX_PER_ACCOUNT)\n";
    }
    if (openMode)
        QTextStream(stderr) << "ragoshell-web-gateway: WARNING — "
                            << (hostedMode
                                    ? "RAGOSHELL_GW_OPEN has no effect in hosted mode"
                                    : "open mode, no pairing token required")
                            << "\n";

    // Canonicalize once at startup and refuse to run without a web root:
    // an empty canonical path degenerates the per-request prefix check
    // into "serves anything under CWD" (in the image: /).
    const QString root = QFileInfo(webRoot).canonicalFilePath();
    if (root.isEmpty()) {
        QTextStream(stderr) << "ragoshell-web-gateway: web root "
                            << webRoot << " does not exist — build the WASM "
                               "bundle first (docs/WebGateway.md §5)\n";
        return 2;
    }

    // Origin allowlist: origins the gateway serves itself + dev loopback +
    // operator extras. The HTTP upgrade verifier enforces this list at the
    // header level — the authoritative check for this deployment.
    QStringList allowedOrigins{
        QStringLiteral("https://ragoshell.com.tr"),
        QStringLiteral("https://app.ragoshell.com.tr"),
        QStringLiteral("https://www.ragoshell.com.tr"),
        QStringLiteral("http://localhost"), QStringLiteral("https://localhost"),
        QStringLiteral("http://127.0.0.1"), QStringLiteral("https://127.0.0.1"),
        QStringLiteral("http://localhost:"), QStringLiteral("https://localhost:"),
        QStringLiteral("http://127.0.0.1:"), QStringLiteral("https://127.0.0.1:")};
    const QString extra = qEnvironmentVariable("RAGOSHELL_GW_ORIGINS");
    for (const QString &o : extra.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString t = o.trimmed();
        if (t == QLatin1String("*"))
            QTextStream(stderr) << "ragoshell-web-gateway: WARNING — '*' is "
                                   "not a valid origin entry and DENIES all "
                                   "origins at the HTTP verifier\n";
        else if (t.endsWith(QLatin1Char(':')))
            QTextStream(stderr) << "ragoshell-web-gateway: WARNING — "
                                   "permissive origin prefix '" << t << "'\n";
        allowedOrigins << t;
    }
    auto originAllowed = [&allowedOrigins](const QString &origin) {
        for (const QString &a : allowedOrigins) {
            if (a.endsWith(QLatin1Char(':'))
                    ? origin.startsWith(a) : origin == a)
                return true;
        }
        return false;
    };

    RagoShell::EgressPolicy egress(allowPrivate);
    egress.setRequireTls(requireTls);
    if (allowPrivate)
        QTextStream(stderr) << "ragoshell-web-gateway: WARNING — private-IP "
                               "targets allowed (RAGOSHELL_GW_ALLOW_PRIVATE)\n";
    if (requireTls)
        QTextStream(stderr) << "ragoshell-web-gateway: TLS required on Mongo "
                               "targets\n";

    // Hosted auth: entitlement+jwt ring + per-device session ledger. Both
    // are shared_ptr — sessions copy HostedAuthConfig, so the objects must
    // outlive session teardown no matter the shutdown order (8.6 review L4).
    RagoShell::HostedAuthConfig hosted;
    auto verifyOpts = std::make_shared<RagoShell::EntitlementVerifyOptions>();
    if (hostedMode) {
        const QString ring = qEnvironmentVariable("RAGOSHELL_GW_ENT_KEYRING");
        bool ok = false;
        QString err;
        auto verifier = std::make_shared<RagoShell::EntitlementVerifier>(
            ring.isEmpty()
                ? RagoShell::EntitlementVerifier::fromEmbeddedDefaults(&ok, &err)
                : RagoShell::EntitlementVerifier::fromKeyRingString(ring, &ok, &err));
        if (!ok || verifier->keyCount() == 0) {
            QTextStream(stderr) << "ragoshell-web-gateway: hosted mode needs a "
                                   "valid entitlement key ring "
                                   "(RAGOSHELL_GW_ENT_KEYRING): " << err << "\n";
            return 2;
        }
        verifyOpts->expectedIssuer =
            envOr("RAGOSHELL_GW_ENT_ISS",
                  RagoShell::EntitlementVerifier::defaultApiIssuer());
        verifyOpts->expectedAudience =
            envOr("RAGOSHELL_GW_ENT_AUD",
                  RagoShell::EntitlementVerifier::defaultAppAudience());
        hosted.verifier = verifier;
        hosted.options = verifyOpts;
        hosted.ledger =
            std::make_shared<RagoShell::DeviceSessionLedger>(int(maxPerAccount));
        hosted.maxSessionMs = int(sessionMaxSec * 1000);
        hosted.idleMs = int(idleSec * 1000);

        // Monthly transfer counters: an existing file that cannot be read
        // stops the start — serving with empty counters would hand every
        // account a fresh quota (fail closed, §9.3).
        auto quota = std::make_shared<RagoShell::BridgeQuota>(quotaFile, monthlyBytes);
        QString qerr;
        if (!quota->load(&qerr)) {
            QTextStream(stderr) << "ragoshell-web-gateway: " << qerr << " ("
                                << quotaFile << "); inspect or remove it\n";
            return 2;
        }
        hosted.quota = quota;

        auto deny = std::make_shared<RagoShell::AccountDenyList>(denyFile);
        QString derr;
        if (!deny->reload(&derr))
            QTextStream(stderr) << "ragoshell-web-gateway: WARNING — " << derr << "\n";
        hosted.deny = deny;

        // Once a minute: persist changed counters, pick up deny list edits.
        auto *maintenance = new QTimer(&app);
        QObject::connect(maintenance, &QTimer::timeout, &app, [quota, deny] {
            QString err;
            if (quota->isDirty() && !quota->save(&err))
                qWarning() << "ragoshell-web-gateway:" << qPrintable(err);
            err.clear();
            if (!deny->reload(&err))
                qWarning() << "ragoshell-web-gateway:" << qPrintable(err);
        });
        maintenance->start(60 * 1000);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [quota] {
            QString err;
            if (quota->isDirty() && !quota->save(&err))
                qWarning() << "ragoshell-web-gateway:" << qPrintable(err);
        });
        std::signal(SIGTERM, requestStop);
        std::signal(SIGINT, requestStop);
        auto *stopPoll = new QTimer(&app);
        QObject::connect(stopPoll, &QTimer::timeout, &app, [] {
            if (g_stopRequested)
                QCoreApplication::quit();
        });
        stopPoll->start(250);

        QTextStream(stdout) << "ragoshell-web-gateway: free tier limits: "
                            << maxSessions << " sessions in total, " << maxPerAccount << " session(s) per account, "
                            << sessionMaxSec << " s per session, " << idleSec
                            << " s idle, " << monthlyBytes << " bytes per month; deny list "
                            << (deny->enabled() ? "on" : "off") << "\n";
    }

    QHttpServer http;
    // WebSocket upgrade gate: path + Origin at HTTP level (G1/G3). The
    // session re-checks inside GatewaySession as well when the socket
    // exposes the handshake data.
    http.addWebSocketUpgradeVerifier(&app, [&](const QHttpServerRequest &req) {
        if (req.url().path() != wsPath)
            return QHttpServerWebSocketUpgradeResponse::deny(
                404, QByteArrayLiteral("not a websocket endpoint"));
        const QString origin = QString::fromUtf8(
            req.headers().value("Origin"));
        if (!originAllowed(origin))
            return QHttpServerWebSocketUpgradeResponse::deny(
                403, QByteArrayLiteral("origin not allowed"));
        return QHttpServerWebSocketUpgradeResponse::accept();
    });

    http.route("/", QHttpServerRequest::Method::Get,
               [&](const QHttpServerRequest &req) {
                   return serveFile(root, QStringLiteral("/index.html"), req);
               });
    http.route("/<arg>", QHttpServerRequest::Method::Get,
               [&](const QString &, const QHttpServerRequest &req) {
                   return serveFile(root, req.url().path(), req);
               });
    http.route("/<arg>/<arg>", QHttpServerRequest::Method::Get,
               [&](const QString &, const QString &, const QHttpServerRequest &req) {
                   return serveFile(root, req.url().path(), req);
               });
    http.route("/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Get,
               [&](const QString &, const QString &, const QString &,
                   const QHttpServerRequest &req) {
                   return serveFile(root, req.url().path(), req);
               });
    http.route("/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Get,
               [&](const QString &, const QString &, const QString &,
                   const QString &, const QHttpServerRequest &req) {
                   return serveFile(root, req.url().path(), req);
               });

    auto *tcp = new QTcpServer(&app);
    const QHostAddress bindAddr(bind);
    if (bindAddr.isNull() || !tcp->listen(bindAddr, static_cast<quint16>(port))
        || !http.bind(tcp)) {
        QTextStream(stderr) << "ragoshell-web-gateway: cannot bind " << bind
                            << ":" << port << "\n";
        return 1;
    }

    // Upgraded sockets → the same session stack as the local agent, plus the
    // SSRF egress policy. Qt populates origin()/request() on upgraded
    // sockets (verified), so the session re-enforces the same allowlist —
    // Origin is checked twice: once by the HTTP verifier, once by the gate.
    // Host stays unchecked (requiredHost empty): a public gateway cannot
    // enumerate its own names and the threat model does not need it.
    int sessions = 0;
    auto fullRefusals = std::make_shared<RagoShell::RefusalWindow>();
    QObject::connect(&http, &QAbstractHttpServer::newWebSocketConnection,
                     &app, [&] {
        auto socket = http.nextPendingWebSocketConnection();
        if (!socket)
            return;
        if (sessions >= maxSessions) {
            fullRefusals->add(QDateTime::currentSecsSinceEpoch());
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("too many sessions"));
            return;
        }
        ++sessions;
        auto *session = new RagoShell::GatewaySession(
            socket.release(), allowedOrigins, QString(), token,
            [] { return std::make_unique<RagoShell::NativeMongoTransport>(); },
            &app, &egress, QString(),
            hostedMode ? &hosted : nullptr);
        QObject::connect(session, &RagoShell::GatewaySession::finished,
                         &app, [&sessions, session] {
                             --sessions;
                             session->deleteLater();
                         });
    });

    // Aggregate status for the host check (docs/HostedGateway.md §9.7): once
    // at start and every minute; no account data, so mode 0644.
    if (!statusFile.isEmpty() && hosted.quota) {
        auto quota = hosted.quota;
        const auto writeStatus = [&sessions, fullRefusals, quota, statusFile, maxSessions] {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            const RagoShell::BridgeQuota::Totals t = quota->totals(now);
            RagoShell::BridgeStatus st;
            st.writtenAt = now;
            st.month = t.month;
            st.sessions = sessions;
            st.maxSessions = int(maxSessions);
            st.fullRefusals1h = fullRefusals->lastHour(now);
            st.monthlyBytes = t.bytes;
            st.monthlyLimit = quota->monthlyLimit();
            st.accounts = t.accounts;
            st.accountsExhausted = t.exhausted;
            st.quotaHealthy = quota->healthy();
            QString err;
            if (!RagoShell::writeBridgeStatus(statusFile, st, &err))
                qWarning() << "ragoshell-web-gateway:" << qPrintable(err);
        };
        writeStatus();
        auto *statusTimer = new QTimer(&app);
        QObject::connect(statusTimer, &QTimer::timeout, &app, writeStatus);
        statusTimer->start(60 * 1000);
    } else if (!statusFile.isEmpty()) {
        QTextStream(stderr) << "ragoshell-web-gateway: WARNING — RAGOSHELL_GW_STATUS_FILE "
                               "is used in hosted mode only; ignored\n";
    }

    QTextStream(stdout) << "ragoshell-web-gateway: http+ws on " << bind << ":"
                        << port << " (ws path " << wsPath << ", web root "
                        << webRoot << ", auth " << authMode << ")\n";
    QTextStream(stdout).flush();
    return app.exec();
}
