# macOS Kod İmzası, Noter Onayı ve `.dmg` Paketleme Kılavuzu (Faz 3.4 — M6 & E2)

Yazılış: 2026-11-01. Bu belge, yol haritası Faz 3.4 (`M6` ve `E2`: `Developer ID Application` kod imzası,
Apple Notarization ve `.dmg` kalıbı üretimi) kapsamındaki **iç operasyon ve CI yapılandırma kılavuzudur**
(`ragoshell-site` dış site `docs.publish` izin listesinde yer almaz).

---

## 1. Bağlam ve Sertifika Türü Farkı (`Developer ID Application` vs `Apple Distribution`)

macOS üzerinde internetten (tarayıcı üzerinden Gitea/GitHub Releases veya `dl.ragoshell.com.tr` adresinden)
indirilen her arşiv veya disk kalıbına işletim sistemi tarafından `com.apple.quarantine` özniteliği eklenir.
`ad-hoc` (`codesign -s -`) imzalı paketler yerel geliştirme makinesinde çalışsa da başka bir Mac'te
**Gatekeeper** tarafından *"geliştirici doğrulanamadığı / kötü amaçlı yazılım denetimi yapılamadığı için açılamıyor"*
hatasıyla engellenir (macOS 15 Sequoia ve sonrasında sağ-tık › Aç kısayolu da kaldırılıp Sistem Ayarları ›
Gizlilik ve Güvenlik onayı zorunlu kılınmıştır).

Apple ekosisteminde sertifika türleri kesin sınırlarla ayrılmıştır; **yanlış sertifika türü ile Gatekeeper onayı alınamaz**:

| Sertifika Türü | Kullanım Amacı | Doğrudan `.dmg` / `.tar.gz` Dağıtımında Çalışır mı? |
|---|---|---|
| **`Developer ID Application`** | Mac App Store **dışında**, web sitesinden doğrudan dağıtılan `.app` paketlerini ve `.dmg` disk kalıplarını imzalamak ve noter onayından (`notarytool`) geçirmek için kullanılır. | **Evet — zorunlu sertifika budur.** |
| `Developer ID Installer` | Yalnızca düz `.pkg` kurulum paketlerini (`productsign`) imzalamak içindir; `.app` veya `.dmg` imzalayamaz. | Hayır (`.pkg` kullanılmıyor). |
| `Apple Distribution` | Yalnızca **App Store Connect / TestFlight** yüklemeleri (macOS App Store ve iOS/iPadOS mağaza arşivleri) içindir. | **Hayır.** Mağaza dışında Gatekeeper tarafından reddedilir ve noter servisi kabul etmez. |
| `Apple Development` | Yalnızca geliştiricinin kendi kayıtlı cihazlarında yerel derleme/hata ayıklama içindir. | **Hayır.** |

> **Mevcut Durum (`docs/MacOsWork.md` §0 & A5):** Şirket geliştirici takımında (`R28L876F4M`) bugün yalnızca mağaza
> için `Apple Distribution` sertifikası bulunmaktadır; doğrudan dağıtım için aynı şirket takımında bir
> **`Developer ID Application`** sertifikası üretilmelidir.

---

## 2. Apple Developer Program'da `Developer ID Application` Sertifikası Üretimi (İnsan Adımı)

> **Yetki Kuralı:** Apple Developer Program şirket/organizasyon takımlarında `Developer ID` sertifikalarını
> yalnızca **Account Holder (Hesap Sahibi)** rolündeki kullanıcı oluşturabilir (`Admin` veya `Developer` rolü bu
> seçeneği göremez). Ayrıca takım başına aktif `Developer ID Application` sertifika sayısı Apple tarafından sıkı
> sınırlandırılmıştır ve iptal edilmesi (revoke) daha önce imzalanmış çevrimdışı kopyaları etkiler; bu nedenle
> üretilen sertifika ve özel anahtarı (`.p12`) mutlaka güvenli parola yöneticisinde yedeklenmelidir.

### Adım A — Mac Üzerinde CSR (Certificate Signing Request) Oluşturma

