#pragma once

#include <QString>
#include <QStringView>

namespace RagoShell
{
    /**
     * @brief Runtime mode of WebAssembly vs native builds (docs/WasmPlaygroundUx.md).
     *
     * Native desktop/mobile builds always resolve to DesktopOrMobile regardless of
     * environment variables so a desktop environment variable can never spoof the
     * browser Playground UI or enable the in-memory transport.
     *
     * On WebAssembly (isWasmBuild == true), RAGOSHELL_WASM_MODE defaults (and
     * fails closed on unknown values) to Playground (Mod A static showcase), while
     * "gateway" selects Gateway (Phase 2 self-hosted Mod B / Mod C).
     */
    enum class WasmRuntimeMode
    {
        DesktopOrMobile,
        Playground,
        Gateway
    };

    namespace WasmMode
    {
        /// Pure mode resolver (unit-tested in WasmMode_test.cpp).
        WasmRuntimeMode resolveRuntimeMode(bool isWasmBuild, QStringView rawEnvValue);

        /// Builds and validates `<productUrl>/download/` via UpdatePulse::isSafeDownloadUrl.
        QString resolveDownloadPageUrl(QStringView rawProductUrl);

        /// Display name for the seeded in-browser demo connection (`"<ProductName> Playground"`).
        QString defaultPlaygroundConnectionName(QStringView productName);

        /// Starter MongoDB JS script (ES2016-compatible) auto-executed on `catalog` in Playground mode.
        QString playgroundStarterScript();
    }
}
