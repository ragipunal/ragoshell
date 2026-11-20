#include "ragoshell/core/transport/EgressPolicy.h"

#include <QHostInfo>
#include <QStringList>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <mongocxx/uri.hpp>

namespace
{
    // IPv4 ranges that must never be MongoDB targets for a public gateway:
    // loopback, RFC1918, link-local/metadata, CGNAT, IETF-reserved,
    // documentation blocks, benchmarking and multicast+reserved space.
    const QPair<QHostAddress, int> kBlockedV4[] = {
        {QHostAddress(QStringLiteral("0.0.0.0")), 8},
        {QHostAddress(QStringLiteral("10.0.0.0")), 8},
        {QHostAddress(QStringLiteral("100.64.0.0")), 10},
        {QHostAddress(QStringLiteral("127.0.0.0")), 8},
        {QHostAddress(QStringLiteral("169.254.0.0")), 16},
        {QHostAddress(QStringLiteral("172.16.0.0")), 12},
        {QHostAddress(QStringLiteral("192.0.0.0")), 24},
        {QHostAddress(QStringLiteral("192.0.2.0")), 24},
        {QHostAddress(QStringLiteral("192.88.99.0")), 24}, // deprecated 6to4 anycast
        {QHostAddress(QStringLiteral("192.168.0.0")), 16},
        {QHostAddress(QStringLiteral("198.18.0.0")), 15},
        {QHostAddress(QStringLiteral("198.51.100.0")), 24},
        {QHostAddress(QStringLiteral("203.0.113.0")), 24},
        {QHostAddress(QStringLiteral("224.0.0.0")), 3},
    };

    const QPair<QHostAddress, int> kBlockedV6[] = {
        {QHostAddress(QStringLiteral("::")), 96},     // IPv4-compatible tail (::x.y.z.w)
        {QHostAddress(QStringLiteral("::1")), 128},   // loopback
        {QHostAddress(QStringLiteral("64:ff9b::")), 96},  // NAT64 well-known (embeds v4)
        {QHostAddress(QStringLiteral("64:ff9b:1::")), 48}, // NAT64 local-use
        {QHostAddress(QStringLiteral("100::")), 64},  // discard-only
        {QHostAddress(QStringLiteral("2001::")), 32}, // Teredo (embeds v4)
        {QHostAddress(QStringLiteral("2001:2::")), 48}, // benchmarking
        {QHostAddress(QStringLiteral("2001:10::")), 28}, // ORCHIDv1
        {QHostAddress(QStringLiteral("2001:db8::")), 32}, // documentation
        {QHostAddress(QStringLiteral("2002::")), 16}, // 6to4 (embeds v4)
        {QHostAddress(QStringLiteral("fc00::")), 7},  // unique-local
        {QHostAddress(QStringLiteral("fe80::")), 10}, // link-local
        {QHostAddress(QStringLiteral("fec0::")), 10}, // deprecated site-local
        {QHostAddress(QStringLiteral("ff00::")), 8},  // multicast
    };

    // Options the gateway writes itself (libmongoc's lower-case names).
    bool isForcedOption(const QString &lowerKey)
    {
        return lowerKey == QLatin1String("directconnection")
               || lowerKey == QLatin1String("connecttimeoutms")
               || lowerKey == QLatin1String("sockettimeoutms")
               || lowerKey == QLatin1String("serverselectiontimeoutms");
    }

    QString keyOf(const bsoncxx::document::element &el)
    {
        const auto k = el.key();
        return QString::fromUtf8(k.data(), static_cast<qsizetype>(k.size())).toLower();
    }

    template <typename Opt>
    int clampTimeout(const Opt &user, int cap)
    {
        // 0 means "no limit" to the driver - clamp it like any oversize value.
        return (user && *user > 0 && *user < cap) ? int(*user) : cap;
    }

