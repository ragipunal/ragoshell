#pragma once

#include <QString>

namespace RagoShell
{
    /// Dev/CI helper (--shell-selftest=host:port): runs a fixed set of scripts through ScriptEngine
    /// (the embedded shell) against a MongoDB
    /// server, in a scratch database, and prints one PASS/FAIL line per check to stderr (so it shows
    /// in `adb logcat` on a phone or tablet). Returns the number of failed checks.
    int runShellSelfTest(const QString &hostAndPort);
}
