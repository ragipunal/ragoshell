#pragma once

namespace RagoShell
{
    /// Dev/CI helper (--drafts-selftest): exercises ScriptDraftModel (tab
    /// drafts, the recovery list, claiming, discarding, the startup prompt and
    /// the file on disk) in a temporary folder and prints one PASS/FAIL line
    /// per check to stderr. Returns the failures.
    int runDraftsSelfTest();
}
