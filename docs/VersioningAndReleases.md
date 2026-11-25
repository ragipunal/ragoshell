# Sürümleme, Etiketleme ve GitHub Release Kuralları

Bu belge RagoShell için **SemVer 2.0.0** sürüm numarası kurallarını, Git etiketleme politikasını ve iki aşamalı GitHub Releases yayın akışını tanımlar. Dağıtım teknik mimarisi, Minisign imzalama ve SBOM ayrıntıları için [ReleasePipeline.md](ReleasePipeline.md) belgesine bakın.

---

## 1. SemVer 2.0.0 Sürüm Numaralama (`MAJOR.MINOR.PATCH`)

| Sürüm Türü | Git Etiketi | Ne Zaman Artırılır? | GitHub Release Tipi | Kök `releases.json` |
|---|---|---|---|---|
| **Major** | `vX.0.0` | Geriye dönük uyumsuz değişiklik: ayar (`ragoshell.json`) veya sync şeması kırılması, kriptografik/protokol kırılması, minimum desteklenen OS/Qt sürümü artışı | **Stable (`Latest`)** | Güncellenir |
| **Minor** | `vX.Y.0` | Geriye dönük uyumlu yeni özellikler (ör. `v2.1.0`, `v2.2.0`) | **Stable (`Latest`)** | Güncellenir |
| **Patch** | `vX.Y.Z` | Yalnızca hata düzeltmesi, güvenlik yaması, çeviri veya paketleme düzeltmesi (yeni büyük özellik yok) | **Stable (`Latest`)** | Güncellenir |
| **Ön Sürüm** | `vX.Y.Z-rc.N` *(veya `-alpha.N`, `-beta.N`)* | Kararlı sürüm kesilmeden önce 4 platformun kurulum paketlerini doğrulama adayı | **Pre-release** | **Güncellenmez** (stabil istemcilere gitmez) |

---

## 2. Etiketleme (Git Tag) Kuralları

1. **Ara geliştirme commit'leri etiketlenmez**: `main` dalı üzerinde işler Conventional Commits (`feat(...)`, `fix(...)`, `docs:`, `ci:`, `refactor:`) ile ilerler. Etiket yalnızca yayın kesiminde oluşturulur.
2. **Yalnızca Annotated Tag kullanılır**:
   ```bash
   git tag -a v2.1.0 -m "RagoShell v2.1.0"
   ```
   Lightweight (`git tag vX.Y.Z`) etiket kullanılmaz; etiketler hem `origin` (GitHub) hem `gitea` uzak depolarına pushlanır.
3. **Sıkı Etiket Formatı**: Yayın iş akışı yalnızca `vMAJOR.MINOR.PATCH` ve `vMAJOR.MINOR.PATCH-(alpha|beta|rc).N` kalıbındaki etiketleri kabul eder. `widgets-final` gibi tarihsel veya dahili etiketler GitHub Releases olarak yayınlanmaz.
4. **Tek Kaynak Eşleşme Şartı (CI Kapısı)**:
   - Etiketteki temel sürüm (`MAJOR.MINOR.PATCH`), `CMakeLists.txt` içindeki `PROJECT_VERSION_MAJOR/MINOR/PATCH` ile birebir aynı olmalıdır (`PROJECT_VERSION_BUILD` kararlı sürümlerde boş olmalıdır).
   - `CHANGELOG` içinde `RagoShell MAJOR.MINOR.PATCH` (veya ön sürümün tam adı) başlığı altında en az bir madde bulunmalıdır; aksi halde `ci/check-release-tag.mjs` CI ön kontrolünü (`i18n` işi) 4 platform derlemesi başlamadan durdurur.

---

## 3. Major / Minor Yayın Akışı (`rc` → Kararlı)

1. **`main` doğrulaması**: Özellikler tamamlanır, yerel/CI testler yeşil geçer, `CMakeLists.txt` sürümü ve `CHANGELOG` güncellenip `main` üzerine pushlanır.
2. **Ön sürüm (`rc.1`) doğrulaması** *(Major ve Minor sürümler için önerilen standart)*:
   - `git tag -a vX.Y.0-rc.1 -m "RagoShell vX.Y.0-rc.1"` oluşturup `origin` ve `gitea`'ya pushlayın.
   - GitHub Actions tüm 4 platformu (Windows, macOS, Linux, Android), SBOM'u ve Minisign imzasını üretip GitHub Releases üzerinde **Pre-release** olarak yayınlar; canlı `releases.json` dosyasına dokunulmaz.
   - Üretilen Windows/macOS/Linux/Android ikilileri ve release notları doğrulandıktan sonra sorun varsa `main`'e düzeltme girip `vX.Y.0-rc.2` kesilir.
3. **Kararlı yayın (`Latest`)**:
   - Aynı doğrulanmış commit üzerinde `git tag -a vX.Y.0 -m "RagoShell vX.Y.0"` oluşturulup `origin` ve `gitea`'ya pushlanır.
   - GitHub Actions `release` ortam onayının ardından GitHub Releases'ta **Latest** sürümünü, imzalı `releases.json(.minisig)`, `SHA256SUMS.txt(.minisig)` ve SPDX 2.3 SBOM varlıklarını yayınlar.
4. **Patch sürümleri (`vX.Y.Z`)**: Küçük hata/güvenlik yamalarında doğrudan `vX.Y.Z` etiketi kesilebilir veya riskli durumlarda önce `-rc.1` kullanılabilir.

---

## 4. GitHub Depo Korumaları (Önerilen İnsan Ayarları)

- **Environment `release` → Required reviewers**: Kendinizi onaylayıcı olarak ekleyin; 4 platform derlendikten sonra canlı yayın öncesi tek tık onay beklenir.
- **Repository Rulesets → Tag ruleset (`refs/tags/v*`)**: Yayınlanmış `v*` etiketlerinin silinmesini ve force-push ile değiştirilmesini engelleyin.