1. Mac'te **Keychain Access (Anahtar Zinciri Erişimi)** uygulamasını açın.
2. Üst menüden **Keychain Access → Certificate Assistant → Request a Certificate From a Certificate Authority...** seçeneğine tıklayın.
3. Formu doldurun:
   - **User Email Address:** Apple Developer hesabınızın e-posta adresi.
   - **Common Name:** `RagoShell Developer ID` (özel anahtarın Keychain içindeki etiketi olur).
   - **CA Email Address:** Boş bırakın.
   - **Request is:** **Saved to disk** seçeneğini işaretleyin (**Let me specify key pair information** kutusunu işaretlemenize gerek yoktur; varsayılan **RSA 2048-bit** Apple'ın istediği formattır).
4. **Continue** düğmesine basıp `CertificateSigningRequest.certSigningRequest` dosyasını yerel diskinize kaydedin (bu dosyayı asla git deposuna koymayın).

### Adım B — Apple Developer Portalında Sertifika Üretme ve İndirme

1. **Account Holder** hesabıyla `https://developer.apple.com/account/resources/certificates/add` adresine gidin (sağ üstte şirket takımının seçili olduğunu doğrulayın).
2. **Software** başlığı altında **`Developer ID Application`** seçeneğini işaretleyin *(dikkat: `Apple Distribution` veya `Developer ID — G2` altındaki `Installer` değil, **`Developer ID Application`**)* ve **Continue** deyin.
   - Profil tipi sorulursa **G2 Sub-CA (Xcode 11.4.1 and later)** varsayılanını koruyun.
3. Az önce kaydettiğiniz `.certSigningRequest` dosyasını yükleyip **Continue** düğmesine basın.
4. Üretilen `developerID_application.cer` dosyasını indirin.

### Adım C — Keychain Kurulumu ve Ara Sertifika (G2) Doğrulaması

1. İndirdiğiniz `developerID_application.cer` dosyasına çift tıklayarak **login** anahtar zincirine ekleyin (`security import developerID_application.cer -k ~/Library/Keychains/login.keychain-db` komutu da kullanılabilir).
2. Güncel `Developer ID Application` sertifikaları Apple'ın **Developer ID Certification Authority (G2)** ara sertifikasıyla imzalanır. Zincirin eksiksiz olduğundan emin olmak için sistemde yoksa Apple'ın resmi ara sertifikasını (`https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer`) indirip kurun.
3. Terminalde geçerli kod imzalama kimliğini doğrulayın:
   ```bash
   security find-identity -v -p codesigning
   ```
   Çıktıda şu biçimde bir satır görünmelidir:
   ```text
   1) 0123456789ABCDEF0123456789ABCDEF01234567 "Developer ID Application: <Şirket Unvanı> (<TEAM_ID>)"
   ```
   Buradaki `"Developer ID Application: <Şirket Unvanı> (<TEAM_ID>)"` metni (veya 40 haneli SHA-1 parmak izi) `RAGO_MAC_SIGN_ID` değerinizdir.

### Adım D — CI İçin `.p12` Dışa Aktarma ve Yedekleme

1. **Keychain Access** içinde **login → My Certificates** sekmesini açın.
2. **`Developer ID Application: <Şirket Unvanı> (<TEAM_ID>)`** satırını genişletip hem sertifikayı hem altındaki özel anahtarı birlikte seçin, sağ tıklayıp **Export 2 items...** deyin.
3. Dosya biçimi olarak **Personal Information Exchange (`.p12`)** seçip **repo dışındaki** güvenli bir klasöre kaydedin.
4. Güçlü ve rastgele bir dışa aktarma parolası belirleyin (bu parola `RAGO_MAC_CERT_PASSWORD` sırrı olacaktır) ve `.p12` dosyası ile parolayı şirket parola kasasına yedekleyin.
5. Geçici `.certSigningRequest` ve `.cer` dosyalarını diskinizden silebilirsiniz.

---

## 3. App Store Connect Notary API Anahtarı (`.p8`) Oluşturma (CI İçin Önerilen)

GitHub Actions gibi başsız (headless) ortamlarda Apple ID + uygulama-özgü parola yerine süresi dolmayan ve tek tıkla iptal edilebilen **App Store Connect API Key** kullanılması Apple'ın resmi önerisidir:

1. `https://appstoreconnect.apple.com/access/integrations/api` adresine (**Users and Access → Integrations → Team Keys**) giriş yapın.
2. **Generate API Key** (`+`) düğmesine tıklayın:
   - **Name:** `ragoshell-ci-notary`
   - **Access (Rol):** **`Developer`** (Noter gönderimi için en düşük yetkili yeterli roldür; `Admin` vermenize gerek yoktur).
3. Anahtar üretildiğinde ekrandaki iki bilgiyi not edin:
   - **Issuer ID** (sayfanın üst kısmındaki UUID, ör. `69a6de70-xxxx-xxxx-xxxx-xxxxxxxxxxxx`) → `RAGO_MAC_NOTARY_ISSUER`
   - **Key ID** (10 karakterli alfasayısal kod, ör. `2X9R4HXF34`) → `RAGO_MAC_NOTARY_KEY_ID`
4. **Download API Key** bağlantısına tıklayarak `AuthKey_<KEY_ID>.p8` dosyasını indirin (**yalnızca bir kez indirilebilir**; parola kasasına yedekleyin, git deposuna asla eklemeyin).

---

## 4. `ci/build-macos.sh` İmzalama, `.dmg` ve Noter Mimarisi

`ci/build-macos.sh`, ürün kimliğini (`RAGO_PRODUCT_NAME`, `RAGO_PRODUCT_ID`, `RAGO_BUNDLE_ID`) yalnızca `cmake/RagoShellDefaults.cmake` dosyasından okur ve aşağıdaki adımlarla çalışır:

### Adım 1 — Erken Yapılandırma ve Araç Denetimi (Fail-Fast)
- Hiçbir imzalama veya noter ortam değişkeni tanımlı değilse açık bir uyarı mesajı basarak mevcut **`ad-hoc`** imzalama davranışını korur; yerel geliştirici derlemeleri ve PR iş akışları kırılmaz.
- Herhangi bir `RAGO_MAC_*` (veya `APPLE_*` eş anlamlısı) tanımlıysa CMake derlemesi başlamadan **önce**:
  - Yanlış yazılmış / tanınmayan `RAGO_MAC_*` / `RAGO_MACOS_*` değişkenlerini (sessizce `ad-hoc` imzalamaya düşmemek için),
  - Kısmi/eksik değişkenleri (ör. `RAGO_MAC_SIGN_ID` olmadan noter değişkeni verilmesi, `.p12` verilip parolanın unutulması, API anahtarı üçlüsünden birinin eksik olması),
  - Yanlış sertifika türlerini (`Apple Distribution`, `Apple Development`, `Developer ID Installer` veya `-`),
  - Çakışan modları (ör. hem `RAGO_MAC_NOTARY_PROFILE` hem API anahtarı verilmesi, hem `RAGO_MAC_CERT_P12` hem `RAGO_MAC_CERT_P12_BASE64` verilmesi veya `RAGO_MAC_SIGN_ID` ile `APPLE_DEVELOPER_ID` değerlerinin uyuşmaması),
  - Gerekli araçları (`codesign`, `hdiutil`, `security`, `openssl`, `plutil`, noter açıkken `xcrun notarytool`, `xcrun stapler`, `ditto`, `spctl`) ve `install/macosx/entitlements.plist` sözdizimini doğrular.
- Herhangi bir tutarsızlıkta derleme başlamadan sıfır dışı çıkış koduyla (`exit 1`) durur. `--validate-signing-only` bayrağı ile C++ derlemesi çalıştırılmadan yalnız imzalama/anahtar zinciri doğrulaması test edilebilir.

### Adım 2 — İzole Geçici Keychain (Ephemeral Keychain) ve Sır Hijyeni
- İmzalama ve noter sırları (`*_BASE64`, `*_PASSWORD`, `APPLE_API_KEY`) betik başında yerel değişkenlere alınıp dışa aktarılan ortamdan (`unset`) temizlenir; böylece CMake, Ninja, `vcpkg`, birim testleri ve `--check-qml` alt süreçleri hiçbir sırrı miras almaz.
- `RAGO_MAC_CERT_P12` veya `RAGO_MAC_CERT_P12_BASE64` + `RAGO_MAC_CERT_PASSWORD` verildiğinde kullanıcının `login.keychain-db` dosyasına **dokunulmaz**.
- `$ci_tmp/sign` (`chmod 700`) altında tek kullanımlık rastgele parolayla (`openssl rand -hex 32`) geçici `codesign.keychain-db` oluşturulur; `security` alt komutları `security -q -i` standart girdisi (`stdin`) üzerinden çalıştırılarak parolaların süreç argüman listesinde (`ps`) görünmesi engellenir; sertifika ve özel anahtar dışa aktarılamaz (`-x`) bayrağıyla içe aktarılır, `security set-key-partition-list -S apple-tool:,apple:,codesign:` ile başsız `codesign` erişimi tanımlanır ve geçici anahtar zinciri mevcut arama listesinin önüne eklenir (mevcut anahtar zinciri yollarındaki boşluk ve NFD/NFC UTF-8 karakterleri dizi olarak korunur).
- İçe aktarılan kimliğin `RAGO_MAC_SIGN_ID` ile eşleştiği ve geçerli bir `Developer ID Application:` sertifikası olduğu 40 haneli SHA-1 parmak izine çözümlenerek derleme başlamadan doğrulanır.
- `trap cleanup EXIT` (`HUP`, `INT` ve `TERM` sinyalleri dâhil) bloğunda kullanıcı anahtar zinciri arama listesi eski hâline döndürülür, geçici anahtar zinciri kilitlenip sıfırlanarak silinir ve çözülmüş `.p12` / `.p8` dosyaları sıfır baytlarla (`0x00`) üzerine yazılarak diskten kaldırılır. Gizli değerler ve parolalar hiçbir koşulda loglara yazdırılmaz (`set +x`).

### Adım 3 — İçten Dışa (Inside-Out) Hardened Runtime İmzalama ve QML Kapısı
- `macdeployqt` sonrasında başsız `--check-qml` denetiminin Hardened Runtime kütüphane doğrulaması (Library Validation) altında aynı takım imzasıyla çalışabilmesi için `$Qt6_DIR/plugins/platforms/libqoffscreen.dylib` uygulama paketinin `Contents/PlugIns/platforms/` dizinine kopyalanır ve `@rpath` bağımlılıkları yerel paket çerçevelerine yönlendirilir.
- `Developer ID` modunda `install/macosx/entitlements.plist` (`com.apple.security.cs.allow-jit` ve `com.apple.security.cs.allow-unsigned-executable-memory` — QML V4 ve gömülü `QJSEngine` kabuğunun JIT derleyicisi için zorunlu asgari izinler; `disable-library-validation` **açılmaz**) kullanılarak içten dışa imzalanır:
  1. `Contents` altındaki tüm `.dylib` ve `.so` dosyaları (`PlugIns`, `Resources/qml`, `Frameworks/*.dylib`),
  2. `Contents/PlugIns` altındaki varsa ek `.bundle` dizinleri ve `Contents/Frameworks/*.framework` çatıları,
  3. `Contents/MacOS/ragoshell-mcp` ikilisi (`--options runtime --timestamp --entitlements`),
  4. Son olarak `RagoShell.app` ana paketi (`--options runtime --timestamp --entitlements`).
- Hemen ardından `codesign --verify --deep --strict --verbose=2` ve üç dilde (`en`, `ar`, `ja`) başsız `--check-qml` yükleme testi bizzat imzalanmış paket üzerinde çalıştırılır.

### Adım 4 — `.dmg` Kalıbı, Noter Gönderimi (`notarytool`), Zımbalama (`stapler`) ve `spctl`
- Sürükle-bırak kurulum için geçici bir klasörde imzalı `RagoShell.app`, `/Applications` sembolik bağı (`ln -s /Applications`) ve `licenses/` klasörü hazırlanır; `hdiutil create -volname ... -format UDZO` ile sıkıştırılmış salt-okunur `dist/<product_id>-<version>-macos-arm64.dmg` üretilir ve `codesign` ile imzalanır.
- Noter kimlik bilgileri (`RAGO_MAC_NOTARY_PROFILE` veya API anahtarı üçlüsü) tanımlıysa:
  1. `.dmg` kalıbı `xcrun notarytool submit --wait --output-format json` ile Apple Noter servisine gönderilir (`.dmg` gönderimi hem disk kalıbını hem içindeki `RagoShell.app` ve tüm alt ikilileri tek seferde noter onayından geçirir).
  2. Yanıt JSON olarak ayrıştırılır; durum `"Accepted"` değilse `xcrun notarytool log <id>` otomatik çekilerek Apple'ın hata raporu ekrana basılır ve sıfır dışı kodla çıkılır (bazı Xcode sürümlerinde `Invalid` durumunda `notarytool`'un `0` döndürmesi riski engellenir).
  3. `xcrun stapler staple` ve `xcrun stapler validate` hem `.dmg` kalıbı hem de `stage` içindeki `RagoShell.app` üzerinde çalıştırılır (böylece ardından üretilen `.tar.gz` arşivi de çevrimdışı geçerli noter biletini taşır).
  4. `spctl -a -vv -t execute` (`RagoShell.app`) ve `spctl -a -vv -t open --context context:primary-signature` (`.dmg`) ile Gatekeeper kabulü doğrulanır.
