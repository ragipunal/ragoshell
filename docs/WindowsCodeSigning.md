# Windows Kod İmzası Kılavuzu (Faz 3.3 — E1 & A9)

Yazılış: 2026-11-01. Bu belge, yol haritası Faz 3.3 (`E1` sertifika seçimi ve `A9` `ci/build-windows.ps1`
Authenticode imzalama adımı) kapsamındaki **iç operasyon ve CI yapılandırma kılavuzudur** (`ragoshell-site`
dış site `docs.publish` izin listesinde yer almaz).

---

## 1. Bağlam ve Neden Gerekli?

Windows üzerinde imzasız dağıtılan `.exe` ve `.msi` kurulum paketleri iki engelle karşılaşır:

1. **Windows Defender SmartScreen:** İndirilen kurulumcu ilk çalıştırmada mavi/kırmızı `"Windows bilgisayarınızı korudu — Bilinmeyen Yayıncı"` ekranıyla engellenir.
2. **Kullanıcı Hesabı Denetimi (UAC):** Kurulum ve kaldırma (`Uninstall.exe`) sırasında doğrulanmış yayıncı adı yerine sarı uyarı penceresi gösterilir.

Haziran 2023'te yürürlüğe giren **CA/Browser Forum** kuralları gereği, halka açık sertifika otoriteleri (CA) kod imzalama özel anahtarlarını artık düz `.pfx` dosyası olarak teslim etmemekte; anahtarın **FIPS 140-2 Level 2 / Common Criteria EAL 4+** uyumlu bir donanım (fiziksel USB token veya bulut tabanlı HSM) üzerinde üretilip saklanmasını zorunlu tutmaktadır. CI otomasyonu için bulut HSM destekli seçenekler tercih edilmelidir.

---

## 2. Sertifika Seçenekleri Karşılaştırması (Roadmap §2 E1)

Yol haritası §2 `E1` maddesinde belirlenen üç seçeneğin **maliyet**, **Türkiye ve açık kaynak / ticari model uygunluğu**, **SmartScreen itibarı** ve **CI otomasyonu** açısından karşılaştırması:

| Ölçüt | Azure Trusted Signing *(yeni adıyla Azure Artifact Signing)* | Certum Open Source Code Signing *(SimplySign Cloud)* | SSL.com OV Code Signing *(+ eSigner Cloud HSM)* |
|---|---|---|---|
| **Tahmini Maliyet** | **Basic:** ~$9,99/ay (~$120/yıl, ayda 5.000 imza dâhil; aşım $0,005/imza)<br>**Premium:** ~$99,99/ay | **Bulut (SimplySign):** ~€49–€69/yıl (ilk alım + bulut sertifikası; yenileme daha düşük) | **OV Sertifika:** ~$129/yıl<br>**eSigner Cloud HSM:** ~$180–$240/yıl<br>**Toplam:** ~$310–$370/yıl (kademeli imza kotası) |
| **Türkiye Uygunluğu** | **Doğrudan uygun değil.** Kimlik doğrulaması (Identity Validation) bireylerde ABD/Kanada, kurumlarda seçili AB/BK/Kuzey Amerika ülkeleri ile sınırlıdır; Türkiye yerleşik gerçek veya tüzel kişiler doğrudan doğrulamadan geçemez (ancak desteklenen ülkede bağlı şirket varsa kullanılabilir). | **Uygun.** Türkiye'deki bireysel açık kaynak geliştiricileri pasaport/kimlik ve ikamet belgesi ile uzaktan doğrulama yapabilir. | **Uygun.** Türkiye'de kurulu şahıs veya sermaye şirketleri (vergi levhası, ticaret sicili, D-U-N-S doğrulaması) küresel OV doğrulamasından geçebilir; bireyler için IV seçeneği de mevcuttur. |
| **Açık Kaynak (GPLv3) ve Ticari Model Uyumu** | Hem açık kaynak hem ticari yazılım dağıtımına uygundur; sertifikada doğrulanmış tüzel kişi adı görünür. | **Yalnızca açık kaynak kapsamı.** Sertifika konu alanında `"Open Source Developer, <Ad Soyad>"` yazar; sözleşmesi ticari satışı yasaklar. Uygulama GPLv3 ücretsiz olduğu için erken topluluk sürümlerinde kullanılabilir; ancak **RagoShell Cloud** abonelik gelir modeli devreye girdiğinde kurumsal OV sertifikasına geçilmelidir. | **Tam uyumlu.** Hem GPLv3 masaüstü istemcisinin ücretsiz dağıtımını hem de ticari şirket kimliğini (`RagoShell` / tüzel kişi unvanı) tek sertifikada sorunsuz karşılar. |
| **SmartScreen İtibarı** | Microsoft'un kendi kısa ömürlü sertifika zincirini kullandığı için SmartScreen itibarı **en hızlı** (çoğu zaman ilk sürümden itibaren uyarısız) kazanılır. | Yeni sertifikalarda başlangıçta sıfır itibarla başlar; güvenli indirme sayısı ve tutarlı imzalama biriktikçe birkaç sürüm içinde SmartScreen uyarısı kalkar. | Standart OV (ve güncel Microsoft politikasında EV dâhil) sertifikalar anında SmartScreen muafiyeti vermez; dosya/sertifika itibarı indirme hacmiyle oluşur. Microsoft Security Intelligence (`wdsi`) gönderimi süreci hızlandırır. |
| **CI / GitHub Actions Otomasyonu** | **Çok kolay (yerel `signtool` eklentisi).** `Microsoft.Trusted.Signing.Client` NuGet paketindeki `Azure.CodeSigning.Dlib.dll` (`/dlib` + `/dmdf`) ve Azure Service Principal / OIDC ortam değişkenleri ile doğrudan çalışır. | **Orta.** SimplySign Desktop uygulaması + TOTP (`CERTUM_OTP`) köprüsüyle sertifika Windows Sertifika Deposu'na (`CurrentUser\My`) bağlanır ve `signtool /sha1` ile imzalanır; alternatif olarak PKCS#11 (`jsign` / `osslsigncode`) kullanılır. | **Kolay–Orta.** SSL.com **eSigner CKA** (Cloud Key Adapter, Windows KSP) + TOTP sırrı kurulduğunda sertifika Windows Sertifika Deposu'na yansır ve doğrudan `signtool /sha1` ile çalışır; alternatif olarak `CodeSignTool` / resmi GitHub Action kullanılabilir. |

