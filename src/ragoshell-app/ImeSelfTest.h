#pragma once

namespace RagoShell
{
    /// Dev/CI helper (--ime-selftest): drives the editor item with scripted
    /// QInputMethodEvents (CJK composition, commit, cancel, replacement) and
    /// prints one PASS/FAIL line per check to stderr. Returns the failures.
    int runInputMethodSelfTest();
}
