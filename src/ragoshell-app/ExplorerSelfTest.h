#pragma once

class QQmlApplicationEngine;

namespace RagoShell
{
    class QmlBackend;

    /// Headless self-test of the Connected Explorer TreeView (ConnectionsPane.qml + ServerTreeModel):
    /// double-click expand/collapse, disclosure indicator clicks, and async child replacement.
    /// Returns the number of failed checks (0 = pass).
    int runExplorerSelfTest(QQmlApplicationEngine *engine, QmlBackend *backend);
}
