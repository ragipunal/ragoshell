#pragma once

namespace RagoShell
{
    /// Dev/CI helper (--tabs-selftest): exercises TabModel (current tab on
    /// open / close / move, colours, Welcome tab, row signals) and prints one
    /// PASS/FAIL line per check to stderr. Returns the failures.
    int runTabModelSelfTest();
}
