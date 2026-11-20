#pragma once

namespace RagoShell
{
    /// IDBFS write-back for the browser build (docs/WebStorage.md §1).
    /// Under Q_OS_WASM this fires an async FS.syncfs(false) so MEMFS writes
    /// under /persist reach IndexedDB; everywhere else it is a no-op so
    /// call sites need no #ifdef.
    void requestPersistSync();
}