- Son adımda `dist/<product_id>-<version>-macos-arm64.tar.gz` arşivi oluşturulur ve her iki dağıtım çıktısının varlığı doğrulanır.

### Adım 5 — Tehdit Modeli ve Güvenlik Kontrolleri Özeti

| Tehdit / Hata Senaryosu | Uygulanan Kontrol |
|---|---|
| CI koşucusunda veya yerel Mac'te `login.keychain-db` kirlenmesi / kilitlenmesi | Sertifika içe aktarma yalnız `$ci_tmp/sign` altındaki geçici anahtar zincirinde yapılır; arama listesi dizi olarak yedeklenip `EXIT` tuzak fonksiyonunda geri yüklenir. |
| Parola veya özel anahtarın CI loglarına / süreç listesine sızması | Base64 çözme `node` ortam değişkeni üzerinden `0600` izinle yapılır; gizli bloklarda `set +x` zorlanır; parola ve anahtar içerikleri hiçbir hata veya bilgi satırında yazdırılmaz; çıkışta `.p12`/`.p8` sıfırlanarak silinir. |
| Yanlış/eksik CI sırrı yüzünden sürüm etiketinin sessizce `ad-hoc` çıkması | Tek bir imzalama/noter değişkeni dahi verilmişse eksik eş değişken, geçersiz Base64/PEM veya bulunamayan kimlik derleme başlamadan `exit 1` ile durdurulur. |
| Hardened Runtime altında harici Qt eklentisi (`TeamIdentifier` uyuşmazlığı) | `disable-library-validation` açmak yerine `libqoffscreen.dylib` paket içine alınıp aynı `Developer ID` ile imzalanır; `otool -L` ile ana makine `Qt6_DIR` sızıntısı denetlenir. |
| `notarytool submit --wait` çağrısının `Invalid` statüsünde `0` dönmesi | JSON çıktısındaki `status === "Accepted"` açıkça denetlenir; aksi durumda `notarytool log` dökümü alınıp derleme başarısız sonlandırılır. |