    // Every option the driver parsed except the forced ones, canonical JSON:
    // proof that the rewrite changed nothing the user asked for.
    std::string optionsWithoutForced(const bsoncxx::document::view &opts)
    {
        bsoncxx::builder::basic::document out;
        for (const auto &el : opts) {
            if (!isForcedOption(keyOf(el)))
                out.append(bsoncxx::builder::basic::kvp(el.key(), el.get_value()));
        }
        return bsoncxx::to_json(out.view(), bsoncxx::ExtendedJsonMode::k_canonical);
    }

    // Drops any user spelling of the forced options from the query and
    // appends ours. Only a textual helper: the caller re-parses the result
    // and compares it with the original, so an odd URI (e.g. a raw '?'
    // inside the password) fails closed instead of being misread.
    QString withForcedOptions(const QString &uri, const QStringList &forced)
    {
        const int scheme = uri.indexOf(QLatin1String("://"));
        if (scheme < 0)
            return {};
        const int body = scheme + 3;
        QString base = uri;
        QString query;
        const int q = uri.indexOf(QLatin1Char('?'), body);
        if (q >= 0) {
            base = uri.left(q);
            query = uri.mid(q + 1);
        }
        if (base.indexOf(QLatin1Char('/'), body) < 0)
            base += QLatin1Char('/');
        QStringList parts;
        for (const QString &part : query.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
            if (!isForcedOption(part.section(QLatin1Char('='), 0, 0).toLower()))
                parts << part;
        }
        parts << forced;
        return base + QLatin1Char('?') + parts.join(QLatin1Char('&'));
    }
}

namespace RagoShell
{
    bool EgressPolicy::isPublicAddress(const QHostAddress &addr)
    {
        if (addr.isNull() || addr.isLoopback() || addr.isMulticast())
            return false;

        // IPv4-mapped IPv6 (::ffff:x.y.z.w) — evaluate the embedded v4.
        QHostAddress eff = addr;
        bool ok = false;
        const quint32 v4 = addr.toIPv4Address(&ok);
        if (ok)
            eff = QHostAddress(v4);

        const QPair<QHostAddress, int> *ranges = eff.protocol() == QAbstractSocket::IPv4Protocol
            ? kBlockedV4 : kBlockedV6;
        const int count = eff.protocol() == QAbstractSocket::IPv4Protocol
            ? int(std::size(kBlockedV4)) : int(std::size(kBlockedV6));
        for (int i = 0; i < count; ++i)
            if (eff.isInSubnet(ranges[i].first, ranges[i].second))
                return false;
        return true;
    }

