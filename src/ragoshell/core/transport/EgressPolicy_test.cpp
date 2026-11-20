#include "gtest/gtest.h"

#include "ragoshell/core/transport/EgressPolicy.h"

#include <QStringList>

#include <mongocxx/uri.hpp>

using namespace RagoShell;

// SSRF egress policy (docs/WebGateway.md §2/G1): every host in a MongoDB URI
// must resolve to a public address before the gateway dials out.

TEST(EgressPolicyTest, private_ipv4_ranges_rejected)
{
    EgressPolicy p(false);
    QString err;
    for (const char *h : {"127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.1",
                          "192.168.1.1", "169.254.169.254", "100.64.0.1",
                          "0.0.0.0", "198.51.100.7"}) {
        EXPECT_FALSE(p.checkHost(QString::fromLatin1(h), &err)) << h;
        EXPECT_FALSE(err.isEmpty()) << h;
    }
}

TEST(EgressPolicyTest, private_ipv6_and_mapped_rejected)
{
    EgressPolicy p(false);
    QString err;
    EXPECT_FALSE(p.checkHost(QStringLiteral("::1"), &err));
    EXPECT_FALSE(p.checkHost(QStringLiteral("[::1]"), &err));
    EXPECT_FALSE(p.checkHost(QStringLiteral("fe80::1"), &err));
    EXPECT_FALSE(p.checkHost(QStringLiteral("fd00::1"), &err));   // ULA
    EXPECT_FALSE(p.checkHost(QStringLiteral("::ffff:127.0.0.1"), &err)); // mapped
    EXPECT_FALSE(p.checkHost(QStringLiteral("::ffff:10.0.0.1"), &err));
}

TEST(EgressPolicyTest, public_addresses_allowed)
{
    EgressPolicy p(false);
    QString err;
    EXPECT_TRUE(p.checkHost(QStringLiteral("8.8.8.8"), &err)) << err.toStdString();
    EXPECT_TRUE(p.checkHost(QStringLiteral("1.1.1.1"), &err)) << err.toStdString();
    EXPECT_TRUE(p.checkHost(QStringLiteral("2606:4700:4700::1111"), &err))
        << err.toStdString();
}

TEST(EgressPolicyTest, localhost_name_rejected)
{
    EgressPolicy p(false);
    QString err;
    // DNS name → resolves to loopback → denied (the DNS-aware path).
    EXPECT_FALSE(p.checkHost(QStringLiteral("localhost"), &err));
}

TEST(EgressPolicyTest, unresolvable_fails_closed)
{
    EgressPolicy p(false);
    QString err;
    EXPECT_FALSE(p.checkHost(QStringLiteral(""), &err));
    EXPECT_FALSE(p.checkHost(
        QStringLiteral("definitely-not-a-real-host.invalid.example"), &err));
}

TEST(EgressPolicyTest, allow_private_bypasses)
{
    EgressPolicy p(true);
    QString err;
    EXPECT_TRUE(p.checkHost(QStringLiteral("127.0.0.1"), &err));
    EXPECT_TRUE(p.checkHost(QStringLiteral("10.0.0.5"), &err));
    EXPECT_TRUE(p.checkMongoUri(QStringLiteral("mongodb://127.0.0.1:27017"), &err));
}

TEST(EgressPolicyTest, mongo_uri_hosts_checked)
{
    EgressPolicy p(false);
    QString err;
    EXPECT_FALSE(p.checkMongoUri(QStringLiteral("mongodb://127.0.0.1:27017"), &err));
    EXPECT_FALSE(p.checkMongoUri(
        QStringLiteral("mongodb://user:pass@10.0.0.9:27017/db?authSource=admin"), &err));
    // One private member in a seedlist poisons the whole URI.
    EXPECT_FALSE(p.checkMongoUri(
        QStringLiteral("mongodb://8.8.8.8:27017,192.168.0.1:27017/?replicaSet=rs"), &err));
    EXPECT_TRUE(p.checkMongoUri(QStringLiteral("mongodb://8.8.8.8:27017"), &err))
        << err.toStdString();
    EXPECT_FALSE(p.checkMongoUri(QStringLiteral("not a uri"), &err));
    EXPECT_FALSE(p.checkMongoUri(QStringLiteral(""), &err));
}

