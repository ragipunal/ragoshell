# Android Kod İmzası, Play App Signing, AAB/APK Paketleme ve 16 KB Hizalama Kılavuzu (Faz 3.8 — M11)

Yazılış: 2026-11-02. Bu belge, yol haritası Faz 3.8 (`M11 Android imzalı APK/AAB`, `docs/Roadmap.md` §7 3.8,
`docs/TodoMac.md` M11 ve `docs/Platforms.md`) kapsamındaki **iç operasyon, güvenlik ve CI yapılandırma
kılavuzudur** (`ragoshell-site` dış site `docs.publish` izin listesinde yer almaz).

---

## 1. Bağlam: APK vs AAB, Play App Signing ve 16 KB Bellek Sayfası Zorunluluğu

Android dağıtımı iki tamamlayıcı paket biçimi üzerinden yürütülür; ürün kimliği (`tr.com.ragoshell.app`) ve
dosya adlandırması yalnızca `cmake/RagoShellDefaults.cmake` tek kaynağından okunur:

| Paket | Dosya Adı (`dist/`) | İmza Aracı ve Şeması | Kullanım Amacı |
|---|---|---|---|
| **APK** | `ragoshell-<sürüm>-android-arm64.apk` | `zipalign` (`-P 16 -f -v 4`) + `apksigner` (APK Signature Scheme **v2 + v3**) | `dl.ragoshell.com.tr`, GitHub Releases ve Gitea Releases üzerinden doğrudan indirme ve kurulum (sideload); `releases.json` içinde `downloads.android` birincil paketi (mağaza adresi tanımlanana kadar). |
| **AAB** | `ragoshell-<sürüm>-android-arm64.aab` | `jarsigner` (`SHA-256` özet, Upload Keystore) | **Google Play Console** üretim ve test kanallarına (Internal / Closed / Production) yüklenen Android App Bundle paketi (`releases.json` `artifacts[]`, `SHA256SUMS.txt` ve SPDX 2.3 SBOM içine dâhil edilir). |

### Play App Signing Mimarisi ve Sertifika Notu

1. **Upload Key (Yükleme Anahtarı) vs App Signing Key (Uygulama İmzalama Anahtarı):**
   - Google Play Console'da yeni uygulamalar için **Play App Signing** zorunludur.
   - CI veya yerel sürüm betiği `.aab` paketini geliştiricinin ürettiği **Upload Keystore** ile imzalar; Google Play bu imzayı doğruladıktan sonra son kullanıcı cihazlarına teslim edilen optimize APK'ları kendi donanım kasasındaki **App Signing Key** ile yeniden imzalar.
   - Upload anahtarı kaybolursa veya sızarsa uygulama paket kimliği (`tr.com.ragoshell.app`) değişmeden Play Console üzerinden **Upload Key Reset** talebiyle yenilenebilir.
2. **Doğrudan APK (Sideload) ve Play Store Çapraz Güncelleme Davranışı:**
   - Upload Keystore ile imzalanan doğrudan `.apk` paketi ile Google Play'in kendi App Signing Key'i ile dağıttığı mağaza paketi farklı sertifika parmak izi taşır. Bir cihazda doğrudan `.apk` kuruluysa Play Store sürümüne geçerken (veya tersi durumda) Android paket yöneticisi imza uyuşmazlığı nedeniyle mevcut kurulumun kaldırılmasını ister; aynı kanal içindeki güncellemeler ise kesintisiz çalışır.

### Android 15+ ve Google Play 16 KB Sayfa Boyutu Zorunluluğu (`targetSdk = 37`)

Google Play politikası (Android 15 / API 35 ve üzeri hedefli tüm uygulamalarda zorunlu) uyarınca 64-bit ARM (`arm64-v8a`) yerel kod içeren paketler iki katmanda **16 KB (16384 bayt / `0x4000`)** bellek sayfası sınırına uymak zorundadır:

1. **ELF `LOAD` Segment Hizalaması:** Uygulamanın kendi `libragoshell_arm64-v8a.so` kütüphanesi, CMake/vcpkg bağımlılıkları ve Qt Android paylaşımlı kütüphanelerinin tamamında her `PT_LOAD` (`LOAD`) segmenti `p_align >= 0x4000` olmalıdır (`-Wl,-z,max-page-size=16384`).
2. **APK ZIP Sayfa Hizalaması:** Sıkıştırılmamış `.so` girdileri içeren APK arşivlerinde `zipalign -P 16 -f -v 4` (Android SDK Build-Tools 35+; daha eski Build-Tools sürümlerinde `-p -f -v 4` geri dönüşü) imzalamadan **önce** uygulanmalıdır (`apksigner` sonrasında `zipalign` çalıştırmak v2/v3 imza bloğunu bozar).