---

## 5. Ortam Değişkenleri Referansı

`docs/GitHubMirror.md` §4.B'deki `APPLE_*` adları ile `RAGO_MAC_*` adlarının her ikisi de desteklenir (ikisi birden farklı değerlerle verilirse çakışma hatası üretilir).

| Ortam Değişkeni | Eş Anlamlısı (Alias) | Mod / Kapsam | Zorunluluk | Açıklama |
|---|---|---|---|---|
| `RAGO_MAC_SIGN_ID` | `APPLE_DEVELOPER_ID` | İmzalama (Tümü) | İmzalama açıkken zorunlu | `"Developer ID Application: <Şirket> (<TEAM_ID>)"` tam adı veya 40 haneli SHA-1 parmak izi. |
| `RAGO_MAC_CERT_P12` | — | Geçici Keychain | `P12_BASE64` yoksa (CI/dosya modu) | Diskteki `.p12` sertifika + özel anahtar dosyasının yolu (yerel Keychain kullanılıyorsa verilmez). |
| `RAGO_MAC_CERT_P12_BASE64` | `APPLE_CERT_P12_BASE64` | Geçici Keychain | `CERT_P12` yoksa (CI modu) | `.p12` dosyasının Base64 kodlanmış metni; geçici `0600` dosyaya çözülür ve çıkışta sıfırlanarak silinir. |
| `RAGO_MAC_CERT_PASSWORD` | `APPLE_CERT_PASSWORD` | Geçici Keychain | `.p12` verildiğinde zorunlu | `.p12` dışa aktarma parolası (loglara asla yazdırılmaz). |
| `RAGO_MAC_NOTARY_PROFILE` | — | Noter — Keychain Profili | Yerel noter modunda zorunlu | `xcrun notarytool store-credentials` ile kaydedilmiş profil adı. API anahtarı değişkenleriyle birlikte kullanılamaz. |
| `RAGO_MAC_NOTARY_KEY_ID` | `APPLE_API_KEY_ID` | Noter — API Anahtarı | API anahtarı modunda zorunlu | App Store Connect API Key ID (ör. `2X9R4HXF34`). |
| `RAGO_MAC_NOTARY_ISSUER` | `APPLE_API_ISSUER` | Noter — API Anahtarı | API anahtarı modunda zorunlu | App Store Connect Issuer UUID (ör. `69a6de70-xxxx-xxxx-xxxx-xxxxxxxxxxxx`). |
| `RAGO_MAC_NOTARY_KEY_FILE` | — | Noter — API Anahtarı | `KEY_BASE64` yoksa zorunlu | Diskteki `AuthKey_<KEY_ID>.p8` dosyasının tam yolu. |
| `RAGO_MAC_NOTARY_KEY_BASE64` | `APPLE_API_KEY` | Noter — API Anahtarı | `KEY_FILE` yoksa zorunlu | `AuthKey_<KEY_ID>.p8` dosyasının Base64 kodlanmış metni (`APPLE_API_KEY` ham PEM metnini de kabul eder). |
| `RAGO_MAC_NOTARY_TIMEOUT` | — | Noter (İsteğe bağlı) | İsteğe bağlı | `notarytool submit --wait` bekleme tavanı (varsayılan: `30m`; biçim: `1800s`, `30m`, `1h`). |

