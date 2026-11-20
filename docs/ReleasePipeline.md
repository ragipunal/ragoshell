# Yayın Hattı, `releases.json` Şeması, `licenses/` Paketi, SPDX 2.3 SBOM, `minisign` İmzalama ve Dağıtım Mimarisi (Faz 3.6 & 3.7)

Yazılış: 2026-11-01 (Güncelleme: 2026-11-02 — Faz 3.7 A5/E6 & Faz 3.8 Android APK/AAB). Bu belge, yol haritası Faz 3.6 (`releases.json`
üretimi, Ed25519 / `minisign` imzalama ve Cloudflare R2 + GitHub Releases + Gitea Releases yayın hattı) ve
Faz 3.7 (`A5` CMake `licenses/` paketi + GPLv3 §6 kaynak kodu bildirimi ve `E6` deterministik SPDX 2.3 JSON
SBOM + OSV-Scanner bağımlılık güvenlik taraması) kapsamındaki **tasarım, tehdit modeli, operasyon ve Faz 5.1
A3 istemci doğrulama sözleşmesidir** (`ragoshell-site` dış site `docs.publish` izin listesinde yer almaz).

---

## 1. Mimari Özeti ve Tek Kaynak İlkesi

Yayın hattı dört platform derleme işinin (`windows`, `macos`, `linux`, `android`) ürettiği ikili paketleri
(`dist/`) toplar, kriptografik özetlerini ve manifest dosyasını sıfır dış bağımlılıklı Node.js betiğiyle üretir,
`minisign` (Ed25519) ile imzalayıp doğrular ve çok hedefli olarak yayınlar:

```text
cmake --install --component licenses  ──►  licenses/ (kök + src/third-party + vcpkg <port>-COPYRIGHT)
                                           └── ci/generate-sbom.mjs --verify-licenses (paketleme öncesi kapı)

dist/ragoshell-<ver>-*  ──►  ci/generate-sbom.mjs
                             └── dist/ragoshell-<ver>-sbom.spdx.json (SPDX 2.3 JSON, purl + cpe23Type + SHA-256)
                                        │
                                        ▼
                             ci/generate-releases-json.mjs
                             ├── dist/SHA256SUMS.txt (tüm paketler + *-sbom.spdx.json)
                             └── dist/releases.json  (downloads + artifacts + sbom)
                                        │
                                        ▼
                             ci/sign-release.sh (minisign Ed25519 + trusted comment)
                             ├── dist/SHA256SUMS.txt.minisig   ──► minisign -Vm + trusted comment denetimi
                             └── dist/releases.json.minisig    ──► minisign -Vm + trusted comment denetimi
                                        │
                                        ▼  (yalnız tüm doğrulamalar geçerse)
                             ci/publish-release.sh
                             ├── 1. Cloudflare R2: releases/<ver>/* (paketler + *-sbom.spdx.json + manifest/imzalar, immutable)
                             ├── 2. GitHub Releases: v<ver> (tüm paketler + *-sbom.spdx.json + SHA256SUMS.txt(.minisig) + releases.json(.minisig))
                             ├── 3. Gitea Releases:  v<ver> (tüm paketler + *-sbom.spdx.json + SHA256SUMS.txt(.minisig) + releases.json(.minisig))
                             ├── 4. Cloudflare R2 kök: releases.json.minisig + releases.json (Cache-Control: public, max-age=300)
                             └── 5. İsteğe bağlı kancalar: release-pulse-api (/v1/admin/releases) ve statik site (/srv/site/)
```

### Tek Kaynak Kuralları
- Ürün adı (`RAGO_PRODUCT_NAME`), küçük harf kimliği (`RAGO_PRODUCT_ID`), paket kimliği (`RAGO_BUNDLE_ID`),
  alan adı (`RAGO_DOMAIN`), site adresi (`RAGO_PRODUCT_URL`) ve gömülü `minisign` açık anahtarı
  (`RAGO_MINISIGN_PUBKEY`) **yalnızca `cmake/RagoShellDefaults.cmake`** dosyasından okunur.
- İndirme alt alanı (`dl.<RAGO_DOMAIN>`) ve izin verilen indirme sunucuları (`dl.<RAGO_DOMAIN>`,
  `play.google.com`, `apps.apple.com`) bu tek kaynaktan türetilir.
