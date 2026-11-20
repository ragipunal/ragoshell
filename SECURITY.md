# Security Policy

RagoShell handles database connections, SSH tunnels, TLS certificates, OS-backed credential storage, and an embedded JavaScript shell. We take the security of our users and their databases seriously and welcome coordinated vulnerability reports from security researchers and the open-source community.

> [!IMPORTANT]
> **Do NOT open a public GitHub issue, pull request, or discussion to report a security vulnerability.** Publicly posting unpatched vulnerability details, exploit proof-of-concepts, memory-corruption reproducers, or credential leaks puts users at immediate risk before a fix can be built, signed, and distributed.

---

## Supported Versions

Security fixes are backported **only to the latest stable release branch**. Users running older builds or legacy unmaintained codebases should upgrade to the current stable release before reporting non-regression issues.

| Version | Supported | Notes |
|---|---|---|
| **2.1.x (latest stable branch / `main`)** | **Yes** | Active development and security updates across Windows, macOS, Linux, iOS/iPadOS, and Android |
| `< 2.1.0` | No | Upgrade to the latest `2.1.x` release |
| Legacy Robomongo `1.4.x` and earlier | No | End-of-life upstream codebase (2021); superseded by RagoShell |

When downloading prebuilt releases, verify both SHA-256 checksums (`SHA256SUMS.txt`) and Ed25519 `minisign` signatures (`releases.json.minisig` and `SHA256SUMS.txt.minisig`) (see `docs/ReleasePipeline.md`).

---

## Reporting a Vulnerability