---

## 6. Yerel Geliştirici Mac'inde Kurulum ve Çalıştırma

Önkoşul: Bölüm 2'deki `Developer ID Application` sertifikası **login** anahtar zincirine kurulmuş olmalıdır (masaüstü oturumunda Keychain kilidi açık olmalıdır; uzak SSH oturumunda çalıştırılacaksa geçici `.p12` modu veya önceden kilidi açılmış Keychain gerekir).

### 1. Tek Seferlik Noter Profili Kaydı

App Store Connect API anahtarı ile (önerilen):
```bash
xcrun notarytool store-credentials "rago-notary" \
    --key /secure/path/AuthKey_<KEY_ID>.p8 \
    --key-id "<KEY_ID>" \
    --issuer "<ISSUER_UUID>"
```

Veya Apple ID + uygulama-özgü parola (`https://appleid.apple.com` → App-Specific Passwords) ile (parolayı komut satırına yazmayın; komutu çalıştırınca gizli istemde girin):
```bash
xcrun notarytool store-credentials "rago-notary" \
    --apple-id "apple-id@alanadi.com" \
    --team-id "<TEAM_ID>"
```

### 2. İmzalı ve Noter Onaylı Paket Üretimi

Önce yalnız imzalama yapılandırmasını hızlıca doğrulamak için:
```bash
RAGO_MAC_SIGN_ID="Developer ID Application: <Şirket Unvanı> (<TEAM_ID>)" \
RAGO_MAC_NOTARY_PROFILE="rago-notary" \
./ci/build-macos.sh --validate-signing-only
```