### İnsan Adımı İçin Önerilen Yol Haritası (`3.3`)

1. **Ticari hedef ve marka bütünlüğü (Önerilen kalıcı çözüm):** Şirket kuruluşu (Faz 0.2) tamamlandığında **SSL.com OV Code Signing + eSigner Cloud (CKA)** alınması. Türkiye'deki şirketle doğrulanabilir, yayıncı alanında tüzel kişilik unvanı görünür ve `RAGO_WIN_SIGN_SHA1` üzerinden `ci/build-windows.ps1` ile doğrudan çalışır.
2. **Şirket öncesi erken açık kaynak geçişi (Düşük bütçeli alternatif):** Faz 0.2 tamamlanana kadar erken sürümleri imzalamak istenirse **Certum Open Source Code Signing (SimplySign)** alınabilir; gelirli bulut abonelikleri (Faz 4.10) açılmadan önce kurumsal OV sertifikasına geçilir.
3. **Gelecek seçeneği:** Microsoft, Azure Artifact Signing ülke listesine Türkiye'yi eklerse veya desteklenen bir bölgede tüzel kişilik açılırsa `ci/build-windows.ps1` içindeki `RAGO_WIN_SIGN_DLIB` + `RAGO_WIN_SIGN_METADATA` modu ek kod değişikliği gerektirmeden devreye alınabilir.

---

## 3. `ci/build-windows.ps1` İmzalama Mimarisi (A9)

`ci/build-windows.ps1`, ürün kimliğini (`RAGO_PRODUCT_NAME`, `RAGO_PRODUCT_ID`, `RAGO_PRODUCT_URL`) yalnızca `cmake/RagoShellDefaults.cmake` dosyasından okur ve imzalama etkin olduğunda aşağıdaki sıra ile çalışır:

