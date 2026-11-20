// IDBFS persistence for the WASM build (docs/WebStorage.md §1, roadmap 8.2).
// Linked with --pre-js: registers a preRun hook that mounts IDBFS at /persist
// and blocks main() via a run dependency until IndexedDB has been populated,
// so SettingsManager sees persisted JSON on its first read. Any failure only
// logs to the console — the app must still boot on a fresh/in-memory volume.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
    if (typeof FS === 'undefined' || typeof IDBFS === 'undefined') {
        console.warn('IDBFS unavailable; browser persistence is off for this session.');
        return;
    }
    addRunDependency('ragoshell-idbfs');
    try {
        FS.mkdirTree('/persist'); // tolerates a pre-existing directory
        FS.mount(IDBFS, {}, '/persist');
        FS.syncfs(true, function (err) {
            if (err)
                console.error('IDBFS populate failed:', err);
            removeRunDependency('ragoshell-idbfs');
        });
    } catch (e) {
        console.error('IDBFS mount failed:', e);
        removeRunDependency('ragoshell-idbfs');
    }
});

// Last-chance write-back when the tab closes right after a save. 'pagehide'
// and 'visibilitychange' cover browsers that drop async work on unload or
// ignore 'beforeunload' entirely (mobile Safari).
function ragoshellPersistFlush() {
    try {
        if (typeof FS !== 'undefined' && FS.syncfs)
            FS.syncfs(false, function () {});
    } catch (e) { /* shutting down anyway */ }
}
if (typeof window !== 'undefined') {
    window.addEventListener('beforeunload', ragoshellPersistFlush);
    window.addEventListener('pagehide', ragoshellPersistFlush);
    document.addEventListener('visibilitychange', function () {
        if (document.visibilityState === 'hidden')
            ragoshellPersistFlush();
    });
}