Please report suspected vulnerabilities privately through one of the official channels below, consistent with our RFC 9116 policy at [`https://ragoshell.com.tr/.well-known/security.txt`](https://ragoshell.com.tr/.well-known/security.txt):

### 1. GitHub Private Vulnerability Reporting (Preferred)

Submit your report privately via **GitHub Security Advisories**:

- **URL:** `https://github.com/ragipunal/ragoshell/security/advisories/new`

This provides an encrypted, private workspace to share reproduction details, review patches, and coordinate advisory publication and CVE assignment.

### 2. Dedicated Security Mailbox

- **Email:** [`security@ragoshell.com.tr`](mailto:security@ragoshell.com.tr)
- **PGP / `age` Encryption:** Available on request via `security@ragoshell.com.tr` prior to sending sensitive exploit material; once published, canonical key fingerprints will also appear here and in `https://ragoshell.com.tr/.well-known/security.txt`.

### 3. RFC 9116 `security.txt`

- **Canonical `security.txt`:** `https://ragoshell.com.tr/.well-known/security.txt`

### What to Include in Your Private Report

Once inside a private channel, please include:

1. **Affected component & version:** Exact RagoShell version or git commit SHA, package format (`.exe`, `.msi`, `.zip`, `.dmg`, `.AppImage`, `.tar.gz`, `.apk`, `.aab`, or source build), and target OS / architecture.
2. **Vulnerability class & impact:** What an attacker can achieve, required preconditions (e.g., network MITM, malicious MongoDB server response, crafted script/import file, local MCP client), and realistic severity.
3. **Minimal reproduction steps:** Clear step-by-step instructions or a self-contained, benign proof-of-concept using synthetic test data (never real credentials or third-party data).
4. **Suggested mitigation (optional):** Any analysis of the root cause in `src/` or `ci/`.

---

## Response Targets & 90-Day Coordinated Vulnerability Disclosure

We follow a **90-day Coordinated Vulnerability Disclosure (CVD)** policy:

| Milestone | Target SLA |
|---|---|
| **Initial acknowledgment** | Within **72 hours** (3 calendar days) |
| **Triage, validation & severity assessment** | Within **7 days** |
| **Progress updates during remediation** | At least every **14 days** until resolved |
| **Patch release & coordinated public disclosure** | Within **90 days** of initial acknowledgment |

- **Disclosure coordination:** Once a patch is released across affected platforms, we publish a security advisory detailing affected versions, remediation steps, and researcher credit (unless you prefer to remain anonymous).
- **Complex multi-platform fixes:** If simultaneous OS code-signing, notarization, or mobile store review requires additional time near the 90-day mark, we may request a mutually agreed grace extension (typically 14 days). Vulnerabilities under active in-the-wild exploitation are prioritized for emergency out-of-band release.
- **Good-faith safe harbor:** We welcome good-faith security research conducted in compliance with this policy that avoids privacy violations, service disruption (DoS/DDoS), spam, and unauthorized access to data belonging to third parties.

---

## Scope

### In Scope

- **Desktop & mobile client (`ragoshell`, `rago-core`, `src/ragoshell-app/`):** Memory safety, command/URI injection, path traversal in collection import/export (`src/ragoshell/`), UI spoofing, and platform sandboxing on Windows x64, macOS arm64, Linux x64, iOS/iPadOS arm64, and Android arm64 (`docs/Platforms.md`).
- **Embedded JavaScript shell (`src/ragoshell/core/engine/`):** Sandbox/host boundary bugs in `QJSEngine`, `ShellHost`, BSON serialization/deserialization, or query/timeout interruption (`docs/Shell.md`).
- **Credential storage & transport security (`KeyStore`, SSH & TLS):** OS credential store integration (Windows Credential Manager, macOS/iOS Keychain, Linux Secret Service / `libsecret`, Android Keystore AES-256-GCM), mobile `0600` imported credential isolation, SSH `known_hosts` host-key verification, and TLS certificate/hostname validation (`docs/Platforms.md`).
- **Update & release verification (`UpdateService`, `UpdatePulse`, `minisign`):** Signature verification bypasses, downgrade/rollback attacks, HTTPS or download-host allowlist bypasses, or unintended data leakage in `POST /v1/release-check` (`docs/UpdateAndTelemetry.md`, `docs/ReleasePipeline.md`).
- **Read-only MCP server (`ragoshell-mcp`):** Bypass of the opt-in connection allowlist, bypass of read-only query enforcement (executing write/admin mutations through MCP), or local privilege escalation (`docs/McpServer.md`).
- **Official installers, packaging & CI workflows:** Insecure file permissions, DLL/dylib search-order hijacking in installers (`install/`), or workflow injection in `ci/` and `.github/workflows/`.
- **First-party web & update services:** `ragoshell.com.tr`, `dl.ragoshell.com.tr`, `services/release-pulse-api/`, `app.ragoshell.com.tr`, `api.ragoshell.com.tr`, and `bridge.ragoshell.com.tr`.

### Out of Scope

- **Intentional local shell execution by the authenticated user:** RagoShell is an administrative MongoDB client; commands or scripts that a user deliberately types or opens and executes in their own local shell tab against databases they control are expected product functionality, not a sandbox escape.
- **Same-UID local OS compromise:** Attacks that assume the adversary already runs arbitrary code under the same operating-system user account (note: when Linux lacks a running Secret Service daemon or when `RAGOSHELL_KEYSTORE=file` is explicitly set, the fallback `ragoshell.key` file behavior is documented in `docs/Platforms.md` and surfaced as a warning in Preferences).
- **Misconfigured user MongoDB deployments:** Unauthenticated or unencrypted MongoDB servers operated by end users.
- **Pure upstream third-party bugs without a RagoShell-specific vector:** Bugs strictly within upstream `Qt`, `mongo-c-driver` / `mongo-cxx-driver`, `OpenSSL`, `libssh2`, or `QScintilla` that are unexploitable in RagoShell's configuration should be reported directly to the upstream maintainer (though you are welcome to notify us once an upstream patch is available so we can update our `vcpkg` / bundled dependencies).
- **Denial-of-service, spam, physical, or social-engineering attacks:** Volumetric traffic tests against `ragoshell.com.tr` and its subdomains, phishing maintainers, or testing third-party providers (`Cloudflare`, `GitHub`, `Paddle`, `iyzico`).

---

*MongoDB is a registered trademark of MongoDB, Inc. RagoShell is not affiliated with or endorsed by MongoDB, Inc.*
