# Supporting RagoShell

**RagoShell** is free and open-source software distributed under the **GNU General Public License v3.0 (GPLv3)**, continuing the open-source Robomongo codebase on top of the Qt 6 Community Edition (LGPLv3 / GPLv3) and the official MongoDB C++ driver.

## Our Free Software Commitment

Every client feature of RagoShell is — and will permanently remain — **100% free and unrestricted**:

- Embedded JavaScript query shell, BSON Tree / Table / Text views, and multi-tab workspace
- Direct, Replica Set, `mongodb+srv`, SSH tunnel, and TLS/SSL connections
- Schema analyzer, visual aggregation pipeline builder, and multi-language query code export
- Optional local read-only Model Context Protocol (`ragoshell-mcp`) server
- Bring-Your-Own-Key (`BYOK`) AI Query Assistant with your own provider API keys

There are no feature locks, trial expirations, telemetry paywalls, or nag screens inside the client. Voluntary donations and sponsorships help cover recurring project costs:

1. **Code-signing certificates & developer memberships** (Windows Authenticode / SmartScreen reputation, Apple Developer Program notarization)
2. **Build, release, and download hosting infrastructure** (`ragoshell.com.tr` and signed multi-platform release artifacts)
3. **Test hardware & compatibility verification** across Windows, macOS, Linux, iPadOS, iOS, and Android

---

## Official Support Channels

> **Current status:** Sponsorship accounts and cold-wallet receive addresses are awaiting maintainer activation (`[PENDING — maintainer account setup]`). Until live handles and PGP-signed addresses are published below, **do not send funds to any third-party account or address claiming to represent RagoShell**.

### 1. Recurring & One-Time Card Sponsorships (Global)

| Channel | Scope | Fee Structure | Official Link / Handle |
|---|---|---|---|
| **GitHub Sponsors** | Global cards (monthly or one-time) | 0% platform fee on individual sponsorships | `[PENDING — maintainer account setup]` |
| **Buy Me a Coffee** | Global & local cards (one-time or membership) | Standard platform + card processing | `[PENDING — maintainer account setup]` |

### 2. Local Support in Türkiye (TRY & Local Cards)

| Channel | Scope | Official Link |
|---|---|---|
| **Local Support Page (`iyziLink` / `Kreosus`)** | Turkish Lira (TRY) & domestic cards | `[PENDING — maintainer account setup]` |

### 3. Direct Cryptocurrency Donations (Non-Custodial)

| Asset | Network / Format | Official Address |
|---|---|---|
| **Bitcoin (BTC)** | Bitcoin Mainnet (Native SegWit) / Lightning | `[PENDING — maintainer account setup]` |
| **USDT / USDC** | Network explicitly specified upon activation | `[PENDING — maintainer account setup]` |
| **Ethereum (ETH)** | Ethereum / L2 explicitly specified upon activation | `[PENDING — maintainer account setup]` |
| **Monero (XMR)** | Monero Mainnet | `[PENDING — maintainer account setup]` |

---

## Security & Anti-Phishing Verification Policy

Open-source repositories are routinely forked, mirrored, and indexed by third-party sites. To protect supporters against address-replacement or impersonation attacks:

1. **Never trust unsigned forks, issues, DMs, or screenshots.** Maintainers will never send wallet addresses or payment links via direct messages, email, chat, or issue comments.
2. **PGP-signed address manifest (`DONATE.txt.asc`).** Before any cryptocurrency address is listed in this file, the maintainer will commit a cleartext PGP-signed manifest (`DONATE.txt.asc`) at the root of the official repository (`https://github.com/ragipunal/ragoshell`) and publish DNS OpenAlias / BIP-353 TXT records under `ragoshell.com.tr`.
3. **Verify before sending:**
   ```bash
   # Import the official maintainer PGP public key published in SECURITY.md, then verify:
   gpg --verify DONATE.txt.asc
   ```
   Confirm that `gpg` reports a **Good signature** matching the maintainer's primary key fingerprint and that the address inside `DONATE.txt.asc` matches character-for-character.

---

## Mobile App Store Policy Note

In accordance with Apple App Store Review Guidelines (§3.1.1 / §3.1.3) and Google Play Payments Policy, external web donation links inside the application (`About` dialog and `Welcome` page footer) are **enabled exclusively on desktop builds** (`Windows`, `macOS`, and `Linux`, gated by `!Platform.isMobile` in QML and `BrandBridge` HTTPS validation) and are omitted from `iOS`, `iPadOS`, and `Android` builds. Any future mobile tip-jar option will use native platform In-App Purchases.

---

*MongoDB is a registered trademark of MongoDB, Inc. RagoShell is not affiliated with or endorsed by MongoDB, Inc.*
