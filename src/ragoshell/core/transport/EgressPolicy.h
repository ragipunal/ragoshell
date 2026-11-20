#pragma once

#include <QHostAddress>
#include <QString>

namespace RagoShell
{
    /// SSRF guard for the web gateway (docs/WebGateway.md §2, WebAssembly.md
    /// §4 "Bulut Gateway İçin SSRF ve DNS Rebinding Koruması").
    ///
    /// Before a session's `connect` op reaches the transport, every host in
    /// the mongodb URI is resolved via QHostInfo and EVERY resolved address
    /// must be public. Loopback, RFC1918, link-local/metadata (169.254/16,
    /// fe80::/10), CGNAT (100.64/10), ULA (fc00::/7), multicast/reserved and
    /// IPv4-mapped forms are rejected. `allowPrivate` (RAGOSHELL_GW_ALLOW_
    /// PRIVATE=1) disables the range check for self-hosted LAN deploys.
    ///
    /// Note: the agent (ragoshell-agent) deliberately gets NO policy —
    /// reaching localhost MongoDB is its entire purpose.
    class EgressPolicy
    {
    public:
        explicit EgressPolicy(bool allowPrivate) : _allowPrivate(allowPrivate) {}
        bool allowPrivate() const { return _allowPrivate; }

        /// Hosted mode (docs/HostedGateway.md H5): also require TLS on the
        /// target — mongodb:// URIs must carry tls=true/ssl=true;
        /// mongodb+srv:// always qualifies (TLS implied by spec).
        void setRequireTls(bool on) { _requireTls = on; }

        /// The gateway's `connect` gate (2026-11-04 review R1/R3/R5/R6).
        /// Validates the URI and, on success, returns in *dialUri the URI to
        /// hand to the driver: driver timeouts clamped (a smaller user value
        /// wins) and - on a public gateway (allowPrivate off) -
        /// `directConnection=true` forced. A public gateway also requires
        /// exactly one host and refuses mongodb+srv://, replicaSet,
        /// loadBalanced, directConnection=false, every tls*/ssl* option
        /// other than tls/ssl themselves, authMechanismProperties and any
        /// authMechanism except SCRAM-SHA-1/SCRAM-SHA-256; then every
        /// resolved address of the host must be public. Unparseable input
        /// or a rewrite that would change anything but the forced options
        /// fails closed.
        ///
        /// *error gets the server-log detail; *clientError (optional) the
        /// text that is safe to send back: the specific reason for policy
        /// refusals, the generic "Egress denied." when the detail would leak
        /// DNS/IP information.
        bool prepareMongoUri(const QString &uri, QString *dialUri,
                             QString *error, QString *clientError = nullptr) const;

        /// prepareMongoUri() without the rewritten URI.
        bool checkMongoUri(const QString &uri, QString *error) const;

        /// Resolves one host (IP literal or DNS name); all returned
        /// addresses must be public unless allowPrivate.
        bool checkHost(const QString &host, QString *error) const;

        /// True when `addr` is a globally routable, non-reserved address.
        static bool isPublicAddress(const QHostAddress &addr);

        /// More hosts than this are refused before any DNS lookup - each
        /// lookup blocks the shared event loop (review R3).
        static constexpr int kMaxUriHosts = 16;
        /// Driver timeout caps forced into every dialed URI (review R3).
        /// socketTimeoutMS stays above the 50 s maxTimeMS cap so a slow
        /// command ends with the server's own error, not a socket timeout.
        static constexpr int kConnectTimeoutCapMs = 10 * 1000;
        static constexpr int kSocketTimeoutCapMs = 55 * 1000;
        static constexpr int kServerSelectionTimeoutCapMs = 15 * 1000;

    private:
        bool _allowPrivate;
        bool _requireTls = false;
    };
}
