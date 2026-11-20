#pragma once

namespace RagoShell
{
    /// Dev/CI helper (--calltip-selftest): checks ScintillaQuickItem::shellCallTip
    /// on real editor text (UTF-8 positions, '(' location) and prints one
    /// PASS/FAIL line per check to stderr. Returns the failures.
    int runCallTipSelfTest();
}
