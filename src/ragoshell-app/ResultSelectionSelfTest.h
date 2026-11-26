#pragma once

class QQmlApplicationEngine;

namespace RagoShell
{
    /// Automated headless verification for multi-document selection (Ctrl+Click,
    /// Shift+Click, Ctrl+Shift+Click, right-click preservation, Shift+Arrow, Ctrl+A)
    /// across ShellResultTreeView and ShellResultTableView (--selection-selftest).
    int runResultSelectionSelfTest(QQmlApplicationEngine *engine);
}