TEST(EgressPolicyTest, noncanonical_ip_literals_rejected)
{
    // Qt parses "0177.0.0.1" as decimal 177.0.0.1 while getaddrinfo /
    // libmongoc read it as octal 127.0.0.1 — the literal path must not
    // trust Qt's interpretation, so these resolve (or fail) closed.
    EgressPolicy p(false);
    QString err;
    EXPECT_FALSE(p.checkHost(QStringLiteral("0177.0.0.1"), &err));
    EXPECT_FALSE(p.checkHost(QStringLiteral("012.0.0.1"), &err));
    EXPECT_FALSE(p.checkHost(QStringLiteral("010.0.0.4"), &err));
    // Canonical public literals still take the fast path.
    EXPECT_TRUE(p.checkHost(QStringLiteral("8.8.8.8"), &err)) << err.toStdString();
}

TEST(EgressPolicyTest, v6_transition_and_site_local_rejected)
{
    // Mechanisms that embed or tunnel IPv4 plus legacy site-local space —
    // a public-target policy never dials them (docs/WebGateway.md §2).
    EgressPolicy p(false);
    QString err;
    EXPECT_FALSE(p.checkHost(QStringLiteral("64:ff9b::7f00:1"), &err));   // NAT64 WKP
    EXPECT_FALSE(p.checkHost(QStringLiteral("2002:7f00:1::"), &err));     // 6to4
    EXPECT_FALSE(p.checkHost(QStringLiteral("2001::7f00:1"), &err));      // Teredo
    EXPECT_FALSE(p.checkHost(QStringLiteral("::7f00:1"), &err));          // v4-compatible
    EXPECT_FALSE(p.checkHost(QStringLiteral("fec0::1"), &err));           // site-local
    EXPECT_FALSE(p.checkHost(QStringLiteral("2001:2::1"), &err));         // benchmarking
    EXPECT_FALSE(p.checkHost(QStringLiteral("192.88.99.1"), &err));       // 6to4 anycast
}

TEST(EgressPolicyTest, require_tls_enforces_tls_targets)
{
    // Hosted mode: plaintext Mongo targets are refused before any address
    // check (docs/HostedGateway.md §H5). requireTls is independent from
    // allowPrivate — a LAN deployment may still require TLS.
    EgressPolicy p(/*allowPrivate=*/true);
    p.setRequireTls(true);
    QString err;
    EXPECT_FALSE(p.checkMongoUri(QStringLiteral("mongodb://8.8.8.8:27017"), &err));
    EXPECT_FALSE(p.checkMongoUri(
        QStringLiteral("mongodb://127.0.0.1:27017/db"), &err)); // private AND plaintext
    EXPECT_TRUE(p.checkMongoUri(
        QStringLiteral("mongodb://127.0.0.1:27017/db?tls=true"), &err))
        << err.toStdString(); // private allowed here, TLS present
    EXPECT_TRUE(p.checkMongoUri(
        QStringLiteral("mongodb+srv://cluster0.example.mongodb.net/db"), &err))
        << err.toStdString(); // srv implies TLS
    EXPECT_FALSE(p.checkMongoUri(
        QStringLiteral("mongodb+srv://cluster0.example.mongodb.net/?tls=false"), &err));
    EXPECT_FALSE(p.checkMongoUri(
        QStringLiteral("mongodb+srv://cluster0.example.mongodb.net/?ssl=false"), &err));
    EXPECT_TRUE(p.checkMongoUri(
        QStringLiteral("mongodb://8.8.8.8:27017/?ssl=true"), &err))
        << err.toStdString(); // legacy ssl option accepted
    EXPECT_FALSE(p.checkMongoUri(
        QStringLiteral("mongodb://8.8.8.8:27017/?tls=false"), &err)); // explicit opt-out

    // Default mode keeps the pre-8.5 semantics: plaintext public target ok.
    EgressPolicy open(false);
    EXPECT_TRUE(open.checkMongoUri(QStringLiteral("mongodb://8.8.8.8:27017"), &err))
        << err.toStdString();
}

// --- 2026-11-04 review (docs/BridgeSecurityReview.md §6) -----------------

