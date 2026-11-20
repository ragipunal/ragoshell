## Summary

<!-- Briefly describe what changed and why. Reference any related issue. -->

## Verification Gates

<!-- Check all applicable items before requesting review (see CONTRIBUTING.md). -->

- [ ] **Build:** Touched CMake preset builds cleanly (`windows`, `macos-*`, `linux-*`, `ios`, or `android-*`)
- [ ] **Unit tests:** `rago_unit_tests` passes with zero regressions
- [ ] **QML check:** `ragoshell --check-qml` exits `0` (plus `--lang ar` and `--lang ja` if UI layout or i18n changed)
- [ ] **Translations (if `qsTr` / `tr` text changed):** Ran `update_translations`, refreshed all 15 `.ts` catalogs under `src/ragoshell-app/i18n/`, and `node ci/check-i18n.mjs` exits `0`
- [ ] **House rules (`CONTRIBUTING.md`):** Uses `Theme` / `UiDensity` tokens (no hardcoded sizes), page-width responsive layout (single column `< 520 dp`, `HScrollRow` toolbars), ES2016 `QJSEngine` syntax in shell JS, `KeyStore` for secrets, single-source brand from `cmake/RagoShellDefaults.cmake`, and LF line endings
- [ ] **Documentation:** Updated relevant files under `docs/` (and included `docs/<Topic>.md` for multi-phase or security-sensitive changes)
- [ ] **Security & GPLv3 compliance:** Contains **no** secrets, credentials, private keys, or `.env` files, and all code/assets are compatible with **GNU GPLv3** (`LICENSE`)