Tam derleme, test, imzalama, noter onayı ve `.dmg` + `.tar.gz` üretimi için:
```bash
RAGO_MAC_SIGN_ID="Developer ID Application: <Şirket Unvanı> (<TEAM_ID>)" \
RAGO_MAC_NOTARY_PROFILE="rago-notary" \
./ci/build-macos.sh 2.1.0
```

Çıktılar:
- `dist/ragoshell-2.1.0-macos-arm64.dmg` (Developer ID imzalı, noter onaylı ve bileti zımbalanmış sürükle-bırak disk kalıbı)
- `dist/ragoshell-2.1.0-macos-arm64.tar.gz` (içindeki `RagoShell.app` Developer ID imzalı, noter onaylı ve bileti zımbalanmış arşiv)

---

## 7. GitHub Actions Üzerinde Kurulum (`Repository Secrets`)

`.github/workflows/ci.yml` içindeki `macos` işi, `push` / `workflow_dispatch` / etiket (`v*`) tetiklemelerinde aşağıdaki sırlar (**Settings → Secrets and variables → Actions → Repository secrets**) tanımlandığında geçici Keychain + App Store Connect Notary API akışını otomatik devreye alır; sırlar tanımlı değilken veya `pull_request` tetiklemelerinde (üretim imzalama anahtarlarını PR koduna açmamak için) `ad-hoc` imzalı `.dmg` ve `.tar.gz` üretmeye devam eder.