---

## 2. Upload Keystore Üretimi (`keytool`) ve Play App Signing Kurulumu (İnsan Adımı)

> **Kural:** Hiçbir `.jks`, `.keystore` veya `.p12` dosyası git çalışma ağacına ya da sürüm kontrolüne yazılmaz (`.gitignore` içinde `*.keystore`, `*.jks`, `*.p12`, `*.pfx` engellidir). Üretilen keystore dosyası ve parolası şirket parola yöneticisinde yedeklenmelidir.

### Adım A — PKCS#12 Upload Keystore Üretimi

Keystore dosyasını **depo dışında** güvenli bir dizinde (`chmod 700`), parolayı komut satırı geçmişine (`history` / `ps`) düşürmeden etkileşimli istemle üretin:

```bash
mkdir -p -m 700 "$HOME/.secure-android-keys"

keytool -genkeypair -v \
    -keystore "$HOME/.secure-android-keys/ragoshell-upload.jks" \
    -storetype PKCS12 \
    -keyalg RSA \
    -keysize 4096 \
    -validity 10000 \
    -alias upload \
    -dname "CN=RagoShell Upload, O=RagoShell, C=TR"

chmod 600 "$HOME/.secure-android-keys/ragoshell-upload.jks"
```

Üretilen anahtarın `PrivateKeyEntry` tipinde olduğunu ve SHA-256 parmak izini doğrulayın:

```bash
keytool -list -v \
    -keystore "$HOME/.secure-android-keys/ragoshell-upload.jks" \
    -alias upload
```

### Adım B — Google Play Console Kaydı (Faz 7.2 İnsan Adımı ile Bağlantı)

