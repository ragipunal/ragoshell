#pragma once

#include <QString>

namespace RagoShell
{
    /**
     * @brief End-to-end live integration test runner (--matrix-selftest=<spec>)
     *        exercising QmlBackend, ServerTreeModel, ServerMonitorController,
     *        ShellController, SshTunnelWorker, MongoWorker, MongoClient, and
     *        CollectionTransfer against the dev/mongo-matrix Docker environments.
     *
     * Supported specs:
     *   - noauth:<port>:<fixturesDir>
     *   - version:<port>:<expectedMajorMinor>:<fixturesDir>
     *   - mtls:<port>:<fixturesDir>
     *   - rs:<p1>,<p2>,<p3>:<fixturesDir>
     *   - sharded:<port>:<fixturesDir>
     *   - ssh:<sshPort>:<fixturesDir>
     *
     * Returns 0 when all checks pass, or the number of failed checks (> 0).
     */
    int runMatrixSelfTest(const QString &spec);
}