    bool EgressPolicy::checkHost(const QString &host, QString *error) const
    {
        if (_allowPrivate)
            return true;

        QString h = host;
        if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']')))
            h = h.mid(1, h.size() - 2); // IPv6 literal brackets
        if (h.isEmpty()) {
            if (error)
                *error = QStringLiteral("Egress denied: empty target host.");
            return false;
        }

        // IP literal — but only trust the parse when Qt's canonical form
        // round-trips to the input. Qt accepts "0177.0.0.1" as decimal
        // 177.0.0.1 while getaddrinfo/libmongoc read it as octal
        // 127.0.0.1 — and Qt's own resolver uses the same interpretation,
        // so falling through to DNS is NOT safe either. A dotted literal
        // whose parse is ambiguous is denied outright (no legitimate use);
        // non-canonical IPv6 forms have no octal ambiguity and resolve
        // identically in every parser, so they may take the DNS path.
        const QHostAddress literal(h);
        if (!literal.isNull()) {
            if (literal.toString().compare(h, Qt::CaseInsensitive) == 0) {
                if (isPublicAddress(literal))
                    return true;
                if (error)
                    *error = QStringLiteral("Egress denied: %1 is a private or reserved address.").arg(h);
                return false;
            }
            if (h.contains(QLatin1Char('.'))) {
                if (error)
                    *error = QStringLiteral("Egress denied: %1 is an ambiguous IPv4 literal.").arg(h);
                return false;
            }
        }

        const QHostInfo info = QHostInfo::fromName(h);
        const auto addrs = info.addresses();
        if (info.error() != QHostInfo::NoError || addrs.isEmpty()) {
            if (error)
                *error = QStringLiteral("Egress denied: %1 cannot be resolved.").arg(h);
            return false;
        }
        // Round-robin DNS may mix public and private — ALL must be public.
        for (const QHostAddress &a : addrs) {
            if (!isPublicAddress(a)) {
                if (error)
                    *error = QStringLiteral("Egress denied: %1 resolves to %2, a private or reserved address.")
                                 .arg(h, a.toString());
                return false;
            }
        }
        return true;
    }

    bool EgressPolicy::checkMongoUri(const QString &uri, QString *error) const
    {
        QString dialUri;
        return prepareMongoUri(uri, &dialUri, error);
    }

    bool EgressPolicy::prepareMongoUri(const QString &uri, QString *dialUri,
                                       QString *error, QString *clientError) const
    {
        // Policy refusals carry no resolution detail - the client may see
        // them verbatim; DNS/IP detail stays in the server log.
        auto refuse = [&](const QString &message) {
            if (error)
                *error = message;
            if (clientError)
                *clientError = message;
            return false;
        };

        mongocxx::uri parsed;
        try {
            parsed = mongocxx::uri(uri.toStdString());
        } catch (const std::exception &) {
            return refuse(QStringLiteral("Egress denied: unparseable MongoDB URI."));
        }

        // Hosted mode refuses cleartext targets (docs/HostedGateway.md H5).
        // parsed.tls() reflects libmongoc's finalizer: mongodb+srv:// gets
        // tls=true UNLESS the user opted out explicitly - so no scheme
        // exemption here, "?tls=false" on a +srv URI is still denied.
        // Independent of allowPrivate - that flag lifts the IP check only.
        if (_requireTls && !parsed.tls())
            return refuse(QStringLiteral("Egress denied: target must use TLS."));

        const auto hosts = parsed.hosts();
        if (hosts.size() > static_cast<size_t>(kMaxUriHosts))
            return refuse(QStringLiteral("Egress denied: the MongoDB URI lists more than %1 hosts.")
                              .arg(kMaxUriHosts));

        if (!_allowPrivate) {
            // libmongoc does not resolve SRV while parsing (hosts() is empty
            // for +srv), and the SRV answers it dials later would bypass the
            // address check - refuse until the gateway dials through its own
            // checked connector (2026-11-04 review R6).
            if (uri.startsWith(QLatin1String("mongodb+srv://")))
                return refuse(QStringLiteral(
                    "Egress denied: mongodb+srv:// URIs are not supported on public "
                    "gateways yet; use a mongodb:// URI that names one host."));

            // Options that make the driver act with the gateway host's own
            // identity or files (review R5): cloud credentials (MONGODB-AWS
            // reads env/ECS/EC2 metadata), Kerberos, client certificates,
            // CA/CRL files and certificate-check opt-outs.
            const QString mechanism =
                QString::fromStdString(parsed.auth_mechanism()).toUpper();
            if (!mechanism.isEmpty() && mechanism != QLatin1String("SCRAM-SHA-1")
                && mechanism != QLatin1String("SCRAM-SHA-256"))
                return refuse(QStringLiteral(
                    "Egress denied: authMechanism '%1' is not supported on public gateways.")
                                  .arg(mechanism));
            if (const auto props = parsed.auth_mechanism_properties();
                props && !props->empty())
                return refuse(QStringLiteral(
                    "Egress denied: URI option 'authMechanismProperties' is not supported "
                    "on public gateways."));
            for (const auto &el : parsed.options()) {
                const QString key = keyOf(el);
                const bool tlsDetail = (key.startsWith(QLatin1String("tls"))
                                        || key.startsWith(QLatin1String("ssl")))
                                       && key != QLatin1String("tls")
                                       && key != QLatin1String("ssl");
                if (tlsDetail || key == QLatin1String("replicaset")
                    || key == QLatin1String("loadbalanced"))
                    return refuse(QStringLiteral(
                        "Egress denied: URI option '%1' is not supported on public gateways.")
                                      .arg(key));
            }
            if (const auto direct = parsed.direct_connection(); direct && !*direct)
                return refuse(QStringLiteral(
                    "Egress denied: URI option 'directconnection=false' is not supported "
                    "on public gateways."));

            // One host, dialed directly: with several seeds or discovery the
            // driver connects to whatever members a server lists in its
            // hello reply - none of which this check ever saw (review R1).
            if (hosts.empty())
                return refuse(QStringLiteral("Egress denied: MongoDB URI lists no hosts."));
            if (hosts.size() != 1)
                return refuse(QStringLiteral(
                    "Egress denied: public gateways accept exactly one MongoDB host "
                    "(seed lists and replica-set discovery are not supported yet)."));

            QString detail;
            if (!checkHost(QString::fromStdString(std::string(hosts.front().name)), &detail)) {
                if (error)
                    *error = detail;
                if (clientError)
                    *clientError = QStringLiteral("Egress denied.");
                return false;
            }
        }

        const int connectMs = clampTimeout(parsed.connect_timeout_ms(), kConnectTimeoutCapMs);
        const int socketMs = clampTimeout(parsed.socket_timeout_ms(), kSocketTimeoutCapMs);
        const int selectMs = clampTimeout(parsed.server_selection_timeout_ms(),
                                          kServerSelectionTimeoutCapMs);
        QStringList forced{QStringLiteral("connectTimeoutMS=%1").arg(connectMs),
                           QStringLiteral("socketTimeoutMS=%1").arg(socketMs),
                           QStringLiteral("serverSelectionTimeoutMS=%1").arg(selectMs)};
        if (!_allowPrivate)
            forced.prepend(QStringLiteral("directConnection=true"));
        const QString rewritten = withForcedOptions(uri, forced);

        // Re-parse with the driver and prove that only the forced options
        // changed - hosts, credentials, database and every other option must
        // be identical, and the forced values must have landed.
        const QString mismatch = QStringLiteral("Egress denied: unsupported MongoDB URI form.");
        try {
            const mongocxx::uri dialed(rewritten.toStdString());
            const auto dialedHosts = dialed.hosts();
            bool same = dialedHosts.size() == hosts.size();
            for (size_t i = 0; same && i < hosts.size(); ++i)
                same = dialedHosts[i].name == hosts[i].name
                       && dialedHosts[i].port == hosts[i].port;
            same = same && dialed.username() == parsed.username()
                   && dialed.password() == parsed.password()
                   && dialed.auth_source() == parsed.auth_source()
                   && dialed.auth_mechanism() == parsed.auth_mechanism()
                   && dialed.database() == parsed.database()
                   && dialed.replica_set() == parsed.replica_set()
                   && dialed.tls() == parsed.tls()
                   && optionsWithoutForced(dialed.options())
                          == optionsWithoutForced(parsed.options());
            const auto dConnect = dialed.connect_timeout_ms();
            const auto dSocket = dialed.socket_timeout_ms();
            const auto dSelect = dialed.server_selection_timeout_ms();
            same = same && dConnect && *dConnect == connectMs
                   && dSocket && *dSocket == socketMs
                   && dSelect && *dSelect == selectMs;
            if (!_allowPrivate) {
                const auto direct = dialed.direct_connection();
                same = same && direct && *direct;
            }
            if (!same)
                return refuse(mismatch);
        } catch (const std::exception &) {
            return refuse(mismatch);
        }

        if (dialUri)
            *dialUri = rewritten;
        return true;
    }
}