### 1. Mac Üzerinde `.p12` ve `.p8` Dosyalarını Panoya Base64 Kodlama (Geçici Dosya Bırakmadan)

```bash
# 1) Developer ID .p12 dosyasını tek satır Base64 olarak panoya kopyala:
base64 -i /secure/path/DeveloperID.p12 | tr -d '\n' | pbcopy

# 2) App Store Connect AuthKey_<KEY_ID>.p8 dosyasını tek satır Base64 olarak panoya kopyala:
base64 -i /secure/path/AuthKey_XXXXXXXXXX.p8 | tr -d '\n' | pbcopy
```

### 2. GitHub Deposuna Eklenecek Sırlar

| GitHub Secret Adı (`docs/GitHubMirror.md` uyumlu alternatif) | Değer |
|---|---|
| `RAGO_MAC_SIGN_ID` *(veya `APPLE_DEVELOPER_ID`)* | `Developer ID Application: <Şirket Unvanı> (<TEAM_ID>)` |
| `RAGO_MAC_CERT_P12_BASE64` *(veya `APPLE_CERT_P12_BASE64`)* | `.p12` dosyasının Base64 metni |
| `RAGO_MAC_CERT_PASSWORD` *(veya `APPLE_CERT_PASSWORD`)* | `.p12` dışa aktarma parolası |
| `RAGO_MAC_NOTARY_KEY_ID` *(veya `APPLE_API_KEY_ID`)* | 10 haneli App Store Connect API Key ID |
| `RAGO_MAC_NOTARY_ISSUER` *(veya `APPLE_API_ISSUER`)* | App Store Connect Issuer UUID |
| `RAGO_MAC_NOTARY_KEY_BASE64` *(veya `APPLE_API_KEY`)* | `AuthKey_<KEY_ID>.p8` dosyasının Base64 metni |

---

## 8. İnsan Doğrulama Kontrol Listesi (Faz 3.4 Kapanışı İçin)

1. [ ] Apple Developer Program şirket takımında Account Holder hesabıyla **`Developer ID Application`** sertifikası üretildi, `.p12` + parolası güvenli kasaya yedeklendi.
2. [ ] App Store Connect üzerinde `Developer` yetkili Notary API anahtarı (`.p8`, `Key ID`, `Issuer ID`) üretildi ve yedeklendi.
3. [ ] Yerel Mac'te (`RAGO_MAC_NOTARY_PROFILE`) ve GitHub Actions'ta (`RAGO_MAC_*` sırları) yapılandırma tanımlandı.
4. [ ] `ci/build-macos.sh` çıktısında hem `RagoShell.app` hem `ragoshell-<ver>-macos-arm64.dmg` için `spctl` doğrulaması `accepted source=Notarized Developer ID` döndürdü.
5. [ ] Derlemeyi **yapmayan** ikinci bir Mac'te (veya tarayıcıdan indirilip `xattr -l` ile `com.apple.quarantine` taşıdığı doğrulanan `.dmg` üzerinde) kalıp çift tıklanarak açıldı, `RagoShell.app` `/Applications` klasörüne sürüklendi, Gatekeeper uyarısı çıkmadan açıldı; bir MongoDB bağlantısı, kabuk sorgusu (JIT) ve QML ekranları çalıştırıldı; ardından `docs/Roadmap.md` §11'deki `3.4` maddesi `[x]` olarak kapatıldı.