1. **Erken Yapılandırma ve Araç Denetimi (Fail-Fast):**
   - Hiçbir `RAGO_WIN_SIGN_*` ortam değişkeni tanımlı değilse açık bir uyarı mesajı basarak imzalamayı atlar (yerel geliştirici derlemeleri ve çatal PR'lar kırılmaz).
   - Herhangi bir `RAGO_WIN_SIGN_*` değişkeni tanımlıysa derleme başlamadan **önce** mod tutarlılığını, sertifika/eklenti dosyalarının varlığını ve x64 `signtool.exe` aracını doğrular. Eksik değişken, çakışan mod veya bulunamayan `signtool.exe` durumunda betik **anında sıfır dışı çıkış koduyla** durur.
2. **Paketleme Öncesi İkili İmzalama (`stage` dizini):**
   - `windeployqt` ve lisans kopyalama adımlarından hemen sonra (`--check-qml` ve arşivleme/kurulumcu üretiminden önce):
     - `dist/ragoshell-<ver>-windows-x64/ragoshell.exe`
     - `dist/ragoshell-<ver>-windows-x64/ragoshell-mcp.exe`
   - Böylece taşınabilir `.zip`, NSIS `-setup.exe` ve WiX `.msi` paketlerinin üçü de imzalı uygulama ikililerini içerir; ayrıca `--check-qml` testi doğrudan imzalanmış `ragoshell.exe` üzerinde koşar.
3. **NSIS Kaldırıcısı (`Uninstall.exe`) ve Kurulumcusu (`*-setup.exe`):**
   - NSIS 3.08 ve üzerinde `install/windows/ragoshell.nsi` içindeki `!uninstfinalize` kancası tetiklenir; `makensis` derleme sırasında ürettiği geçici kaldırıcı ikilisini imzalar ve doğrular (`= 0` denetimiyle hata anında derleme durur, komut satırına parola sızdırılmaz).
   - Ardından üretilen `dist/ragoshell-<ver>-windows-x64-setup.exe` imzalanır ve doğrulanır.
4. **WiX v5 MSI Kurulumcusu (`*.msi`):**
   - Üretilen `dist/ragoshell-<ver>-windows-x64.msi` paketi imzalanır ve doğrulanır.
5. **Zorunlu Doğrulama ve Güvenli Temizlik:**
   - İmzalanan **her** dosya için SHA-256 dosya özeti (`/fd sha256`) ve RFC 3161 SHA-256 zaman damgası (`/tr <url> /td sha256`) uygulanır; hemen ardından `signtool verify /pa /v` çalıştırılır.
   - `RAGO_WIN_SIGN_PFX_BASE64` kullanıldığında geçici `.pfx` dosyası yalnızca kısıtlı erişim izinli geçici alt dizinde açılır ve `finally` bloğunda baytları sıfırlanarak (`0x00`) diskten silinir.

---

## 4. Ortam Değişkenleri Referansı

Tam olarak **tek bir** kimlik modu seçilmelidir; birden fazla modun aynı anda verilmesi veya zorunlu eş değişkenin unutulması hatayla sonuçlanır.

| Ortam Değişkeni | Mod | Zorunluluk | Açıklama |
|---|---|---|---|
| `RAGO_WIN_SIGN_PFX` | PFX | `PFX_BASE64` yoksa zorunlu | Diskteki `.pfx` / `.p12` dosyasının tam yolu. |
| `RAGO_WIN_SIGN_PFX_BASE64` | PFX | `PFX` yoksa zorunlu | Base64 kodlanmış `.pfx` içeriği (CI sırları için; geçici dosyaya çözülüp `finally`'de sıfırlanarak silinir). |
| `RAGO_WIN_SIGN_PASSWORD` | PFX | Zorunlu | `.pfx` parolası (komut satırı çıktısına asla yazdırılmaz). |
| `RAGO_WIN_SIGN_SHA1` | Sertifika Deposu | Zorunlu | Windows Sertifika Deposu'ndaki sertifikanın 40 haneli onaltılık (hex) SHA-1 parmak izi (USB token, eSigner CKA, SimplySign Desktop). |
| `RAGO_WIN_SIGN_SUBJECT` | Sertifika Deposu | Zorunlu | Windows Sertifika Deposu'ndaki sertifikanın konu adı (`signtool /n`, ör. kuruluş veya geliştirici adı). |
| `RAGO_WIN_SIGN_STORE` | `SHA1` / `SUBJECT` | İsteğe bağlı | Sertifika deposu adı (`signtool /s`, ör. `My`). |
| `RAGO_WIN_SIGN_MACHINE_STORE` | `SHA1` / `SUBJECT` | İsteğe bağlı | `1` veya `true` ise `CurrentUser` yerine `LocalMachine` deposunu (`/sm`) kullanır. |
| `RAGO_WIN_SIGN_DLIB` | Azure Trusted Signing | Zorunlu | x64 `Azure.CodeSigning.Dlib.dll` dosyasının tam yolu (`signtool /dlib`). |
| `RAGO_WIN_SIGN_METADATA` | Azure Trusted Signing | `METADATA_JSON` yoksa zorunlu | `metadata.json` dosyasının tam yolu (`signtool /dmdf`). |
| `RAGO_WIN_SIGN_METADATA_JSON` | Azure Trusted Signing | `METADATA` yoksa zorunlu | Satır içi `metadata.json` metni (CI sırları/değişkenleri için; geçici dosyaya yazılıp `finally`'de silinir). |
| `RAGO_WIN_SIGN_TIMESTAMP_URL` | Tümü | İsteğe bağlı | RFC 3161 zaman damgası sunucusu. Varsayılan: Azure modunda `http://timestamp.acs.microsoft.com`, diğer modlarda `http://timestamp.digicert.com` (Certum için `http://time.certum.pl`, SSL.com için `http://ts.ssl.com` verilebilir). |
| `RAGO_WIN_SIGN_SIGNTOOL` | Tümü | İsteğe bağlı | `signtool.exe` tam yolu. Verilmezse Windows 10/11 SDK (`Windows Kits\10\bin\10.*\x64\signtool.exe`) en güncel sürümden başlanarak otomatik bulunur. |

---

## 5. Yerel Geliştirici Makinesinde Kurulum (PowerShell)

Önkoşul: **Windows 10/11 SDK** (`Signing Tools for Desktop Apps` bileşeni — Visual Studio Installer içinden seçilebilir).

### Seçenek A — Windows Sertifika Deposu / Bulut HSM Köprüsü (SSL.com eSigner CKA, Certum SimplySign, USB Token)

1. Sağlayıcı istemcisinde (SimplySign Desktop veya eSigner CKA) oturum açın ve sertifikanın Windows Sertifika Deposu'nda göründüğünü doğrulayın:
   ```powershell
   Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Format-List Thumbprint, Subject, NotAfter
   ```
2. Parmak izini ortam değişkenine atayıp derlemeyi çalıştırın:
   ```powershell
   $env:RAGO_WIN_SIGN_SHA1 = '0123456789ABCDEF0123456789ABCDEF01234567'
   $env:RAGO_WIN_SIGN_TIMESTAMP_URL = 'http://timestamp.digicert.com'   # Certum: http://time.certum.pl | SSL.com: http://ts.ssl.com
   .\ci\build-windows.ps1 -Config Release
   ```

### Seçenek B — Yerel PFX Dosyası

```powershell
$env:RAGO_WIN_SIGN_PFX = 'C:\path\to\codesign.pfx'
$env:RAGO_WIN_SIGN_PASSWORD = '<PFX-PAROLASI>'
.\ci\build-windows.ps1 -Config Release
```

> **Not:** Parolayı betik dosyalarına veya `ci/.env` dışındaki izlenen dosyalara yazmayın. Oturum sonunda `Remove-Item Env:\RAGO_WIN_SIGN_*` ile temizleyebilirsiniz.

### Seçenek C — Azure Trusted (Artifact) Signing

1. `.NET 8.0 Runtime (x64)`, güncel Windows SDK (`10.0.22621+`) ve `Microsoft.Trusted.Signing.Client` paketini kurun; `az login` ile `Artifact Signing Certificate Profile Signer` rolüne sahip hesapla giriş yapın.
2. `metadata.json` dosyasını (repo dışında) oluşturup derlemeyi başlatın:
   ```powershell
   $env:RAGO_WIN_SIGN_DLIB = 'C:\tools\TrustedSigning\bin\x64\Azure.CodeSigning.Dlib.dll'
   $env:RAGO_WIN_SIGN_METADATA = 'C:\secure\trusted-signing-metadata.json'
   .\ci\build-windows.ps1 -Config Release
   ```

---

## 6. GitHub Actions Üzerinde Kurulum (`Secrets`)

`.github/workflows/ci.yml` içindeki `windows` işi, depo sırları (**Settings → Secrets and variables → Actions → Repository secrets**) tanımlandığında imzalamayı otomatik olarak etkinleştirir; sırlar tanımlı değilken (veya dış çatallardan gelen Pull Request'lerde) imzasız derleme yapmaya devam eder.

> **Kota Notu:** Bulut HSM planınızda aylık imza sayısı sınırı varsa (ör. SSL.com eSigner alt paketleri), sırları doğrudan tüm dallara açmak yerine yalnızca sürüm etiketlerinde (`v*`) çalışan bir `release` ortamına (**GitHub Environments**) bağlayabilirsiniz.

### Mod 1 — Base64 PFX ile GitHub Actions (`docs/GitHubMirror.md` ile uyumlu)

1. Yerel makinede `.pfx` dosyasını tek satır Base64 dizgesine dönüştürüp panoya kopyalayın (dosya oluşturmadan):
   ```powershell
   [Convert]::ToBase64String([System.IO.File]::ReadAllBytes('C:\path\to\codesign.pfx')) | Set-Clipboard
   ```
2. GitHub deposunda aşağıdaki **Repository secrets** değerlerini ekleyin:
   - `RAGO_WIN_SIGN_PFX_BASE64` *(veya `WINDOWS_CERT_BASE64` — her ikisi de desteklenir)*: Panodaki Base64 metni.
   - `RAGO_WIN_SIGN_PASSWORD` *(veya `WINDOWS_CERT_PASSWORD`)*: PFX parolası.
   - `RAGO_WIN_SIGN_TIMESTAMP_URL` *(isteğe bağlı)*: Özel RFC 3161 zaman damgası URL'si.

### Mod 2 — Azure Trusted (Artifact) Signing ile GitHub Actions

GitHub deposunda aşağıdaki **Repository secrets** değerlerini ekleyin (`windows` işi `RAGO_WIN_SIGN_METADATA_JSON` dolu olduğunda `Microsoft.Trusted.Signing.Client` NuGet paketini otomatik indirip `RAGO_WIN_SIGN_DLIB` yolunu bağlar):

- `AZURE_TENANT_ID`: Azure Entra ID kiracı kimliği (UUID).
- `AZURE_CLIENT_ID`: Uygulama kaydı (Service Principal) istemci kimliği.
- `AZURE_CLIENT_SECRET`: Uygulama kaydı gizli anahtarı (`Artifact Signing Certificate Profile Signer` rolü atanmış olmalı).
- `RAGO_WIN_SIGN_METADATA_JSON`:
  ```json
  {
    "Endpoint": "https://neu.codesigning.azure.net/",
    "CodeSigningAccountName": "<HESAP-ADI>",
    "CertificateProfileName": "<PROFIL-ADI>"
  }
  ```

### Mod 3 — Bulut HSM KSP (SSL.com eSigner CKA / Certum SimplySign) ile GitHub Actions

1. Sağlayıcının resmi kurulum adımını (ör. eSigner CKA kurulumu ve TOTP ile sertifikanın runner sertifika deposuna yüklenmesi) `windows` işinde `Build, Test & Package` adımından hemen önce ekleyin.
2. GitHub **Repository secrets** altında:
   - `RAGO_WIN_SIGN_SHA1`: Sertifikanın 40 haneli SHA-1 parmak izi.
   - `RAGO_WIN_SIGN_TIMESTAMP_URL`: `http://ts.ssl.com` (SSL.com) veya `http://time.certum.pl` (Certum).
   - Sağlayıcıya özgü kimlik ve TOTP sırları (`ES_USERNAME`, `ES_PASSWORD`, `ES_TOTP_SECRET` veya `CERTUM_USER_ID`, `CERTUM_OTP_URI`).

---

## 7. İnsan Doğrulama Kontrol Listesi (Faz 3.3 Kapanışı İçin)

1. [ ] Seçilen CA üzerinden kod imzalama sertifikası temin edildi ve kimlik doğrulaması onaylandı.
2. [ ] GitHub deposunda (ve yerel yayın ortamında) ilgili `RAGO_WIN_SIGN_*` sırları tanımlandı.
3. [ ] Üretilen `ragoshell-<ver>-windows-x64-setup.exe`, `ragoshell-<ver>-windows-x64.msi` ve arşiv içindeki `ragoshell.exe` / `ragoshell-mcp.exe` üzerinde `signtool verify /pa /v` doğrulaması yeşil geçti.
4. [ ] Temiz bir Windows 11 makinesinde kurulumcu çalıştırıldı; UAC penceresinde doğrulanmış yayıncı adı göründü ve NSIS kurulumu sonrasında `Uninstall.exe` dijital imzasının geçerli olduğu teyit edildi.
5. [ ] İlk imzalı sürüm gerekirse Microsoft Security Intelligence (`https://www.microsoft.com/wdsi/filesubmission`) üzerinden gönderilerek SmartScreen itibarı doğrulandı ve `docs/Roadmap.md` §11'deki `3.3` maddesi `[x]` olarak kapatıldı.
