#include "ragoshell-app/MatrixSelfTestInternal.h"

namespace RagoShell::MatrixTestDetail
{
        // -------------------------------------------------------------------
        // Suite 3: Strict Mutual TLS (requireTLS + mandatory client cert)
        // -------------------------------------------------------------------
        void runStrictMtlsSuite(Harness &h, int port, const QString &fixturesDir)
        {
            const std::string caPath = (fixturesDir + QStringLiteral("/ca.crt")).toStdString();
            const std::string clientPem = (fixturesDir + QStringLiteral("/client.pem")).toStdString();
            const std::string clientEncPem = (fixturesDir + QStringLiteral("/client-enc.pem")).toStdString();
            const std::string x509Subject = readTextTrimmed(fixturesDir + QStringLiteral("/client_subject.txt")).toStdString();

            // 1. Connecting without client PEM MUST fail against strict mTLS server
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");
                const QString u = h.saveAndConnect(cs, /*expectSuccess=*/false, 8000);
                h.check(!h.backend->isServerConnected(u) && !h.lastErrorMsg.isEmpty(),
                        QStringLiteral("mtls: connection without client certificate rejected"),
                        h.lastErrorMsg);
            }

            // 2. Mutual TLS + SCRAM-SHA-256 with unencrypted client.pem
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                cs.sslSettings()->setUsePemFile(true);
                cs.sslSettings()->setPemKeyFile(clientPem);
                addAuth(cs, "rago_sha256", "ragoSha256Pass!", "admin", "SCRAM-SHA-256");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), QStringLiteral("mtls: client.pem + SCRAM-SHA-256"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 3. Mutual TLS + MONGODB-X509 with passphrase-encrypted client-enc.pem
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                cs.sslSettings()->setUsePemFile(true);
                cs.sslSettings()->setPemKeyFile(clientEncPem);
                cs.sslSettings()->setPemPassPhrase("ragoPemPass2026");
                addAuth(cs, x509Subject, "", "$external", "MONGODB-X509");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), QStringLiteral("mtls: encrypted client-enc.pem + MONGODB-X509"), h.lastErrorMsg);
                if (!u.isEmpty()) {
                    ShellController *ctl = h.runShellScript(
                        u, QStringLiteral("appdb"), QStringLiteral("db.items.countDocuments({})"));
                    h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("3"),
                            QStringLiteral("mtls: shell query over mTLS + X.509"),
                            ctl ? ctl->textResult() : QStringLiteral("no shell"));
                    h.backend->disconnectServer(u, true);
                }
            }
        }

        // -------------------------------------------------------------------
        // Suite 4: 3-Node Replica Set 'rs0'
        // -------------------------------------------------------------------
        void runReplicaSetSuite(Harness &h, const QStringList &ports, const QString &fixturesDir)
        {
            if (ports.size() < 3) {
                h.check(false, QStringLiteral("rs: expected 3 ports"));
                return;
            }
            const std::string p1 = ("127.0.0.1:" + ports[0]).toStdString();
            const std::string p2 = ("127.0.0.1:" + ports[1]).toStdString();
            const std::string p3 = ("127.0.0.1:" + ports[2]).toStdString();
            const std::string caPath = (fixturesDir + QStringLiteral("/ca.crt")).toStdString();

            // 1. Seed ONLY Secondary (p2) AND leave Set Name empty -> auto-discovers 'rs0' & all 3 members!
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("RS Auto-Discover");
                cs.setReplicaSet(true);
                cs.replicaSetSettings()->setMembers({ p2 });
                cs.replicaSetSettings()->setSetNameUserEntered("");
                addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("rs: auto-discover setName & primary from single secondary seed"),
                        h.lastErrorMsg);
                if (!u.isEmpty()) {
                    const QModelIndex srvIdx = h.findServerIndex(u);
                    const QString badge = h.backend->serverTree()->data(srvIdx, ServerTreeModel::BadgeTextRole).toString();
                    const QString tip = h.backend->serverTree()->data(srvIdx, ServerTreeModel::ToolTipRole).toString();
                    const QModelIndex rsFolder = h.findChild(srvIdx, QStringLiteral("rsFolder"));
                    const int memberCount = rsFolder.isValid() ? h.backend->serverTree()->rowCount(rsFolder) : 0;
                    h.check(badge.startsWith(QLatin1String("RS")) && tip.contains(QLatin1String("rs0")) && memberCount == 3,
                            QStringLiteral("rs: ServerTreeModel shows RS badge, rs0 tooltip, and 3 member nodes"),
                            QStringLiteral("badge='%1', tip='%2', members=%3").arg(badge, tip).arg(memberCount));

                    // Open per-member shell on secondary p2
                    const int shellsBefore = h.backend->shells().size();
                    h.backend->openMemberShell(u, QString::fromStdString(p2),
                                               QStringLiteral("rs.secondaryOk(); db.getSiblingDB('appdb').items.countDocuments({})"), true);
                    Harness::waitUntil(12000, [&]() { return h.backend->shells().size() > shellsBefore; });
                    auto *mctl = h.backend->shells().size() > shellsBefore
                        ? qobject_cast<ShellController *>(h.backend->shells().last().value<QObject *>())
                        : nullptr;
                    if (mctl) {
                        Harness::waitUntil(12000, [&]() {
                            return !mctl->busy() && (mctl->hasTextOutput() || !mctl->errorText().isEmpty());
                        });
                    }
                    h.check(mctl && mctl->errorText().isEmpty() && mctl->textResult().trimmed() == QLatin1String("3"),
                            QStringLiteral("rs: openMemberShell queries secondary node directly"),
                            mctl ? (mctl->errorText().isEmpty() ? mctl->textResult() : mctl->errorText())
                                 : QStringLiteral("no shell"));
                    h.backend->disconnectServer(u, true);
                }
            }

            // 2. Replica Set + TLS + secondaryPreferred ReadPreference + localhost normalization + Monitor
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("RS TLS SecondaryPreferred");
                cs.setReplicaSet(true);
                cs.replicaSetSettings()->setMembers({ ("localhost:" + ports[0]).toStdString(), p2, p3 });
                cs.replicaSetSettings()->setSetNameUserEntered("rs0");
                cs.replicaSetSettings()->setReadPreference(ReplicaSetSettings::ReadPreference::SECONDARY_PREFERRED);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                addAuth(cs, "rago_sha256", "ragoSha256Pass!", "admin", "DEFAULT");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("rs: TLS + secondaryPreferred + localhost<->127.0.0.1 seed normalization"),
                        h.lastErrorMsg);
                if (!u.isEmpty()) {
                    ShellController *ctl = h.runShellScript(
                        u, QStringLiteral("appdb"), QStringLiteral("db.items.countDocuments({})"));
                    h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("3"),
                            QStringLiteral("rs: secondaryPreferred query over TLS"),
                            ctl ? ctl->textResult() : QStringLiteral("no shell"));

                    const int beforeMon = h.backend->monitors().size();
                    h.backend->openMonitor(u);
                    Harness::waitUntil(6000, [&]() { return h.backend->monitors().size() > beforeMon; });
                    auto *mon = h.backend->monitors().isEmpty()
                        ? nullptr
                        : qobject_cast<ServerMonitorController *>(h.backend->monitors().last().value<QObject *>());
                    if (mon)
                        Harness::waitUntil(8000, [&]() { return !mon->replSetName().isEmpty() || !mon->errorText().isEmpty(); });
                    h.check(mon && mon->replSetName() == QLatin1String("rs0"),
                            QStringLiteral("rs: ServerMonitorController detects replSetName='rs0'"),
                            mon ? (mon->replSetName() + " / " + mon->role()) : QStringLiteral("no monitor"));
                    h.backend->disconnectServer(u, true);
                }
            }

            // 3. Restricted non-admin user on Replica Set (exercises ismaster fallback when replSetGetStatus is Unauthorized)
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("RS Restricted Tenant");
                cs.setReplicaSet(true);
                cs.replicaSetSettings()->setMembers({ p1, p2, p3 });
                cs.replicaSetSettings()->setSetNameUserEntered("rs0");
                cs.setDefaultDatabase("tenant_a");

                auto *cred = new CredentialSettings();
                cred->setEnabled(true);
                cred->setUserName("rago_restricted");
                cred->setUserPassword("ragoRestrictedPass!");
                cred->setDatabaseName("tenant_a");
                cred->setMechanism("DEFAULT");
                cred->setUseManuallyVisibleDbs(true);
                cred->setManuallyVisibleDbs("tenant_a");
                cs.addCredential(cred);

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("rs: restricted single-DB user connects via {ismaster:1} fallback"),
                        h.lastErrorMsg);
                if (!u.isEmpty()) {
                    ShellController *ctl = h.runShellScript(
                        u, QStringLiteral("tenant_a"), QStringLiteral("db.orders.countDocuments({})"));
                    h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("2"),
                            QStringLiteral("rs: restricted user queries tenant_a.orders on Replica Set"),
                            ctl ? ctl->textResult() : QStringLiteral("no shell"));
                    h.backend->disconnectServer(u, true);
                }
            }
        }

        // -------------------------------------------------------------------
        // Suite 5: Sharded Cluster ('mongos')
        // -------------------------------------------------------------------
        void runShardedSuite(Harness &h, int port, const QString &fixturesDir)
        {
            const std::string caPath = (fixturesDir + QStringLiteral("/ca.crt")).toStdString();
            const std::string clientPem = (fixturesDir + QStringLiteral("/client.pem")).toStdString();
            const std::string x509Subject = readTextTrimmed(fixturesDir + QStringLiteral("/client_subject.txt")).toStdString();

            ConnectionSettings cs(false);
            cs.setConnectionName("Sharded mongos");
            cs.setServerHost("127.0.0.1");
            cs.setServerPort(port);
            cs.sslSettings()->enableSSL(true);
            cs.sslSettings()->setCaFile(caPath);
            addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");

            const QString u = h.saveAndConnect(cs, true, 20000);
            h.check(!u.isEmpty(), QStringLiteral("sharded: connect to mongos over TLS + SCRAM-SHA-256"), h.lastErrorMsg);
            if (u.isEmpty())
                return;

            h.check(h.backend->serverIsMongos(u),
                    QStringLiteral("sharded: Backend.serverIsMongos() == true"));

            const QModelIndex srvIdx = h.findServerIndex(u);
            const QString badge = h.backend->serverTree()->data(srvIdx, ServerTreeModel::BadgeTextRole).toString();
            const QString tip = h.backend->serverTree()->data(srvIdx, ServerTreeModel::ToolTipRole).toString();
            h.check(badge.startsWith(QLatin1String("MONGOS")) && tip.contains(QLatin1String("mongos")),
                    QStringLiteral("sharded: ServerTreeModel displays MONGOS badge & tooltip"),
                    badge);

            // ServerMonitorController on mongos
            const int beforeMon = h.backend->monitors().size();
            h.backend->openMonitor(u);
            Harness::waitUntil(6000, [&]() { return h.backend->monitors().size() > beforeMon; });
            auto *mon = h.backend->monitors().isEmpty()
                ? nullptr
                : qobject_cast<ServerMonitorController *>(h.backend->monitors().last().value<QObject *>());
            if (mon)
                Harness::waitUntil(8000, [&]() { return !mon->version().isEmpty() || !mon->errorText().isEmpty(); });
            h.check(mon && mon->role() == QLatin1String("MONGOS"),
                    QStringLiteral("sharded: ServerMonitorController role() == 'MONGOS'"),
                    mon ? mon->role() : QStringLiteral("no monitor"));

            // User management on mongos (create & drop user on sharded 'appdb')
            h.lastErrorMsg.clear();
            h.backend->createOrUpdateUser(
                u, QStringLiteral("appdb"),
                QStringLiteral("mongos_app_user"), QStringLiteral("MongosPass2026!"),
                QStringLiteral("appdb"), QStringList{ QStringLiteral("readWrite") },
                /*isUpdate=*/false, QStringList{ QStringLiteral("SCRAM-SHA-256") });
            Harness::pumpMs(500);
            h.check(h.lastErrorMsg.isEmpty(),
                    QStringLiteral("sharded: createUser on appdb through mongos"),
                    h.lastErrorMsg);
            h.backend->dropUser(u, QStringLiteral("appdb"), QStringLiteral("mongos_app_user"));
            Harness::pumpMs(300);

            // Shell query & listShards through mongos
            ShellController *ctl = h.runShellScript(
                u, QStringLiteral("admin"),
                QStringLiteral("db.runCommand({listShards: 1}).shards.length"));
            h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("1"),
                    QStringLiteral("sharded: listShards via ShellController returns 1 shard"),
                    ctl ? ctl->textResult() : QStringLiteral("no shell"));

            h.backend->disconnectServer(u, true);

            // MONGODB-X509 against mongos
            {
                ConnectionSettings xcs(false);
                xcs.setServerHost("127.0.0.1");
                xcs.setServerPort(port);
                xcs.sslSettings()->enableSSL(true);
                xcs.sslSettings()->setCaFile(caPath);
                xcs.sslSettings()->setUsePemFile(true);
                xcs.sslSettings()->setPemKeyFile(clientPem);
                addAuth(xcs, x509Subject, "", "$external", "MONGODB-X509");
                const QString xu = h.saveAndConnect(xcs, true, 15000);
                h.check(!xu.isEmpty() && h.backend->serverIsMongos(xu),
                        QStringLiteral("sharded: MONGODB-X509 auth against mongos"),
                        h.lastErrorMsg);
                if (!xu.isEmpty())
                    h.backend->disconnectServer(xu, true);
            }
        }

        // -------------------------------------------------------------------
        // Suite 6: SSH Tunnel Combinations
        // -------------------------------------------------------------------
        void runSshSuite(Harness &h, int sshPort, const QString &fixturesDir)
        {
            const std::string sshTargetHost = qEnvironmentVariable(
                "RAGO_SSH_TARGET_HOST", QStringLiteral("mongo-70")).toStdString();
            const std::string caPath = (fixturesDir + QStringLiteral("/ca.crt")).toStdString();
            const std::string clientEncPem = (fixturesDir + QStringLiteral("/client-enc.pem")).toStdString();
            const std::string x509Subject = readTextTrimmed(fixturesDir + QStringLiteral("/client_subject.txt")).toStdString();
            const std::string edKey = (fixturesDir + QStringLiteral("/ssh/id_ed25519")).toStdString();
            const std::string edEncKey = (fixturesDir + QStringLiteral("/ssh/id_ed25519_enc")).toStdString();
            const std::string rsaKey = (fixturesDir + QStringLiteral("/ssh/id_rsa")).toStdString();

            auto setupSshCommon = [&](ConnectionSettings &cs, const std::string &method) {
                cs.sshSettings()->setEnabled(true);
                cs.sshSettings()->setHost("127.0.0.1");
                cs.sshSettings()->setPort(sshPort);
                cs.sshSettings()->setUserName("rago_ssh_user");
                cs.sshSettings()->setAuthMethod(method);
            };

            // 1. SSH Password Auth -> mongo-noauth:27017 (verifies TOFU host key trust + password SSH + No-Auth Mongo)
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("SSH Password + NoAuth");
                cs.setServerHost("mongo-noauth");
                cs.setServerPort(27017);
                setupSshCommon(cs, "password");
                cs.sshSettings()->setUserPassword("ragoSshSecret2026");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty() && h.hostKeysTrusted >= 1,
                        QStringLiteral("ssh: Password auth + TOFU hostKeyRequested -> mongo-noauth"),
                        h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 2. SSH Unencrypted Ed25519 Key -> mongo-70:27017 + SCRAM-SHA-256 + Shell + Transfer
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("SSH Ed25519 + SCRAM-SHA-256");
                cs.setServerHost(sshTargetHost);
                cs.setServerPort(27017);
                setupSshCommon(cs, "publickey");
                cs.sshSettings()->setPrivateKeyFile(edKey);
                addAuth(cs, "rago_sha256", "ragoSha256Pass!", "admin", "DEFAULT");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("ssh: Ed25519 key -> mongo-70 + DEFAULT (SCRAM-SHA-256)"),
                        h.lastErrorMsg);
                if (!u.isEmpty()) {
                    ShellController *ctl = h.runShellScript(
                        u, QStringLiteral("appdb"), QStringLiteral("db.items.countDocuments({})"));
                    h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("3"),
                            QStringLiteral("ssh: ShellController query over SSH tunnel"),
                            ctl ? ctl->textResult() : QStringLiteral("no shell"));
                    h.backend->disconnectServer(u, true);
                }
            }

            // 3. SSH Passphrase-Protected Ed25519 Key -> mongo-70:27017 + SCRAM-SHA-1
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("SSH Encrypted Ed25519 + SCRAM-SHA-1");
                cs.setServerHost(sshTargetHost);
                cs.setServerPort(27017);
                setupSshCommon(cs, "publickey");
                cs.sshSettings()->setPrivateKeyFile(edEncKey);
                cs.sshSettings()->setPassphrase("ragoSshPass2026");
                addAuth(cs, "rago_sha1", "ragoSha1Pass!", "admin", "SCRAM-SHA-1");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("ssh: Passphrase-protected Ed25519 key + SCRAM-SHA-1"),
                        h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 4. SSH RSA Key + Server TLS (CA-verified over local SSH tunnel port!)
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("SSH RSA + TLS");
                cs.setServerHost(sshTargetHost);
                cs.setServerPort(27017);
                setupSshCommon(cs, "publickey");
                cs.sshSettings()->setPrivateKeyFile(rsaKey);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("ssh: RSA key + CA-verified TLS over SSH tunnel"),
                        h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 5. SSH + Strict mTLS + Passphrase-Encrypted Client PEM + MONGODB-X509
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("SSH + Strict mTLS + X.509");
                cs.setServerHost("mongo-mtls");
                cs.setServerPort(27017);
                setupSshCommon(cs, "publickey");
                cs.sshSettings()->setPrivateKeyFile(edKey);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                cs.sslSettings()->setUsePemFile(true);
                cs.sslSettings()->setPemKeyFile(clientEncPem);
                cs.sslSettings()->setPemPassPhrase("ragoPemPass2026");
                addAuth(cs, x509Subject, "", "$external", "MONGODB-X509");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty(),
                        QStringLiteral("ssh: Tunnel + Strict mTLS + encrypted PEM + MONGODB-X509"),
                        h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 6. SSH + Sharded Cluster (mongos:27017)
            {
                ConnectionSettings cs(false);
                cs.setConnectionName("SSH + Sharded mongos");
                cs.setServerHost("mongos");
                cs.setServerPort(27017);
                setupSshCommon(cs, "password");
                cs.sshSettings()->setUserPassword("ragoSshSecret2026");
                addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");

                const QString u = h.saveAndConnect(cs, true, 20000);
                h.check(!u.isEmpty() && h.backend->serverIsMongos(u),
                        QStringLiteral("ssh: Password SSH tunnel -> Sharded Cluster mongos"),
                        h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }
        }
}
