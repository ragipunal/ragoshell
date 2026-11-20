# RagoShell — Update Check, Mandatory Update Gate & Anonymous Usage Pulse

RagoShell combines a calm, unobtrusive **Welcome-page release check** with optional **strictly anonymous aggregate usage statistics** in a single HTTPS request (`POST /v1/release-check`), backed by a zero-dependency Node.js + SQLite web service (`services/release-pulse-api/`).

---

## 1. UX & Privacy Design Principles

1. **Zero Consent Popups / Zero Offline Errors:**
   - The check runs quietly when `WelcomePage` opens **only if** an internet connection is available (`QNetworkInformation::reachability() != Disconnected`).
   - If the user is offline (e.g. working on an air-gapped LAN or airplane mode), no request is sent and no error dialog or warning is ever shown. If connectivity returns during the session, `UpdateService` completes the deferred check silently.
   - Automatic checks are throttled to at most once every **12 hours** (`lastUpdateCheckEpochSec`).
2. **Transparent Preferences Controls (`Preferences › UPDATES & USAGE`):**
   - **Automatically check for updates on startup** (`checkForUpdates`, default `true`)
   - **Share anonymous usage statistics with update checks** (`shareAnonymousUsage`, default `true`)
   - Setting `disableHttpsFeatures: true` in `ragoshell.json` disables all outbound HTTPS checks completely for air-gapped enterprise environments.
   - Users can also trigger a manual check anytime via **Help › About RagoShell › Check for Updates**.
3. **Strictly Non-Personal Payload:**
   - The local `anonymousID` UUID is hashed on-device with salted SHA-256 (`UpdatePulse::deriveInstallHash`, truncated to 32 hex chars) before leaving the client.
   - MongoDB versions are stripped to `major.minor` only (e.g. `7.0.14-enterprise` → `7.0`).
   - **Never collected or transmitted:** hostnames, IP addresses (the server never writes client IPs to SQLite or logs), database names, collection names, usernames, credentials, SSH keys, or query/script text.

---

## 2. Optional vs. Mandatory Update Behavior

### Optional Update (`updateAvailable == true && mandatory == false`)
- Triggered when `appVersion < latestVersion` and `appVersion >= minSupportedVersion` (with `mandatory: false`).
- Displays a non-blocking `RagoGlassCard` banner on `WelcomePage.qml` with:
  - `NEW VERSION` badge and `latestVersion`
  - Localized `releaseNotes` (matching active UI language with fallback to `"en"`)
  - Primary **Download for `<Platform>`** button (`Windows`, `macOS`, `Linux`, `Google Play`, `App Store`) opening the validated `https://` download URL for the running OS
  - **Later** button that dismisses the card for that specific `latestVersion` (re-appearing automatically when a newer version is released).

### Mandatory Update (`mandatory == true` or `appVersion < minSupportedVersion`)
- Triggered when either:
  1. `appVersion < minSupportedVersion` (e.g. versions older than `2.1.0` must upgrade due to a critical security or protocol change), OR
  2. `mandatory: true` is set on the server (globally or per-platform in `downloads.<platform>.mandatory`) while `appVersion < latestVersion`.
- Opens `MandatoryUpdateDialog.qml`:
  - Modal, non-dismissible (`closable: false`, `Popup.NoAutoClose`, no `✕` close button, `Esc` disabled).
  - Global workspace keyboard shortcuts (`MainShortcuts.qml`) are disabled except `Quit`.
  - Provides **Download for `<Platform>`**, **Copy Link**, and (on desktop) **Exit**.
  - **Offline restart persistence:** `UpdateService` caches `cachedMinSupportedVersion` and `cachedMandatoryForAppVersion` in `ragoshell.json` so restarting offline cannot bypass a known mandatory upgrade; the lock clears automatically as soon as the upgraded binary (`appVersion >= minSupportedVersion`) starts or when the server clears the `mandatory` flag.

---

## 3. Wire Protocol (`POST /v1/release-check`)

### Request Body
```json
{
  "schemaVersion": 1,
  "appVersion": "2.0.0",
  "platform": "macos",
  "arch": "arm64",
  "formFactor": "desktop",
  "osVersion": "15.4",
  "uiLanguage": "tr",
  "usage": {
    "installId": "9f86d081884c7d659a2feaa0c55ad015",
    "launchCount": 14,
    "lastSessionSec": 1820,
    "savedConnections": 4,
    "sshConnections": 1,
    "tlsConnections": 2,
    "replicaSetConnections": 1,
    "shellsOpened": 28,
    "queriesExecuted": 145,
    "aiEnabled": false,
    "uiTheme": "System",
    "uiDensity": "Automatic",
    "dbVersions": ["6.0", "7.0", "8.0"]
  }
}
```
*(When `shareAnonymousUsage` is unchecked, the `"usage"` object is omitted completely.)*

