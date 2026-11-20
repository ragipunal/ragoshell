#include "ragoshell/core/utils/WasmPersist.h"

#if defined(Q_OS_WASM)
#include <emscripten.h>
#endif

namespace RagoShell
{
    void requestPersistSync()
    {
#if defined(Q_OS_WASM)
        // Fire-and-forget: syncfs only flushes dirty MEMFS nodes to IndexedDB.
        // The beforeunload hook in wasm/persist-pre.js covers tab-closing races.
        EM_ASM({
            try {
                if (typeof FS !== 'undefined' && FS.syncfs)
                    FS.syncfs(false, function(err) {
                        if (err)
                            console.error('IDBFS write-back failed:', err);
                    });
            } catch (e) {
                console.error('IDBFS write-back threw:', e);
            }
        });
#endif
    }
}