TEST(EgressPolicyTest, public_gateway_forces_direct_connection_and_clamps_timeouts)
{
    // R1/R3: the driver gets a rewritten URI — directConnection=true and
    // clamped timeouts (0 = "no limit" is clamped; a smaller value wins) —
    // with credentials, database and every other option untouched.
    EgressPolicy p(false);
    QString dial, err, client;
    ASSERT_TRUE(p.prepareMongoUri(
        QStringLiteral("mongodb://alice:s%40cret@8.8.8.8:27017/shop"
                       "?connectTimeoutMS=999999999&socketTimeoutMS=0"
                       "&serverSelectionTimeoutMS=5000&appName=rago&directConnection=true"),
        &dial, &err, &client)) << err.toStdString();

    const mongocxx::uri u(dial.toStdString());
    ASSERT_EQ(u.hosts().size(), 1u);
    EXPECT_EQ(u.hosts().front().name, "8.8.8.8");
    EXPECT_EQ(u.hosts().front().port, 27017);
    EXPECT_EQ(u.username(), "alice");
    EXPECT_EQ(u.password(), "s@cret");
    EXPECT_EQ(u.database(), "shop");
    ASSERT_TRUE(u.appname());
    EXPECT_EQ(std::string(*u.appname()), "rago");
    ASSERT_TRUE(u.direct_connection());
    EXPECT_TRUE(*u.direct_connection());
    ASSERT_TRUE(u.connect_timeout_ms());
    EXPECT_EQ(*u.connect_timeout_ms(), EgressPolicy::kConnectTimeoutCapMs);
    ASSERT_TRUE(u.socket_timeout_ms());
    EXPECT_EQ(*u.socket_timeout_ms(), EgressPolicy::kSocketTimeoutCapMs);
    ASSERT_TRUE(u.server_selection_timeout_ms());
    EXPECT_EQ(*u.server_selection_timeout_ms(), 5000);

    // No options at all: the forced ones are appended after a '/'.
    ASSERT_TRUE(p.prepareMongoUri(QStringLiteral("mongodb://8.8.8.8"), &dial, &err))
        << err.toStdString();
    const mongocxx::uri bare(dial.toStdString());
    ASSERT_TRUE(bare.server_selection_timeout_ms());
    EXPECT_EQ(*bare.server_selection_timeout_ms(), EgressPolicy::kServerSelectionTimeoutCapMs);
}

TEST(EgressPolicyTest, public_gateway_refuses_discovery)
{
    // R1: seed lists, replicaSet, loadBalanced and directConnection=false
    // would let a server's hello reply steer the driver to unchecked hosts.
    EgressPolicy p(false);
    QString dial, err, client;
    for (const char *uri : {"mongodb://8.8.8.8:27017/?replicaSet=rs0",
                            "mongodb://8.8.8.8:27017/?directConnection=false",
                            "mongodb://8.8.8.8:27017/?loadBalanced=true",
                            "mongodb://8.8.8.8:27017,1.1.1.1:27017/"}) {
        client.clear();
        EXPECT_FALSE(p.prepareMongoUri(QString::fromLatin1(uri), &dial, &err, &client)) << uri;
        // A policy refusal, not the generic DNS/IP denial.
        EXPECT_NE(client, QStringLiteral("Egress denied.")) << uri;
        EXPECT_FALSE(client.isEmpty()) << uri;
    }

    // A private literal still gets the generic client text (no IP oracle).
    client.clear();
    EXPECT_FALSE(p.prepareMongoUri(QStringLiteral("mongodb://10.0.0.9:27017"),
                                   &dial, &err, &client));
    EXPECT_EQ(client, QStringLiteral("Egress denied."));
    EXPECT_TRUE(err.contains(QStringLiteral("10.0.0.9"))); // detail stays server-side

    // A LAN gateway (allowPrivate) keeps discovery but still clamps timeouts.
    EgressPolicy lan(true);
    ASSERT_TRUE(lan.prepareMongoUri(
        QStringLiteral("mongodb://10.0.0.1:27017,10.0.0.2:27017/?replicaSet=rs0&socketTimeoutMS=0"),
        &dial, &err)) << err.toStdString();
    const mongocxx::uri u(dial.toStdString());
    EXPECT_EQ(u.replica_set(), "rs0");
    EXPECT_FALSE(u.direct_connection());
    ASSERT_TRUE(u.socket_timeout_ms());
    EXPECT_EQ(*u.socket_timeout_ms(), EgressPolicy::kSocketTimeoutCapMs);
}