### Response Body (`releases.json` — produced by `ci/generate-releases-json.mjs`, see `docs/ReleasePipeline.md`)
```json
{
  "schemaVersion": 1,
  "latestVersion": "2.1.0",
  "minSupportedVersion": "1.8.0",
  "mandatory": false,
  "publishedAt": "2026-10-30",
  "allDownloadsUrl": "https://dl.ragoshell.com.tr/releases/",
  "releaseNotes": {
    "en": "MongoDB 8.0 shell improvements and SSH stability fixes.",
    "tr": "MongoDB 8.0 kabuk iyileştirmeleri ve SSH kararlılık düzeltmeleri."
  },
  "downloads": {
    "windows": {
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-windows-x64-setup.exe",
      "label": "Windows",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "size": 78643200
    },
    "macos": {
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-macos-arm64.dmg",
      "label": "macOS",
      "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
      "size": 83886080
    },
    "linux": {
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-linux-x64.AppImage",
      "label": "Linux",
      "sha256": "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
      "size": 90177536
    },
    "android": {
      "url": "https://play.google.com/store/apps/details?id=tr.com.ragoshell.app",
      "label": "Google Play"
    },
    "ios": {
      "url": "https://apps.apple.com/app/ragoshell/id0000000000",
      "label": "App Store"
    }
  },
  "artifacts": [
    {
      "name": "ragoshell-2.1.0-windows-x64-setup.exe",
      "platform": "windows",
      "arch": "x64",
      "format": "exe",
      "primary": true,
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-windows-x64-setup.exe",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "size": 78643200
    }
  ]
}
```
Each `downloads.<platform>` entry may also specify optional per-platform `latestVersion`, `minSupportedVersion`, and `mandatory` fields (useful when App Store or Google Play review rolls out on a different day than desktop installers). The same `releases.json` (accompanied by `releases.json.minisig`, signed via `ci/sign-release.sh`) is served by Cloudflare R2 (`https://dl.ragoshell.com.tr/releases.json`), `ragoshell-site` (`/releases.json`), and `services/release-pulse-api/`; Phase 5.1 (A3) adds client-side Ed25519 + SHA-256 verification before launching any downloaded update (`docs/ReleasePipeline.md` §5).

---

## 4. Deploying the Self-Hosted Web Service (`services/release-pulse-api/`)

The service requires **Node.js 22.5+ / 24+** (`node:sqlite` built-in) and **zero npm packages**.

### Environment Variables
| Variable | Default | Description |
|---|---|---|
| `PULSE_BIND_HOST` | `127.0.0.1` | Listen address (keep `127.0.0.1` behind a local reverse proxy such as Caddy or Nginx) |
| `PULSE_PORT` | `8787` | Listen TCP port |
| `PULSE_DB_PATH` | `./data/pulse.sqlite` | SQLite database file (`WAL` mode) |
| `PULSE_RELEASES_PATH` | `./releases.json` | Release manifest JSON file |
| `PULSE_ADMIN_TOKEN` | *(required in prod)* | Secret token (`>= 16` chars) for `/v1/admin/*` endpoints |
| `PULSE_ADMIN_TOKEN_FILE` | *(optional)* | Path to file containing the admin token (for Docker/systemd secrets) |

### Running & Querying Statistics
```bash
# Generate a strong admin token and start locally:
export PULSE_ADMIN_TOKEN="$(openssl rand -hex 32)"
node services/release-pulse-api/server.mjs

# Query DAU/WAU/MAU, platform/OS/language breakdown, SSH/TLS/AI adoption, and MongoDB versions:
curl -s -H "Authorization: Bearer $PULSE_ADMIN_TOKEN" \
  http://127.0.0.1:8787/v1/admin/stats | jq .

# Publish a new release or trigger a mandatory update live without restarting:
curl -s -X POST http://127.0.0.1:8787/v1/admin/releases \
  -H "Authorization: Bearer $PULSE_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d @services/release-pulse-api/releases.json
```

### Example Caddy HTTPS Reverse Proxy
```caddyfile
api.ragoshell.com.tr {
    reverse_proxy 127.0.0.1:8787
}
```
Clients default to `https://api.ragoshell.com.tr/v1/release-check` and can override the endpoint for local testing via `RAGOSHELL_UPDATE_URL=http://127.0.0.1:8787/v1/release-check`. Packages are served from `https://dl.ragoshell.com.tr/releases/<version>/`. Until the service runs there, a check fails quietly (no message unless the user pressed "Check for Updates").

---

## 5. App Store & Google Play Store Privacy Declarations

- **Apple App Store (`install/ios/PrivacyInfo.xcprivacy`):**
  - `NSPrivacyTracking = false` (no ATT prompt, zero cross-app tracking).
  - `NSPrivacyCollectedDataTypeProductInteraction` (`Linked = false`, `Tracking = false`, purposes: `Analytics`, `AppFunctionality`).
- **Google Play Console (Data Safety Form):**
  - *App activity → App interactions*: Collected, **Ephemeral / Anonymous (not linked to user identity)**, **Optional (user can turn off in Preferences)**, encrypted in transit over TLS, never shared with third parties.