1. Google Play Console'da `tr.com.ragoshell.app` uygulamasını oluştururken **Test and release → Setup → App signing** bölümünde **Play App Signing** varsayılanını etkin tutun.
2. İlk imzalı `ragoshell-<sürüm>-android-arm64.aab` paketini **Internal testing** kanalına yüklediğinizde Google Play bu `.aab` içindeki sertifikayı (`upload` alias'ı) uygulamanın kalıcı **Upload key certificate** kaydı olarak kilitler.
3. Play Console'daki **Upload key certificate (SHA-256)** parmak izinin yerel `keytool -list -v` çıktısıyla birebir eşleştiğini doğrulayıp parola kasasına not edin.

---

## 3. `ci/build-android.sh` ve `ci/build-android.ps1` Mimarisi

Hem macOS/Linux (`ci/build-android.sh`) hem Windows (`ci/build-android.ps1`) betikleri aynı güvenlik ve doğrulama sözleşmesini uygular:

### Adım 1 — Erken Yapılandırma ve Araç Denetimi (Fail-Fast)
- Hiçbir `RAGO_ANDROID_KEYSTORE*` / `RAGO_ANDROID_KEY_*` (veya `ANDROID_KEYSTORE_*` / `ANDROID_KEY_*` eş anlamlısı) tanımlı değilse açık bir `warn:` satırı basarak imzasız/debug-imzalı Release `.apk` ve `.aab` üretir; böylece PR iş akışları ve yerel geliştirici derlemeleri sır gerektirmeden yeşil kalır.
- Herhangi bir imzalama değişkeni tanımlıysa CMake derlemesi başlamadan **önce**:
  - Tanınmayan / yanlış yazılmış `RAGO_ANDROID_*` değişkenlerini (sessizce imzasız üretime düşmemek için),
  - Eksik veya çelişkili parametreleri (ör. hem `RAGO_ANDROID_KEYSTORE` hem `RAGO_ANDROID_KEYSTORE_BASE64` verilmesi, parola veya `RAGO_ANDROID_KEY_ALIAS` unutulması, alias eş anlamlılarıyla çakışma),
  - Gerekli imzalama araçlarını (`keytool`, `jarsigner`, Android SDK `build-tools` altındaki `zipalign` ve `apksigner`),
  - Keystore bütünlüğünü, `alias` varlığını (`PrivateKeyEntry`) ve anahtar parolasının doğruluğunu (küçük bir geçici arşiv üzerinde `jarsigner` sınamasıyla) doğrular.
- `--validate-signing-only` (`build-android.sh`) ve `-ValidateSigningOnly` (`build-android.ps1`) bayrakları CMake/Qt/NDK derlemesi başlatmadan yalnızca bu doğrulama kapısını çalıştırır.

### Adım 2 — İzole Geçici Keystore ve Sır Hijyeni
- İmzalama sırları (`*_BASE64`, `*_PASS`, `*_PASSWORD`) betik başında yerel değişkenlere kopyalanır ve dışa aktarılan ortamdan (`unset` / `$env:... = $null`) derhal silinir; CMake, Ninja, `vcpkg`, Gradle ve Node alt süreçleri hiçbir sırrı miras almaz.
- `RAGO_ANDROID_KEYSTORE_BASE64` verildiğinde yalnız sahibi tarafından erişilebilen `$ci_tmp/sign` (`chmod 700` / Windows'ta devralmasız kullanıcı ACL'si) altında `0600` izinli `upload.keystore` dosyasına çözülür.
- Parolalar süreç komut satırında (`ps` / `Get-CimInstance Win32_Process`) **asla** geçirilmez:
  - `keytool` ve `jarsigner`: `-storepass:env <VAR>` ve `-keypass:env <VAR>` ile yalnızca o alt sürecin anlık ortamından okunur.
  - `apksigner`: `--ks-pass env:<VAR>` ve `--key-pass env:<VAR>` ile yalnızca o alt sürecin anlık ortamından okunur.
- `trap cleanup EXIT` (`HUP`, `INT`, `TERM` dâhil) ve PowerShell `finally` bloğunda geçici keystore dosyası önce boyutu kadar `0x00` baytıyla üzerine yazılıp (`zero-wipe`) ardından silinir.

### Adım 3 — Release Derleme, APK (`zipalign` + `apksigner`) ve AAB (`jarsigner`) İmzalama
- CMake Release önayarları kullanılır:
  - macOS konak: `android-arm64-macos-release` (`CMAKE_BUILD_TYPE=Release`)
  - Linux konak: `android-arm64-linux` (`CMAKE_BUILD_TYPE=Release`)
  - Windows konak: `android-arm64-release` (`CMAKE_BUILD_TYPE=Release`)
- Hem `CMakeLists.txt` / `src/ragoshell-app/CMakeLists.txt` (`-Wl,-z,max-page-size=16384`) hem `vcpkg-overlays/triplets/{arm64,x64}-android.cmake` (`VCPKG_LINKER_FLAGS`) üzerinde 16 KB ELF bağlayıcı hizalaması zorlanır.
- Hem `${product_id}_make_apk` (`apk`) hem `${product_id}_make_aab` (`aab`) hedefleri derlenir; gömülü açık kaynak lisans bildirimi (`THIRD-PARTY-NOTICES.txt`) ve `ci/generate-sbom.mjs --verify-source-licenses` kapısı doğrulanır.
- Keystore etkin olduğunda:
  1. **APK:** Ham Release APK üzerinde `zipalign -P 16 -f -v 4` (Build-Tools < 35 ise `-p -f -v 4`) çalıştırılır, `zipalign -c` ile hizalama teyit edilir, ardından `apksigner sign --v2-signing-enabled true --v3-signing-enabled true` ile `dist/ragoshell-<sürüm>-android-arm64.apk` imzalanır ve `apksigner verify` (hem manifest SDK aralığı hem v2 bloğunun atlanmaması için `--min-sdk-version 24 --verbose --print-certs`) çıktısında hem **v2** hem **v3** şemasının `true` döndüğü doğrulanır.
  2. **AAB:** Ham Release AAB `jarsigner -digestalg SHA-256` ile imzalanır ve `jarsigner -verify -verbose -certs` çalıştırılarak hem çıkış kodunun `0` olduğu hem de çıktının `"jar verified."` içerdiği (`"jar is unsigned"` ve `"contains unsigned entries"` içermediği; Android upload sertifikaları kendi kendine imzalı olduğundan `-strict` kod `4` ürettiği için açık metin denetimi) zorunlu olarak doğrulanır.

### Adım 4 — Zorunlu 16 KB ELF `LOAD` Hizalama Kapısı (`RAGO_ANDROID_STRICT_ALIGN=1`)
- `RAGO_ANDROID_STRICT_ALIGN` varsayılan olarak **`1`** değerindedir.
- `llvm-readelf` veya `readelf` (`PATH` veya `$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/*/bin/llvm-readelf*`) bulunur; `RAGO_ANDROID_STRICT_ALIGN=1` iken araç bulunamazsa betik hata vererek durur.
- Denetim üç alanı da kapsar:
  1. Derleme ağacındaki (`build/<preset>`) tüm `*.so` dosyaları,
  2. Üretilen `dist/ragoshell-<sürüm>-android-arm64.apk` arşivinden güvenli geçici dizine (yol geçişi / `..` ve sembolik bağ denetimli) çıkarılan tüm `lib/arm64-v8a/*.so` dosyaları (en az 1 adet `.so` bulunması zorunludur),
  3. Üretilen `dist/ragoshell-<sürüm>-android-arm64.aab` arşivinden çıkarılan tüm `base/lib/arm64-v8a/*.so` dosyaları (en az 1 adet `.so` bulunması zorunludur).
- Her `.so` dosyasının tüm ELF `LOAD` segmentlerinde `p_align >= 0x4000` (16384 bayt) şartı kontrol edilir; tek bir hizasız segment bile varsa betik hatalı dosyayı raporlayıp sıfır dışı kodla (`exit 1`) sonlanır.
- `--check-align-only <yol...>` / `-CheckAlignOnly` kipiyle bu kapı CI öz-testlerinde (`ci/test-release-pipeline.mjs`) sentetik hizalı/hizasız ELF, APK ve AAB dosyaları üzerinde doğrudan sınanır.

---

## 4. Tehdit Modeli ve Sır Hijyeni Özeti

| Tehdit / Hata Senaryosu | Uygulanan Kontrol |
|---|---|
| Keystore veya anahtar parolasının süreç tablosuna (`ps`) veya CI loglarına sızması | `set +x` zorlanır; parolalar argüman olarak değil yalnız `env:` (`-storepass:env`, `--ks-pass env:`) üzerinden anlık alt sürece verilir; derleme alt süreçlerinden `unset` ile temizlenir; hata mesajları parola veya Base64 verisi yazdırmaz. |
| Çözülmüş geçici keystore dosyasının paylaşımlı koşucuda kalması | `$ci_tmp/sign` dizini `0700` (Windows'ta yalnızca mevcut kullanıcı ACL'si), dosya `0600` izinle açılır; `EXIT` / `finally` bloğunda dosya boyutu kadar sıfır bayt (`0x00`) yazılıp silinir. |
| Yanlış yazılmış veya eksik CI sırrı yüzünden etiket sürümünün sessizce imzasız çıkması | Tek bir `RAGO_ANDROID_*` imzalama değişkeni veya tanınmayan `RAGO_ANDROID_*` adı bile algılandığında eksik/çelişkili parametre derleme başlamadan `exit 1` ile durdurulur. |
| `jarsigner -verify` komutunun imzasız `.aab` üzerinde `0` çıkış kodu döndürmesi | Sıfır çıkış koduna ek olarak standart çıktıda `"jar verified."` satırı zorunlu tutulur; `"jar is unsigned"` ve `"contains unsigned entries"` metinleri reddedilir. |
| Arşiv çıkarma sırasında Zip Slip (`../`) veya sembolik bağ saldırısı | APK/AAB içinden `.so` çıkarılırken girdi adlarında mutlak yol, `\` ve `..` bileşenleri denetlenir; sembolik bağlar elenir. |
| Üçüncü parti veya önbellekten gelen tek bir `.so` dosyasının 4 KB (`0x1000`) hizalı kalması | Yalnız derleme dizini değil, bizzat paketlenmiş son `.apk` ve `.aab` arşivlerinin içindeki tüm `*.so` dosyaları `RAGO_ANDROID_STRICT_ALIGN=1` ile taranır. |

---

## 5. Ortam Değişkenleri Referansı

`docs/GitHubMirror.md` §4.B'deki `ANDROID_*` adları ile `RAGO_ANDROID_*` adlarının her ikisi de desteklenir (ikisi birden farklı değerlerle verilirse çakışma hatası üretilir).

| Ortam Değişkeni | Eş Anlamlısı (Alias) | Zorunluluk | Açıklama |
|---|---|---|---|
| `RAGO_ANDROID_KEYSTORE` | — | `KEYSTORE_BASE64` yoksa zorunlu | Diskteki `.jks` / `.keystore` / `.p12` upload keystore dosyasının tam yolu (yerel kullanım). `KEYSTORE_BASE64` ile aynı anda verilemez. |
| `RAGO_ANDROID_KEYSTORE_BASE64` | `ANDROID_KEYSTORE_BASE64` | `KEYSTORE` yoksa zorunlu | Keystore dosyasının Base64 kodlanmış metni (CI kullanımı); `0700` dizinde `0600` geçici dosyaya çözülür ve çıkışta sıfırlanarak silinir. |
| `RAGO_ANDROID_KEYSTORE_PASS` | `ANDROID_KEYSTORE_PASSWORD` | İmzalama açıkken zorunlu | Keystore bütünlük ve erişim parolası. |
| `RAGO_ANDROID_KEY_ALIAS` | `ANDROID_KEY_ALIAS` | İmzalama açıkken zorunlu | Keystore içindeki özel anahtar girdisinin adı (ör. `upload`). |
| `RAGO_ANDROID_KEY_PASS` | `ANDROID_KEY_PASSWORD` | İsteğe bağlı | Özel anahtar parolası; belirtilmezse `RAGO_ANDROID_KEYSTORE_PASS` değeri kullanılır (PKCS#12 standardı). |
| `RAGO_ANDROID_REQUIRE_SIGNING` | — | İsteğe bağlı (varsayılan: `0`) | `1` verildiğinde keystore sırları tamamen boş olsa bile imzasız çıktıya düşülmez ve CMake öncesi hata verilir (`.github/workflows/ci.yml` canlı `v*` sürüm etiketlerinde `1` olarak geçirir). |
| `RAGO_ANDROID_STRICT_ALIGN` | — | İsteğe bağlı (varsayılan: `1`) | `1` (varsayılan): 16 KB ELF `LOAD` (`p_align >= 0x4000`, 2'nin kuvveti) ve `.apk` içi sıkıştırılmamış `.so` ZIP veri ofseti (`offset % 16384 == 0`, `zipalign -P 16`) kontrolünü zorunlu tutar. Yalnız yerel hata ayıklamada geçici olarak `0` verilebilir; sürüm derlemelerinde `1` kalmalıdır. |
| `RAGO_ANDROID_BUILD_TOOLS_DIR` | — | İsteğe bağlı | `zipalign` ve `apksigner` araçlarını içeren Android SDK `build-tools/<sürüm>` dizini; verilmezse `$ANDROID_SDK_ROOT/build-tools/` altındaki en güncel sürüm otomatik seçilir. |

---

## 6. Yerel Geliştirici ve GitHub Actions Kurulumu

### A. macOS veya Linux Üzerinde (`ci/build-android.sh`)

Önce yalnız imzalama anahtarını doğrulamak için:
```bash
RAGO_ANDROID_KEYSTORE="$HOME/.secure-android-keys/ragoshell-upload.jks" \
RAGO_ANDROID_KEY_ALIAS="upload" \
RAGO_ANDROID_KEYSTORE_PASS="<KEYSTORE_PAROLASI>" \
./ci/build-android.sh --validate-signing-only
```

Tam Release derleme, imzalama ve 16 KB doğrulaması için:
```bash
VCPKG_ROOT="$HOME/vcpkg" \
Qt6_DIR="$HOME/Qt/6.12.0/macos" \
Qt6_DIR_ANDROID="$HOME/Qt/6.12.0/android_arm64_v8a" \
ANDROID_SDK_ROOT="$HOME/Library/Android/sdk" \
RAGO_ANDROID_KEYSTORE="$HOME/.secure-android-keys/ragoshell-upload.jks" \
RAGO_ANDROID_KEY_ALIAS="upload" \
RAGO_ANDROID_KEYSTORE_PASS="<KEYSTORE_PAROLASI>" \
./ci/build-android.sh 2.1.0
```

### B. Windows Üzerinde (`ci/build-android.ps1`)

```powershell
$env:RAGO_ANDROID_KEYSTORE = "$HOME\.secure-android-keys\ragoshell-upload.jks"
$env:RAGO_ANDROID_KEY_ALIAS = 'upload'
$env:RAGO_ANDROID_KEYSTORE_PASS = '<KEYSTORE_PAROLASI>'

.\ci\build-android.ps1 -ValidateSigningOnly
.\ci\build-android.ps1 -Version 2.1.0
```

### C. GitHub Actions (`Repository Secrets`)

Keystore dosyasını geçici dosya bırakmadan Base64 metnine çevirin:
```bash
# macOS:
base64 -i "$HOME/.secure-android-keys/ragoshell-upload.jks" | tr -d '\n' | pbcopy

# Linux:
base64 -w 0 "$HOME/.secure-android-keys/ragoshell-upload.jks"
```

**Settings → Secrets and variables → Actions → Repository secrets** altına eklenecek sırlar (`.github/workflows/ci.yml` içindeki `android` işi bunları yalnızca `pull_request` olmayan çalışmalarda aktarır):

| GitHub Secret Adı (`docs/GitHubMirror.md` uyumlu alternatif) | Değer |
|---|---|
| `RAGO_ANDROID_KEYSTORE_BASE64` *(veya `ANDROID_KEYSTORE_BASE64`)* | Upload keystore (`.jks` / `.p12`) dosyasının tek satır Base64 metni |
| `RAGO_ANDROID_KEYSTORE_PASS` *(veya `ANDROID_KEYSTORE_PASSWORD`)* | Keystore parolası |
| `RAGO_ANDROID_KEY_ALIAS` *(veya `ANDROID_KEY_ALIAS`)* | Anahtar takma adı (ör. `upload`) |
| `RAGO_ANDROID_KEY_PASS` *(veya `ANDROID_KEY_PASSWORD`, isteğe bağlı)* | Anahtar parolası (keystore parolasından farklıysa) |

---

## 7. Google Play Data Safety (Veri Güvenliği) Özeti (`Roadmap.md` §3.4 & `TodoMac.md` M11)

Google Play Console **App content → Data safety** formu doldurulurken (Faz 7.2) uygulamanın mevcut teknik davranışı aşağıdaki gibidir:

1. **Veri Toplama ve Paylaşım (`Roadmap.md` §3.4 & §12.1 Uzlaştırma Notu):**
   - `docs/Roadmap.md` §3.4 ve `docs/TodoMac.md` M11 hedefi, istemcinin varsayılan olarak hiçbir kişisel veri, çökme raporu, reklam kimliği (`AD_ID` izni yoktur) veya telemetri toplamaması yönündedir.
   - **Önemli Kod Durumu (`Roadmap.md` §12.1 / Madde `0.9` `(insan)`):** Mevcut `src/ragoshell-app/UpdateService.cpp` (`shareAnonymousUsageEnabled()`, `docs/UpdateAndTelemetry.md`) içinde otomatik güncelleme kontrolüyle birlikte anonim sayaç paylaşımı (`shareAnonymousUsage`) şu an varsayılan `true` durumundadır (karma `installId`, açılış/kabuk/sorgu sayaçları ve MongoDB `major.minor`). Faz 7.2 Google Play mağaza gönderiminden **önce** `0.9` insan kararıyla ya (a) `shareAnonymousUsage` varsayılanı `false` (açık opt-in) yapılarak Play Data Safety formunda *"No data collected"* beyan edilmeli, ya da (b) mevcut davranış korunacaksa Play Data Safety formunda `App info and performance` / `App interactions` (anonim, şifreli aktarım, silme/kapatma ayarı) açıkça beyan edilmelidir; kod ile form beyanı birbiriyle çelişmemelidir.
2. **Yerel Sır Saklama ve Yedekleme Koruması:**
   - MongoDB parolaları, SSH özel anahtarları/parolaları ve isteğe bağlı AI API anahtarları yalnızca cihazda Android Keystore donanım destekli anahtarla şifrelenerek saklanır (`KeyStore_android.cpp`); `ragoshell.json` içinde düz metin sır tutulmaz.
   - `install/android/AndroidManifest.xml` içinde `android:allowBackup="false"` tanımlıdır; kayıtlı bağlantılar ve anahtar deposu verileri bulut yedeklemelerine aktarılmaz.
3. **Doğrudan Kullanıcı Yapılandırmalı Ağ Bağlantıları:**
   - MongoDB TCP/TLS ve SSH tünel bağlantıları yalnızca kullanıcının kendisinin girdiği hedef sunuculara doğrudan cihazdan açılır (`install/android/res/xml/network_security_config.xml`).
   - İsteğe bağlı AI sorgu asistanı varsayılan olarak kapalıdır; yalnızca kullanıcı kendi sağlayıcı API anahtarını girip özelliği açıkça çalıştırdığında şema özeti ve istem metnini doğrudan ilgili sağlayıcıya HTTPS (TLS) üzerinden gönderir.
4. **Cihaz İzinleri (`AndroidManifest.xml`):**
   - `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `ACCESS_LOCAL_NETWORK` (Android 17 yerel ağ erişimi), ve yalnızca API 28 için sınırlandırılmış `READ/WRITE_EXTERNAL_STORAGE` (`maxSdkVersion="28"`; Android 10+ üzerinde Storage Access Framework kullanılır). Kamera, mikrofon, konum veya rehber izni yoktur.

---

## 8. Doğrulama Planı (Verification Plan) ve İnsan Kontrol Listesi

### Otomatik Güvenlik ve Bütünlük Kapıları (Automated Security Check)

1. **Sır Hijyeni ve Fail-Fast İmza Doğrulaması (`ci/test-release-pipeline.mjs`):**
   - `--validate-signing-only` ve `-ValidateSigningOnly` üzerinde kısmi değişken, çakışan `KEYSTORE` + `KEYSTORE_BASE64`, yanlış yazılmış `RAGO_ANDROID_*`, hatalı Base64, yanlış parola ve bulunmayan `alias` senaryolarının CMake başlamadan sıfır dışı kodla reddedildiğinin doğrulanması.
   - Geçici üretilen gerçek bir PKCS#12 keystore ile hem `KEYSTORE` hem `KEYSTORE_BASE64` modlarının doğrulanması, süreç argümanlarında parolanın geçmediğinin ve çıkışta geçici dosyanın silindiğinin kontrolü.
2. **16 KB ELF / APK / AAB Kapısı Öz-Testi (`ci/test-release-pipeline.mjs`):**
   - Sentetik 64-bit ARM64 ELF `.so` ikilileriyle `p_align = 0x4000` (16 KB) içeren `.apk` ve `.aab` arşivlerinin `--check-align-only` kapısından geçtiğinin; `p_align = 0x1000` (4 KB) içeren `.so` veya yerel kütüphanesi eksik arşivlerin sıfır dışı çıkış koduyla engellendiğinin doğrulanması.
3. **Yayın Hattı ve SBOM Bütünlüğü (`ci/test-release-pipeline.mjs`, `ci/test-sbom-and-licenses.mjs`, `ragoshell-site`):**
   - `ragoshell-<sürüm>-android-arm64.aab` paketinin `releases.json` (`format: "aab"`, `primary: false`), `SHA256SUMS.txt` ve SPDX 2.3 SBOM çıktısına eklendiğinin; `downloads.android` birincil indirmesinin `.apk` olarak korunduğunun ve `ragoshell-site` statik derleme kapılarının kırılmadığının doğrulanması.

### İnsan Doğrulama Kontrol Listesi (Faz 3.8 Kapanışı İçin)

1. [ ] Depo dışında üretim Google Play **Upload Keystore** (`ragoshell-upload.jks`, PKCS#12, RSA 4096-bit) üretildi ve parola kasasına yedeklendi.
2. [ ] GitHub Actions `Repository Secrets` (`RAGO_ANDROID_KEYSTORE_BASE64`, `RAGO_ANDROID_KEYSTORE_PASS`, `RAGO_ANDROID_KEY_ALIAS`) tanımlandı.
3. [ ] `ci/build-android.sh` (veya CI `android` işi) ile imzalı `ragoshell-<sürüm>-android-arm64.apk` ve `ragoshell-<sürüm>-android-arm64.aab` üretildi; `apksigner verify`, `jarsigner -verify` ve 16 KB ELF kontrolleri yeşil geçti.
4. [ ] Üretilen imzalı `.apk` fiziksel bir Android tablet/cihaz üzerine (`adb install` veya doğrudan dosya kurulumu ile) kuruldu; bağlantı, QML ekranları, kabuk editörü ve Android Keystore parola saklama akışı doğrulandı; ardından `docs/Roadmap.md` §11'deki `3.8` maddesi `[x]` olarak kapatıldı.