- Yayın ve SBOM betikleri (`ci/generate-sbom.mjs`, `ci/generate-releases-json.mjs`, `ci/sign-release.sh`,
  `ci/publish-release.sh`) yerel geliştirici dosyası olan `ci/.env` dosyasını **asla okumaz, kaynaklamaz
  (`source`) ve yazdırmaz**.

---

## 2. Paket Matrisi ve `releases.json` Şeması

### 2.1 Beklenen Sürüm Çıktıları (`dist/`)

| Platform | Mimari | Dosya Adı | Format | Rol |
|---|---|---|---|---|
| `windows` | `x64` | `ragoshell-<ver>-windows-x64-setup.exe` | `exe` | **Birincil masaüstü kurulumcu** (`downloads.windows`) |
| `windows` | `x64` | `ragoshell-<ver>-windows-x64.msi` | `msi` | Kurumsal MSI kurulumcu (`artifacts[]`) |
| `windows` | `x64` | `ragoshell-<ver>-windows-x64.zip` | `zip` | Taşınabilir arşiv (`artifacts[]`) |
| `macos` | `arm64` | `ragoshell-<ver>-macos-arm64.dmg` | `dmg` | **Birincil masaüstü kalıbı** (`downloads.macos`) |
| `macos` | `arm64` | `ragoshell-<ver>-macos-arm64.tar.gz` | `tar.gz` | Taşınabilir uygulama arşivi (`artifacts[]`) |
| `linux` | `x64` | `ragoshell-<ver>-linux-x64.AppImage` | `AppImage` | **Birincil taşınabilir paket** (`downloads.linux`) |
| `linux` | `x64` | `ragoshell-<ver>-linux-x64.tar.gz` | `tar.gz` | Sistem Qt / arşiv paketi (`artifacts[]`) |
| `android` | `arm64` | `ragoshell-<ver>-android-arm64.apk` | `apk` | Doğrudan APK paketi (`artifacts[]`; mağaza URL'si yoksa `downloads.android`) |
| `android` | `arm64` | `ragoshell-<ver>-android-arm64.aab` | `aab` | Google Play App Bundle (`artifacts[]`, `primary: false`; SBOM, `SHA256SUMS.txt` ve R2/GitHub/Gitea arşivine dâhil edilir) |

Varsayılan olarak üç masaüstü platformunun (`windows`, `macos`, `linux`) birincil kurulumcuları zorunludur;
herhangi biri eksikse, 0 bayt ise veya `dist/` içinde farklı sürüme ait yabancı bir paket dosyası varsa üretim
hatayla durur.

### 2.2 Kanonik `releases.json` Yapısı

Şema üç tüketiciyle %100 uyumludur:
1. **`ragoshell-site` (`src/lib/releases.ts` `parseManifest` & `site.config.mjs` `allowedHosts`):**
   `latestVersion`, `publishedAt` (`YYYY-MM-DD`), `downloads.<platform>.url` (yalnız `https://` ve
   `allowedHosts` içindeki sunucular), `downloads.<platform>.label` (`<= 40` karakter). Bilinmeyen üst düzey
   veya iç alanlar (`schemaVersion`, `sha256`, `size`, `artifacts`) güvenli biçimde yoksayılır.
2. **`services/release-pulse-api/server.mjs` (`validateReleasesManifest` & `POST /v1/release-check`):**
   `schemaVersion`, `latestVersion`, `minSupportedVersion`, `mandatory`, `publishedAt`, `allDownloadsUrl`,
   `releaseNotes`, `downloads.<platform>` (`url`, `label`, `sha256`, `size`, isteğe bağlı platform bazlı
   `latestVersion`/`minSupportedVersion`/`mandatory`) ve `artifacts[]` alanlarını doğrular ve korur.
3. **Uygulama İstemcisi (`UpdateService` / `UpdatePulse` ve Faz 5.1 A3):**
   Toplam JSON boyutu `< 64 KiB` sınırının çok altındadır (~3–4 KiB); `UpdatePulse::parseCheckResponse` mevcut
   alanları okur, Faz 5.1 A3 ise `releases.json.minisig` imzasını ve `sha256`/`size` alanlarını doğrular.

```json
{
  "schemaVersion": 1,
  "latestVersion": "2.1.0",
  "minSupportedVersion": "1.8.0",
  "mandatory": false,
  "publishedAt": "2026-11-01",
  "allDownloadsUrl": "https://dl.ragoshell.com.tr/releases/",
  "releaseNotes": {
    "en": "Embedded JavaScript shell across desktop and mobile, adaptive touch density, OS keystore encryption, and command palette.",
    "tr": "Masaüstü ve mobilde gömülü JavaScript kabuğu, uyarlanabilir dokunmatik yoğunluk, işletim sistemi anahtarlığı şifrelemesi ve komut paleti."
  },
  "downloads": {
    "windows": {
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-windows-x64-setup.exe",
      "label": "Windows",
      "sha256": "<64-karakter-küçük-harf-hex>",
      "size": 68157440
    },
    "macos": {
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-macos-arm64.dmg",
      "label": "macOS",
      "sha256": "<64-karakter-küçük-harf-hex>",
      "size": 73400320
    },
    "linux": {
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-linux-x64.AppImage",
      "label": "Linux",
      "sha256": "<64-karakter-küçük-harf-hex>",
      "size": 79691776
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
      "name": "ragoshell-2.1.0-android-arm64.aab",
      "platform": "android",
      "arch": "arm64",
      "format": "aab",
      "primary": false,
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-android-arm64.aab",
      "sha256": "<64-karakter-küçük-harf-hex>",
      "size": 50331648
    },
    {
      "name": "ragoshell-2.1.0-android-arm64.apk",
      "platform": "android",
      "arch": "arm64",
      "format": "apk",
      "primary": true,
      "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-android-arm64.apk",
      "sha256": "<64-karakter-küçük-harf-hex>",
      "size": 52428800
    }
  ],
  "sbom": {
    "name": "ragoshell-2.1.0-sbom.spdx.json",
    "format": "spdx-json",
    "spdxVersion": "SPDX-2.3",
    "url": "https://dl.ragoshell.com.tr/releases/2.1.0/ragoshell-2.1.0-sbom.spdx.json",
    "sha256": "<64-karakter-küçük-harf-hex>",
    "size": 24576
  }
}
```

### 2.3 `SHA256SUMS.txt` Formatı
Standart GNU coreutils (`sha256sum -c SHA256SUMS.txt` ve `shasum -a 256 -c SHA256SUMS.txt`) ile uyumlu, dosya
adına göre ASCII sıralı, iki boşluk ayraçlı düz metin (ikili paketlerle birlikte `ragoshell-<ver>-sbom.spdx.json`
dosyasını da içerir):
```text
<64-hex-sha256>  ragoshell-2.1.0-android-arm64.aab
<64-hex-sha256>  ragoshell-2.1.0-android-arm64.apk
<64-hex-sha256>  ragoshell-2.1.0-linux-x64.AppImage
...
<64-hex-sha256>  ragoshell-2.1.0-sbom.spdx.json
```

### 2.4 A5 Üçüncü Taraf Lisans Paketi (`licenses/`) ve E6 Deterministik SPDX 2.3 SBOM

1. **CMake `licenses/` Kurulum Bileşeni (`cmake/RagoShellLicenses.cmake`):**
   - `cmake --install <build> [--component licenses] --prefix <stage>` çalıştığında hedef platform düzenine
     uygun olarak (macOS: `RagoShell.app/Contents/Resources/licenses`, Windows/Linux: `licenses/`):
     - Kök bildirim dosyalarını (`LICENSE`, `LICENSE.LGPLv3`, `COPYRIGHT`, `THIRD-PARTY-NOTICES.txt`),
     - Gömülü üçüncü taraf lisanslarını (`licenses/third-party/qscintilla-LICENSE.GPL3`,
       `qscintilla-OPENSOURCE-NOTICE.txt`, `qjson-LICENSE.txt`, `googletest-LICENSE.txt`,
       `DejaVuSans-LICENSE.txt`),
     - Mevcutsa `vcpkg_installed/<triplet>/share/<port>/copyright` dosyalarını çakışmasız
       `licenses/third-party/<port>-COPYRIGHT` adlarıyla (sembolik bağları ve 0 bayt dosyaları atlayarak)
       otomatik kurar.
   - `THIRD-PARTY-NOTICES.txt` dosyası GNU GPLv3 §6 ("Complete Corresponding Source Code" — kanonik Gitea ve
     GitHub kaynak depoları, en az 3 yıl erişilebilirlik taahhüdü) ve GNU LGPLv3 §4 ("Linking & Relinking
     Information" — paylaşımlı kütüphane değiştirme ve `-DCMAKE_PREFIX_PATH` ile yeniden bağlama adımları)
     bildirimlerini eksiksiz içerir ve `src/ragoshell-app/qml.qrc` üzerinden uygulama içi lisans ekranına da
     gömülür.
   - `ci/build-linux.sh`, `ci/build-macos.sh` ve `ci/build-windows.ps1` paketleme öncesinde
     `node ci/generate-sbom.mjs --verify-licenses <dir> --require-vcpkg-copyrights` ile tüm dosyaların mevcut,
     düzenli dosya (sembolik bağ değil) ve boş olmadığını fail-fast doğrular.

2. **Deterministik SPDX 2.3 JSON SBOM (`ci/generate-sbom.mjs`) ve OSV-Scanner Taraması:**
   - `cmake/RagoShellDefaults.cmake`, `CMakeLists.txt`, `vcpkg.json` (`builtin-baseline` + `overrides`),
     mevcutsa `vcpkg_installed/<triplet>/share/<port>/vcpkg.spdx.json` çıktıları, Qt 6 modülleri, gömülü
     `src/third-party/` bileşenleri ve `dist/` altındaki kurulum paketlerinin SHA-256 özetlerini birleştirerek
     bayt-bayt deterministik `dist/ragoshell-<ver>-sbom.spdx.json` üretir (`documentNamespace` içerik tabanlı
     RFC 4122 UUIDv5 ile türetilir).
   - Her bileşen geçerli SPDX lisans ifadesi (`GPL-3.0-only`, `LGPL-3.0-only OR GPL-3.0-only`, `Apache-2.0`,
     `BSD-3-Clause`, `Zlib`, `MIT`, `LGPL-2.1-only`, `Bitstream-Vera`, `LicenseRef-Scintilla`), `purl`
     (`pkg:vcpkg/...`, `pkg:github/...`, `pkg:generic/...`) ve `cpe23Type` tanımlayıcıları taşır.
   - `.github/workflows/ci.yml` `i18n` kapısında SHA-256 özeti sabitlenmiş Google `osv-scanner` (`v2.7.0`) ile
     hem `vcpkg.json` hem üretilen SPDX 2.3 SBOM açık kaynak güvenlik açıklarına karşı otomatik taranır.

---

## 3. Ed25519 / `minisign` Anahtar Yaşam Döngüsü ve Trusted Comment Bağlaması

### 3.1 Neden `minisign` ve Trusted Comment?
`minisign` imzası (`.minisig`) dört satırdan oluşur:
1. `untrusted comment: ...` — **İmzalanmaz**; doğrulayıcılar tarafından tamamen yoksayılır.
2. `<Base64 74 bayt>` — Algoritma (`"ED"` BLAKE2b-512 ön-özetli Ed25519 veya `"Ed"` saf Ed25519) + 8 bayt
   `keynum` + 64 bayt Ed25519 dosya imzası (`sig`).
3. `trusted comment: ...` — **İmzalanır**; dosya imzasının hemen ardından gelen bu metin, dördüncü satırdaki
   64 baytlık `global_sig` (`Ed25519(sk, sig || trusted_comment)`) ile kriptografik olarak mühürlenir.
4. `<Base64 64 bayt>` — `global_sig`.

`ci/sign-release.sh` her imzada `trusted comment` (`-t`) içine şu üç alanı deterministik sekme (`\t`) ayracıyla
bağlar:

```text
release:<version>	timestamp:<YYYY-MM-DDTHH:MM:SSZ>	file:<basename>
```

Örnek:
- `releases.json.minisig` → `trusted comment: release:2.1.0	timestamp:2026-11-01T20:30:00Z	file:releases.json`
- `SHA256SUMS.txt.minisig` → `trusted comment: release:2.1.0	timestamp:2026-11-01T20:30:00Z	file:SHA256SUMS.txt`

Bu bağlama sayesinde:
- `SHA256SUMS.txt.minisig` dosyasının `releases.json.minisig` yerine konulması (`file:` uyuşmazlığı) reddedilir.
- Eski bir sürümün geçerli imzasının yeni bir sürüm adına yeniden oynatılması (`release:` uyuşmazlığı ve
  `latestVersion` kontrolü) reddedilir.
- İmzalanma zamanı (`timestamp:`) kriptografik olarak kanıtlanır; istemci dondurma (freeze) saldırılarını tespit
  edebilir.

### 3.2 Anahtar Üretimi, Saklama ve Döndürme (Key Lifecycle)

1. **Üretim Anahtar Çifti Oluşturma (İnsan Adımı — Çevrimdışı / Güvenli Makinede):**
   - Parola korumalı ana üretim anahtarı (yerel imzalama ve soğuk yedek için):
     ```bash
     minisign -G -p ragoshell-release.pub -s ragoshell-release.key \
       -c "RagoShell Release Signing Key (2026)"
     ```
   - Otomatik GitHub Actions CI imzalaması için seçenekler:
     - **Seçenek A (Önerilen — Parola Korumalı Anahtar):** Yukarıdaki scrypt korumalı `.key` içeriği
       `RAGO_MINISIGN_SECRET_KEY`, parolası `RAGO_MINISIGN_PASSWORD` olarak yalnızca korumalı `release`
       ortamına (GitHub Actions Protected Environment / Repository Secrets) girilir. `ci/sign-release.sh`,
       `minisign`'ın `/dev/tty` parola istemini Python `pty` üzerinden besler; parola hiçbir zaman süreç komut
       satırında (`ps aux`) görünmez.
     - **Seçenek B (Parolasız CI Alt Anahtarı):** Yalnızca CI ortamına özel ayrı bir anahtar `-W` ile üretilir
       (`minisign -G -W -p ragoshell-ci.pub -s ragoshell-ci.key`); sızıntı şüphesinde ana soğuk anahtara
       dokunmadan döndürülür.
2. **Açık Anahtarın Depoya Yazılması:**
   - `ragoshell-release.pub` dosyasının ikinci satırındaki `RW...` Base64 açık anahtar dizgesi
     `cmake/RagoShellDefaults.cmake` içindeki `RAGO_MINISIGN_PUBKEY` değişkenine yazılır.
   - Gerçek üretim `.key` dosyası **asla** git deposuna, imajlara veya `ci/.env` dosyasına konmaz; yalnız
     çevrimdışı şifreli parola yöneticisinde ve CI Secret kasasında tutulur.
3. **Anahtar Döndürme / İptal (Rotation & Revocation):**
   - Planlı döndürmede yeni açık anahtar bir sürüm önceden (`N`) ikincil/sonraki anahtar olarak uygulamaya
     eklenir, `N+1` sürümüyle birincil yapılır.
   - Acil anahtar sızıntısında eski CI sırrı derhal silinir, R2 ve Releases üzerindeki şüpheli manifest kaldırılır,
     yeni anahtarla imzalanmış yama sürümü yayınlanır ve `minSupportedVersion` yükseltilir.

---

## 4. Tehdit Modeli ve Güvenlik Önlemleri

| Kod | Tehdit / Saldırı Yüzeyi | Etki | Uygulanan Kriptografik ve Mimari Önlem |
|---|---|---|---|
| **T1** | R2 kovası, CDN veya ayna üzerinde ikili kurulumcu değiştirme (Binary Tampering) | Uzaktan kod çalıştırma (RCE) | `releases.json` ve `SHA256SUMS.txt` Ed25519 (`minisign`) ile imzalanır; her paketin tam bayt boyutu (`size`) ve `sha256` özeti manifest içinde mühürlenir. İşletim sistemi düzeyinde ayrıca Windows Authenticode (Faz 3.3) ve macOS Developer ID + Notarization (Faz 3.4) imzaları bulunur. |
| **T2** | Sürüm geri alma (Downgrade) ve eski manifesti tekrar oynatma (Replay / Freeze) | Bilinen güvenlik açığı bulunan eski sürüme indirme | `trusted comment` içinde `release:<version>` ve UTC `timestamp:` imzalanır; `releases.json.minisig` içindeki `release:` değeri JSON içindeki `latestVersion` ile birebir eşleşmek zorundadır. İstemci (`UpdateService`) `latestVersion <= currentAppVersion` ise güncelleme önermez ve `cachedMinSupportedVersion` kilidini çevrimdışı korur. |
| **T3** | Dosya/imza çapraz değiştirme (`SHA256SUMS.txt.minisig` imzasını `releases.json.minisig` olarak sunma) | Yanlış dosya doğrulaması | `trusted comment` içine `file:<basename>` (`file:releases.json` vs `file:SHA256SUMS.txt`) gömülür; doğrulayıcı `global_sig` kontrolünden sonra `file:` değerini hedef dosya adıyla karşılaştırır. |
| **T4** | Atomik olmayan yükleme yarışı (`releases.json` güncellendiğinde yeni paketlerin henüz R2'de olmaması) | İndirme sırasında 404 veya imza uyuşmazlığı | `ci/publish-release.sh` önce `releases/<version>/*` altına tüm paketleri ve sürüm manifestini yükler; **ancak tümü başarılı olduktan sonra** en son R2 kökündeki `releases.json.minisig` ve `releases.json` dosyalarını günceller. |
| **T5** | Zararlı URL, SSRF, açık yönlendirme, kimlik bilgisi sızıntısı veya XSS (`releases.json` / `CHANGELOG`) | İstemciyi zararlı sunucuya yönlendirme veya sitede XSS | Tüm URL'ler `https://` zorunludur, `username`/`password` içeremez ve `hostname` yalnızca `['dl.<domain>', 'play.google.com', 'apps.apple.com']` izin listesinde olabilir. `ragoshell-site` yalnızca `textContent` kullanır. Dosya adlarında `..`, `/`, `\`, boşluk ve kontrol karakterleri reddedilir; sembolik bağlar (`symlink`) `lstat` ile reddedilir. |
| **T6** | CI loglarında veya süreç tablosunda (`ps`) gizli anahtar / token sızıntısı | İmza anahtarı veya R2/Gitea yazma yetkisi kaybı | Tüm kabuk betiklerinde `set +x` ve `umask 077` uygulanır; geçici `.key` dosyaları `0700` dizinde `0600` izinle açılır ve `trap ... EXIT` içinde sıfırlanarak (`dd if=/dev/zero`) silinir. Parolalar ve admin token'lar CLI argümanı olarak değil, PTY veya `curl --config -` (stdin) üzerinden aktarılır. |
| **T7** | `dist/` dizininde önceki derlemeden kalan eski sürüm paketlerinin yeni sürüme karışması | Yanlış paketin imzalanıp yayınlanması | `ci/generate-releases-json.mjs` ve `ci/generate-sbom.mjs`, `dist/` içindeki her paket dosyasının adındaki `<version>` önekini hedef sürümle karşılaştırır; farklı sürümlü veya tanınmayan tek bir paket dosyası bile varsa işlemi hatayla durdurur. |
| **T8** | Üçüncü taraf bağımlılık CVE'leri, eksik GPLv3/LGPLv3 lisans bildirimi veya SBOM/paket özeti uyumsuzluğu | Tedarik zinciri zafiyeti veya lisans ihlali | CMake `licenses` bileşeni ve `--verify-licenses` paketleme öncesi boş/eksik/symlink lisans dosyalarını reddeder; `ci/generate-sbom.mjs --verify-only` ve `verifyReleaseDirectory` SBOM içindeki `SPDXRef-Artifact-*` SHA-256 özetlerini diskteki ikililerle ve imzalı `SHA256SUMS.txt` / `releases.json` ile çapraz doğrular; CI üzerinde SHA-256 sabitli Google `osv-scanner` (`v2.7.0`) her push/PR'da zafiyet taraması yapar. |

---

## 5. Çok Hedefli Yükleme Sırası ve `Cache-Control` Politikası

| Sıra | Hedef | Nesne Yolu / Uç Nokta | `Cache-Control` Başlığı | Neden? |
|---|---|---|---|---|
| **1** | Yerel Doğrulama Kapısı | `dist/releases.json(.minisig)`, `dist/SHA256SUMS.txt(.minisig)`, `dist/ragoshell-<ver>-*` | — | `minisign -Vm` + Node Ed25519 + trusted comment + SHA-256 + boyut kontrolü geçmeden ağa çıkılmaz. |
| **2** | Cloudflare R2 (Sürüm Arşivi) | `s3://<R2_BUCKET>/releases/<ver>/<dosya>` | `public, max-age=31536000, immutable` | Sürümlü URL'ler değişmezdir; CDN kenarında 1 yıl önbelleklenir, VPS/R2 işlem maliyetini sıfıra indirir. |
| **3** | GitHub Releases | `refs/tags/v<ver>` (`gh release create/upload --clobber`) | GitHub CDN | Açık kaynak topluluk doğrulaması ve yedek indirme kanalı. |
| **4** | Gitea Releases (isteğe bağlı) | `POST /api/v1/repos/<owner>/ragoshell/releases` (`v<ver>`) | Gitea | İsteğe bağlı kendi barındırılan Gitea sürüm arşivi. |
| **5** | Cloudflare R2 (Kök İşaretçi) | `s3://<R2_BUCKET>/releases.json.minisig` ardından `s3://<R2_BUCKET>/releases.json` | `public, max-age=300` | En son güncellenir (atomik geçiş). 5 dakikalık kısa TTL sayesinde yeni sürüm ve zorunlu güvenlik güncellemeleri hızla yayılır. |
| **6** | İsteğe Bağlı Kancalar | `POST /v1/admin/releases` (`release-pulse-api`) ve `deploy-receive.sh releases` (`ragoshell-infra` `/srv/site/releases.json(.minisig)`) | `max-age=300` (nginx) | `release-pulse-api` yeniden başlatılmadan canlı manifesti belleğe alır; statik site `/releases.json` aynı köken (`'self'`) üzerinden güncel kalır. |

---

## 6. Faz 5.1 (Madde A3) İstemci Doğrulamasıyla Sözleşme

Faz 5.1 A3 (`UpdateService` minisign doğrulaması) bu hattın ürettiği çıktıları şu kesin sözleşmeyle tüketecektir:

1. **Gömülü Açık Anahtar:** `cmake/RagoShellDefaults.cmake` içindeki `RAGO_MINISIGN_PUBKEY` (`RW...` Base64, 42
   bayt: `"Ed"` + 8 bayt `keynum` + 32 bayt Ed25519 açık anahtarı) şu an tek yapılandırma kaynağı olarak
   tanımlanmıştır; Faz 5.1 (A3) adımında `BrandInfo.h.in` üzerinden `rago-core` içine derleme zamanında gömülecektir.
2. **İmza Konumu:**
   - Statik R2 / CDN kontrolünde: `https://dl.<domain>/releases.json` ve `https://dl.<domain>/releases.json.minisig`.
   - `release-pulse-api` (`POST /v1/release-check`) üzerinden: yanıtla birlikte sunulan veya `dl.<domain>`
     üzerinden çekilen `releases.json.minisig`.
3. **İstemci Doğrulama Adımları (Fail-Closed):**
   - `releases.json` gövdesi `<= 65536` bayt olmalı; `.minisig` dosyasının 2. satırındaki `keynum` gömülü açık
     anahtarın `keynum` değeriyle eşleşmeli.
   - Yalnızca ön-özetli `"ED"` (`0x45 0x44`, `BLAKE2b-512(releases.json)`) imzaları kabul edilir (eski ön-özetsiz
     `"Ed"` imza biçimi algoritma karışıklığını önlemek için reddedilir); Ed25519 imzası (`sig`) OpenSSL
     `EVP_DigestVerify` ile doğrulanır.
   - `global_sig`, `sig || trusted_comment` baytları üzerinde aynı Ed25519 açık anahtarıyla doğrulanır.
   - Doğrulanmış `trusted comment` içinde `file:releases.json` ve `release:<latestVersion>` (`releases.json`
     içindeki `latestVersion` ile birebir aynı) bulunması zorunludur; `timestamp:` geçerli bir UTC ISO-8601 zaman
     damgası olmalıdır.
   - Herhangi bir doğrulama başarısız olursa yanıt sessizce reddedilir (`StatusKind::Failed`), önbelleğe
     yazılmaz ve kullanıcıya doğrulanmamış hiçbir indirme bağlantısı gösterilmez.
   - Doğrudan ikili indirme yapıldığında indirilen dosyanın boyutu `downloads.<platform>.size` ve özeti
     `downloads.<platform>.sha256` ile eşleşmeden çalıştırılmaz.

---

## 7. Üretim Devreye Alma Kontrol Listesi (İnsan Adımları)

1. **Minisign Anahtar Çifti ve Korumalı `release` Ortamı:** Güvenli makinede `minisign -G` ile üretim anahtar
   çiftini oluşturun; `RW...` açık anahtar satırını `cmake/RagoShellDefaults.cmake` içindeki `RAGO_MINISIGN_PUBKEY`
   alanına yazın; özel anahtarı parola yöneticisine yedekleyip GitHub Actions'ta **korumalı `release` Environment**
   (`Required reviewers` + yalnız `v*` etiketleri ve `main`/`master` dalı kısıtıyla) altındaki
   `RAGO_MINISIGN_SECRET_KEY` (+ parolalıysa `RAGO_MINISIGN_PASSWORD`) sırrına ekleyin.
2. **Cloudflare R2 ve `dl.` Alan Adı:** Cloudflare'de `ragoshell-releases` R2 kovasını açın, `dl.ragoshell.com.tr`
   özel alan adını (Custom Domain, proxied HTTPS) bağlayın ve **yalnızca bu kovaya Object Read & Write** yetkili
   bir R2 S3 API token'ı üretip `R2_ACCOUNT_ID`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY` olarak
   tanımlayın.
3. **Dar Kapsamlı Yayın Token'ları:** İsteğe bağlı Gitea yayın hedefi kullanılıyorsa yalnızca ilgili depoya paket/sürüm yazma yetkili dar kapsamlı `GITEA_TOKEN` oluşturun.

---

## 8. Doğrulama Planı (Verification Plan)

### Otomatik Test Kapıları
- `node ci/test-sbom-and-licenses.mjs`: CMake `licenses/` kurulum kuralları (macOS `.app` ve düz dizin düzeni,
  0 bayt ve sembolik bağ reddi), `THIRD-PARTY-NOTICES.txt` GPLv3 §6 / LGPLv3 §4 ve marka temizliği, SPDX 2.3
  JSON şema/deterministiklik/`purl`/`cpe23Type`/`vcpkg.spdx.json` birleştirme doğrulaması, negatif ve kurcalama
  (tamper) testleri ve uçtan uca yayın hattı (`releases.json`, `SHA256SUMS.txt`, R2/GitHub/Gitea `--dry-run`,
  `ragoshell-site` ve `release-pulse-api`) entegrasyonu.
- `node ci/test-release-pipeline.mjs`: Sahte `dist/` ağacı üzerinde manifest ve `SHA256SUMS.txt` üretimi,
  `ragoshell-site` (`parseManifest` + `allowedHosts`) ve `release-pulse-api` (`createReleasePulseServer`) şema
  uyumu, negatif güvenlik testleri (hatalı sürüm, `http://` URL, allowlist dışı sunucu, gömülü kimlik bilgisi,
  0 bayt dosya, sembolik bağ, eksik zorunlu platform, bozuk SHA-256, değiştirilmiş `releases.json`, çapraz dosya
  imzası ve yanlış `trusted comment` reddi), `--dry-run` yayın simülasyonu ve sistemde `minisign` kuruluysa uçtan
  uca gerçek `minisign` imzalama/doğrulama testi.
- `node services/release-pulse-api/test-server.mjs`: Sunucu birim ve entegrasyon testleri (yalnızca `127.0.0.1`
  üzerinde dinler).
- `bash -n ci/*.sh`, `node ci/check-i18n.mjs`, `ragoshell-site` (`npm run check && npm run build && npm run check:dist`)
  ve `ragoshell-infra` (`docker compose config -q` + container içinde `nginx -t`).

### Automated Security Check
- **Security Scanner**: Yeni oluşturulan tüm kod dosyaları üzerinde güvenlik taraması çalıştırılır (aktif tarayıcı
  mevcutsa `run_security_scanner`; ek olarak statik girdi doğrulama, kabuk enjeksiyonu, yol geçişi ve sır sızıntısı
  kontrolleri). Bulgular varsa düzeltilir ve belgelenir.
- **Security Audit**: Üretilen tüm yeni betikler ve yapılan tüm değişiklikler (`git diff`) girdi doğrulaması,
  komut çalıştırma güvenliği (`spawnFile`/`execFileSync` argüman dizileri, kabuk genişletmesiz çağrı), dosya yolu
  sınırları (`path.resolve` + `path.sep` önek kontrolü + `lstat` sembolik bağ reddi), sırların süreç/log/disk
  güvenliği ve kriptografik doğrulama tamlığı açısından denetlenir.

## Pre-releases never move the root manifest (2026-11-04)

`ci/publish-release.sh` publishes a version with a SemVer pre-release part (`2.1.0-rc.1`), or any run with
`is_prerelease=true`, only under `releases/<ver>/` (manifest, signature, artifacts) and as a GitHub/Gitea
pre-release. The root `releases.json` / `releases.json.minisig` and the site and pulse hooks are skipped,
so stable installations are never offered a release candidate (`UpdatePulse` has no channel filter).
`--stable` cannot override a pre-release version. Version precedence everywhere (`generate-releases-json.mjs`,
`UpdatePulse::compareVersions`) follows SemVer 2.0.0 §11: `rc.10 > rc.2`, numeric < alphanumeric
identifiers, leading-zero numeric identifiers are rejected.