TEST(EgressPolicyTest, public_gateway_refuses_srv)
{
    // R6: SRV answers would be dialed unchecked — refused with a clear
    // reason until the gateway has its own checked connector.
    EgressPolicy p(false);
    p.setRequireTls(true);
    QString dial, err, client;
    EXPECT_FALSE(p.prepareMongoUri(
        QStringLiteral("mongodb+srv://cluster0.example.mongodb.net/db"), &dial, &err, &client));
    EXPECT_TRUE(client.contains(QStringLiteral("not supported on public gateways")))
        << client.toStdString();

    // A LAN gateway may still use SRV (no address check there).
    EgressPolicy lan(true);
    EXPECT_TRUE(lan.prepareMongoUri(
        QStringLiteral("mongodb+srv://cluster0.example.mongodb.net/db"), &dial, &err))
        << err.toStdString();
}

TEST(EgressPolicyTest, host_count_capped_before_dns)
{
    // R3: every lookup blocks the shared loop — more than kMaxUriHosts
    // hosts are refused before the first one is resolved. The .invalid
    // names would fail DNS; the message proves no lookup happened first.
    QStringList hosts;
    for (int i = 0; i <= EgressPolicy::kMaxUriHosts; ++i)
        hosts << QStringLiteral("h%1.invalid:27017").arg(i);
    const QString uri = QStringLiteral("mongodb://") + hosts.join(QLatin1Char(','));
    QString dial, err;
    for (bool allowPrivate : {false, true}) {
        EgressPolicy p(allowPrivate);
        EXPECT_FALSE(p.prepareMongoUri(uri, &dial, &err));
        EXPECT_TRUE(err.contains(QStringLiteral("more than 16 hosts"))) << err.toStdString();
    }
}

TEST(EgressPolicyTest, public_gateway_option_allowlist)
{
    // R5: options that make the driver act with the gateway host's own
    // credentials or files, or skip certificate checks, are refused.
    EgressPolicy p(false);
    QString dial, err, client;
    for (const char *uri : {
             "mongodb://8.8.8.8:27017/?authMechanism=MONGODB-AWS",
             "mongodb://user@8.8.8.8:27017/?authMechanism=GSSAPI",
             "mongodb://user:pw@8.8.8.8:27017/?authMechanism=PLAIN",
             "mongodb://8.8.8.8:27017/?authMechanism=MONGODB-X509&tls=true",
             "mongodb://8.8.8.8:27017/?tls=true&tlsCAFile=/etc/passwd",
             "mongodb://8.8.8.8:27017/?tls=true&tlsCertificateKeyFile=/etc/key.pem",
             "mongodb://8.8.8.8:27017/?tls=true&tlsInsecure=true",
             "mongodb://8.8.8.8:27017/?tls=true&tlsAllowInvalidCertificates=true",
             "mongodb://8.8.8.8:27017/?tls=true&tlsAllowInvalidHostnames=true",
         }) {
        EXPECT_FALSE(p.prepareMongoUri(QString::fromLatin1(uri), &dial, &err, &client)) << uri;
    }
    client.clear();
    EXPECT_FALSE(p.prepareMongoUri(
        QStringLiteral("mongodb://8.8.8.8:27017/?authMechanism=MONGODB-AWS"), &dial, &err, &client));
    EXPECT_TRUE(client.contains(QStringLiteral("authMechanism"))) << client.toStdString();
    client.clear();
    EXPECT_FALSE(p.prepareMongoUri(
        QStringLiteral("mongodb://8.8.8.8:27017/?tls=true&tlsCAFile=/etc/passwd"),
        &dial, &err, &client));
    EXPECT_TRUE(client.contains(QStringLiteral("tlscafile"))) << client.toStdString();

    // SCRAM, no mechanism and plain tls=true stay allowed.
    for (const char *uri : {"mongodb://user:pw@8.8.8.8:27017/?authMechanism=SCRAM-SHA-256",
                            "mongodb://user:pw@8.8.8.8:27017/?authMechanism=SCRAM-SHA-1",
                            "mongodb://user:pw@8.8.8.8:27017/admin",
                            "mongodb://8.8.8.8:27017/?tls=true"}) {
        EXPECT_TRUE(p.prepareMongoUri(QString::fromLatin1(uri), &dial, &err)) << uri
            << " " << err.toStdString();
    }
}
