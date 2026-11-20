#include "ragoshell-app/MatrixSelfTestInternal.h"

namespace RagoShell
{
    using namespace MatrixTestDetail;

    namespace
    {
        // -------------------------------------------------------------------
        // Suite 1: Standalone No-Auth
        // -------------------------------------------------------------------
        void runNoAuthSuite(Harness &h, int port)
        {
            ConnectionSettings cs(false);
            cs.setConnectionName("NoAuth Test");
            cs.setServerHost("127.0.0.1");
            cs.setServerPort(port);

            const QString uuid = h.saveAndConnect(cs);
            h.check(!uuid.isEmpty(), QStringLiteral("noauth: connect without credentials"), h.lastErrorMsg);
            if (uuid.isEmpty())
                return;

            h.check(h.backend->serverVersion(uuid) >= 4.0,
                    QStringLiteral("noauth: version detected"),
                    h.backend->serverVersionString(uuid));

            ShellController *ctl = h.runShellScript(
                uuid, QStringLiteral("rago_noauth_db"),
                QStringLiteral("db.items.deleteMany({}); db.items.insertMany([{_id:1, v:'ok'}, {_id:2, v:'ok2'}]); db.items.countDocuments({})"));
            h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("2"),
                    QStringLiteral("noauth: shell CRUD & countDocuments"),
                    ctl ? (ctl->errorText().isEmpty() ? ctl->textResult() : ctl->errorText()) : QStringLiteral("no shell"));

            h.backend->dropDatabase(uuid, QStringLiteral("rago_noauth_db"));
            Harness::pumpMs(300);
            h.backend->disconnectServer(uuid, true);
        }

        // -------------------------------------------------------------------
        // Suite 2: Full Per-Version Matrix (4.0, 4.2, 4.4, 5.0, 6.0, 7.0, 8.0)
        // -------------------------------------------------------------------
        void runVersionSuite(Harness &h, int port, const QString &expectedVerPrefix, const QString &fixturesDir)
        {
            const QString tag = QStringLiteral("v%1").arg(expectedVerPrefix);
            const double expectedVer = expectedVerPrefix.toDouble();
            const std::string caPath = (fixturesDir + QStringLiteral("/ca.crt")).toStdString();
            const std::string clientPem = (fixturesDir + QStringLiteral("/client.pem")).toStdString();
            const std::string clientEncPem = (fixturesDir + QStringLiteral("/client-enc.pem")).toStdString();
            const std::string x509Subject = readTextTrimmed(fixturesDir + QStringLiteral("/client_subject.txt")).toStdString();

            // 1. DEFAULT mechanism auto-negotiating SCRAM-SHA-256-only user
            QString adminUuid;
            {
                ConnectionSettings cs(false);
                cs.setConnectionName(("Root-" + tag).toStdString());
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                addAuth(cs, "rago_sha256", "ragoSha256Pass!", "admin", "DEFAULT");
                adminUuid = h.saveAndConnect(cs);
                h.check(!adminUuid.isEmpty(),
                        tag + QStringLiteral(": DEFAULT auto-negotiates SCRAM-SHA-256-only user"),
                        h.lastErrorMsg);
            }
            if (adminUuid.isEmpty())
                return;

            const double detectedVer = h.backend->serverVersion(adminUuid);
            const QString detectedVerStr = h.backend->serverVersionString(adminUuid);
            h.check(std::abs(detectedVer - expectedVer) < 0.001
                        && detectedVerStr.startsWith(expectedVerPrefix),
                    tag + QStringLiteral(": exact serverVersion() & serverVersionString()"),
                    QStringLiteral("got %1 ('%2')").arg(detectedVer, 0, 'f', 2).arg(detectedVerStr));
            h.check(!h.backend->serverIsMongos(adminUuid),
                    tag + QStringLiteral(": standalone serverIsMongos() == false"));

            // 2. Explicit SCRAM-SHA-1 user
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                addAuth(cs, "rago_sha1", "ragoSha1Pass!", "admin", "SCRAM-SHA-1");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": explicit SCRAM-SHA-1 auth"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 3. Mismatched mechanism (SCRAM-SHA-1 on SCRAM-SHA-256-only user) fails cleanly
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                addAuth(cs, "rago_sha256", "ragoSha256Pass!", "admin", "SCRAM-SHA-1");
                const QString u = h.saveAndConnect(cs, /*expectSuccess=*/false, 8000);
                h.check(!h.backend->isServerConnected(u) && !h.lastErrorMsg.isEmpty(),
                        tag + QStringLiteral(": wrong SCRAM mechanism rejected cleanly"),
                        h.lastErrorMsg);
            }

            // 4. Custom non-admin authSource ('appdb')
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.setDefaultDatabase("appdb");
                addAuth(cs, "rago_app_owner", "ragoAppPass!", "appdb", "DEFAULT");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": custom authSource='appdb' dbOwner"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 5. Restricted single-DB user (no listDatabases, no serverStatus) + ManuallyVisibleDbs
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.setDefaultDatabase("tenant_a");
                auto *cred = new CredentialSettings();
                cred->setEnabled(true);
                cred->setUserName("rago_restricted");
                cred->setUserPassword("ragoRestrictedPass!");
                cred->setDatabaseName("tenant_a");
                cred->setMechanism("DEFAULT");
                cred->setUseManuallyVisibleDbs(true);
                cred->setManuallyVisibleDbs("  tenant_a , ,  ");
                cs.addCredential(cred);

                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": restricted user connects without serverStatus/listDatabases"), h.lastErrorMsg);
                if (!u.isEmpty()) {
                    const QStringList dbs = h.listedDatabaseNames(u);
                    h.check(dbs == QStringList{ QStringLiteral("tenant_a") },
                            tag + QStringLiteral(": ManuallyVisibleDbs trims whitespace & hides unauthorized DBs"),
                            dbs.join(QLatin1Char(',')));
                    ShellController *ctl = h.runShellScript(
                        u, QStringLiteral("tenant_a"), QStringLiteral("db.orders.countDocuments({})"));
                    h.check(ctl && ctl->errorText().isEmpty() && ctl->textResult().trimmed() == QLatin1String("2"),
                            tag + QStringLiteral(": restricted user queries tenant_a.orders"),
                            ctl ? ctl->textResult() : QStringLiteral("no shell"));
                    h.backend->disconnectServer(u, true);
                }
            }

            // 6. Server TLS (CA-verified)
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                cs.sslSettings()->setAllowInvalidCertificates(false);
                addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": TLS with CA verification"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 7. Self-signed / allowInvalidCertificates TLS (without CA file)
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setAllowInvalidCertificates(true);
                cs.sslSettings()->setAllowInvalidHostnames(true);
                addAuth(cs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": TLS with allowInvalidCertificates"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 8. Mutual TLS + MONGODB-X509 (unencrypted client.pem, explicit subject)
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                cs.sslSettings()->setUsePemFile(true);
                cs.sslSettings()->setPemKeyFile(clientPem);
                addAuth(cs, x509Subject, "", "$external", "MONGODB-X509");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": MONGODB-X509 auth with unencrypted client.pem"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 9. Mutual TLS + MONGODB-X509 (passphrase-encrypted client-enc.pem, empty username)
            {
                ConnectionSettings cs(false);
                cs.setServerHost("127.0.0.1");
                cs.setServerPort(port);
                cs.sslSettings()->enableSSL(true);
                cs.sslSettings()->setCaFile(caPath);
                cs.sslSettings()->setUsePemFile(true);
                cs.sslSettings()->setPemKeyFile(clientEncPem);
                cs.sslSettings()->setPemPassPhrase("ragoPemPass2026");
                addAuth(cs, "", "", "$external", "MONGODB-X509");
                const QString u = h.saveAndConnect(cs);
                h.check(!u.isEmpty(), tag + QStringLiteral(": MONGODB-X509 auth with encrypted client-enc.pem & auto-subject"), h.lastErrorMsg);
                if (!u.isEmpty())
                    h.backend->disconnectServer(u, true);
            }

            // 10. In-App User Management (SCRAM-SHA-256 mechanisms, updateUser, cross-DB @admin roles, $external X.509)
            {
                // Ensure adminUuid is upgraded to rago_root for $external user management too
                h.backend->disconnectServer(adminUuid, true);
                ConnectionSettings rootCs(false);
                rootCs.setServerHost("127.0.0.1");
                rootCs.setServerPort(port);
                addAuth(rootCs, "rago_root", "ragoRootSecret2026", "admin", "DEFAULT");
                adminUuid = h.saveAndConnect(rootCs);
                h.check(!adminUuid.isEmpty(), tag + QStringLiteral(": connect as rago_root"), h.lastErrorMsg);
                if (adminUuid.isEmpty())
                    return;

                // 10a. Create SCRAM-SHA-256-only user in 'appdb'
                const bool created = h.backend->createOrUpdateUser(
                    adminUuid, QStringLiteral("appdb"),
                    QStringLiteral("ui_sha256_user"), QStringLiteral("UiPass256!"),
                    QStringLiteral("appdb"), QStringList{ QStringLiteral("readWrite") },
                    /*isUpdate=*/false, QStringList{ QStringLiteral("SCRAM-SHA-256") });
                Harness::pumpMs(400);
                h.check(created && h.lastErrorMsg.isEmpty(),
                        tag + QStringLiteral(": createOrUpdateUser creates SCRAM-SHA-256 user in appdb"),
                        h.lastErrorMsg);

                // Expand appdb: verify direct Compass-style tree (no collectionsFolder / usersFolder)
                // and inspect user mechanisms + roles via requestDatabaseUsers / cachedDatabaseUsers.
                const QModelIndex appDbIdx = h.findDatabaseIndex(adminUuid, QStringLiteral("appdb"));
                h.expandAndWait(appDbIdx);
                h.check(!h.findChild(appDbIdx, QStringLiteral("collectionsFolder")).isValid()
                            && !h.findChild(appDbIdx, QStringLiteral("usersFolder")).isValid(),
                        tag + QStringLiteral(": Database expands directly without Collections/Users wrapper folders"));

                auto findCachedUser = [&](const QString &dbName, const QString &userName) -> QVariantMap {
                    const QVariantList list = h.backend->cachedDatabaseUsers(adminUuid, dbName);
                    for (const QVariant &v : list) {
                        const QVariantMap m = v.toMap();
                        if (m.value(QStringLiteral("name")).toString() == userName)
                            return m;
                    }
                    return {};
                };

                h.backend->requestDatabaseUsers(adminUuid, QStringLiteral("appdb"));
                Harness::waitUntil(5000, [&]() {
                    return !findCachedUser(QStringLiteral("appdb"), QStringLiteral("ui_sha256_user")).isEmpty();
                });
                const QVariantMap uiMeta = findCachedUser(QStringLiteral("appdb"), QStringLiteral("ui_sha256_user"));
                h.check(!uiMeta.isEmpty()
                            && uiMeta.value(QStringLiteral("mechanisms")).toStringList() == QStringList{ QStringLiteral("SCRAM-SHA-256") },
                        tag + QStringLiteral(": Users manager cache exposes SCRAM-SHA-256 mechanism metadata"),
                        uiMeta.value(QStringLiteral("mechanisms")).toStringList().join(QLatin1Char(',')));

                // Verify live login as newly created ui_sha256_user
                {
                    ConnectionSettings ucs(false);
                    ucs.setServerHost("127.0.0.1");
                    ucs.setServerPort(port);
                    ucs.setDefaultDatabase("appdb");
                    addAuth(ucs, "ui_sha256_user", "UiPass256!", "appdb", "DEFAULT");
                    const QString uu = h.saveAndConnect(ucs);
                    h.check(!uu.isEmpty(), tag + QStringLiteral(": newly created ui_sha256_user authenticates live"), h.lastErrorMsg);
                    if (!uu.isEmpty())
                        h.backend->disconnectServer(uu, true);
                }

                // 10b. Update ui_sha256_user roles WITHOUT changing password (empty password)
                h.lastErrorMsg.clear();
                h.backend->createOrUpdateUser(
                    adminUuid, QStringLiteral("appdb"),
                    QStringLiteral("ui_sha256_user"), QString(),
                    QStringLiteral("appdb"), QStringList{ QStringLiteral("readWrite"), QStringLiteral("dbAdmin") },
                    /*isUpdate=*/true, QStringList{ QStringLiteral("SCRAM-SHA-256") });
                Harness::pumpMs(400);
                h.backend->refreshUsers(adminUuid, QStringLiteral("appdb"));
                Harness::waitUntil(5000, [&]() {
                    const QStringList r = findCachedUser(QStringLiteral("appdb"), QStringLiteral("ui_sha256_user"))
                                              .value(QStringLiteral("roles")).toStringList();
                    return r.contains(QStringLiteral("dbAdmin")) && r.contains(QStringLiteral("readWrite"));
                });
                const QStringList updatedRoles = findCachedUser(QStringLiteral("appdb"), QStringLiteral("ui_sha256_user"))
                                                     .value(QStringLiteral("roles")).toStringList();
                h.check(h.lastErrorMsg.isEmpty() && updatedRoles.contains(QStringLiteral("dbAdmin")),
                        tag + QStringLiteral(": updateUser updates roles while preserving existing password"),
                        h.lastErrorMsg);

                // 10c. Create cross-DB + @admin role user in 'admin'
                h.lastErrorMsg.clear();
                h.backend->createOrUpdateUser(
                    adminUuid, QStringLiteral("admin"),
                    QStringLiteral("ui_cross_ops"), QStringLiteral("CrossOps2026!"),
                    QStringLiteral("appdb"),
                    QStringList{ QStringLiteral("readWrite"), QStringLiteral("clusterMonitor@admin"), QStringLiteral("read@tenant_a") },
                    /*isUpdate=*/false, QStringList{ QStringLiteral("SCRAM-SHA-256") });
                Harness::pumpMs(400);
                h.backend->refreshUsers(adminUuid, QStringLiteral("admin"));
                Harness::waitUntil(5000, [&]() {
                    return !findCachedUser(QStringLiteral("admin"), QStringLiteral("ui_cross_ops")).isEmpty();
                });
                const QVariantMap crossMeta = findCachedUser(QStringLiteral("admin"), QStringLiteral("ui_cross_ops"));
                const QStringList crossRoles = crossMeta.value(QStringLiteral("roles")).toStringList();
                h.check(!crossMeta.isEmpty()
                            && crossRoles.contains(QStringLiteral("clusterMonitor"))
                            && crossRoles.contains(QStringLiteral("readWrite@appdb"))
                            && crossRoles.contains(QStringLiteral("read@tenant_a")),
                        tag + QStringLiteral(": admin user preserves cross-DB role@db specs"),
                        crossRoles.join(QLatin1Char(',')));

                // 10d. Create passwordless X.509 user in '$external' & drop users cleanly
                h.lastErrorMsg.clear();
                const QString x509Principal = QStringLiteral("CN=ui-x509-test,OU=Matrix,O=RagoShellClient,C=US");
                h.backend->createOrUpdateUser(
                    adminUuid, QStringLiteral("$external"),
                    x509Principal, QString(),
                    QStringLiteral("appdb"),
                    QStringList{ QStringLiteral("read"), QStringLiteral("clusterMonitor@admin") },
                    /*isUpdate=*/false);
                Harness::pumpMs(400);
                h.backend->requestDatabaseUsers(adminUuid, QStringLiteral("$external"));
                Harness::waitUntil(5000, [&]() {
                    return !findCachedUser(QStringLiteral("$external"), x509Principal).isEmpty();
                });
                const QVariantMap extUserMeta = findCachedUser(QStringLiteral("$external"), x509Principal);
                h.check(!extUserMeta.isEmpty()
                            && !h.findDatabaseIndex(adminUuid, QStringLiteral("$external")).isValid()
                            && h.lastErrorMsg.isEmpty(),
                        tag + QStringLiteral(": passwordless X.509 user created & listed in $external without polluting DB tree"),
                        h.lastErrorMsg);

                h.backend->dropUser(adminUuid, QStringLiteral("appdb"), QStringLiteral("ui_sha256_user"));
                h.backend->dropUser(adminUuid, QStringLiteral("admin"), QStringLiteral("ui_cross_ops"));
                h.backend->dropUser(adminUuid, QStringLiteral("$external"), x509Principal);
                Harness::pumpMs(400);
            }

            // 11. Version-Adaptive Collections (Capped, View, Validator/Collation, 5.0+ TimeSeries, 6.0+ Clustered)
            {
                h.lastErrorMsg.clear();
                h.backend->createCollectionAdvanced(
                    adminUuid, QStringLiteral("appdb"), QStringLiteral("capped_logs"),
                    /*capped=*/true, 65536, 100, QString(), QStringLiteral("strict"), QStringLiteral("error"),
                    QString(), QString());
                h.backend->createCollectionAdvanced(
                    adminUuid, QStringLiteral("appdb"), QStringLiteral("featured_items_view"),
                    /*capped=*/false, 0, 0, QString(), QStringLiteral("strict"), QStringLiteral("error"),
                    QString(), QStringLiteral("{\"viewOn\":\"items\",\"pipeline\":[{\"$match\":{\"qty\":{\"$gte\":40}}}]}"));
                h.backend->createCollectionAdvanced(
                    adminUuid, QStringLiteral("appdb"), QStringLiteral("validated_users"),
                    /*capped=*/false, 0, 0,
                    QStringLiteral("{\"$jsonSchema\":{\"bsonType\":\"object\",\"required\":[\"email\"]}}"),
                    QStringLiteral("strict"), QStringLiteral("error"),
                    QString(), QStringLiteral("{\"collation\":{\"locale\":\"tr\",\"strength\":2}}"));
                Harness::pumpMs(500);
                h.check(h.lastErrorMsg.isEmpty(),
                        tag + QStringLiteral(": createCollectionAdvanced (capped + view + $jsonSchema + collation)"),
                        h.lastErrorMsg);

                if (detectedVer >= 5.0) {
                    h.lastErrorMsg.clear();
                    h.backend->createCollectionAdvanced(
                        adminUuid, QStringLiteral("appdb"), QStringLiteral("sensor_ts"),
                        false, 0, 0, QString(), QStringLiteral("strict"), QStringLiteral("error"),
                        QString(),
                        QStringLiteral("{\"timeseries\":{\"timeField\":\"ts\",\"metaField\":\"sensor\",\"granularity\":\"seconds\"}}"));
                    Harness::pumpMs(400);
                    h.check(h.lastErrorMsg.isEmpty(),
                            tag + QStringLiteral(": Time Series collection created (5.0+)"),
                            h.lastErrorMsg);
                }

                if (detectedVer >= 6.0) {
                    h.lastErrorMsg.clear();
                    h.backend->createCollectionAdvanced(
                        adminUuid, QStringLiteral("appdb"), QStringLiteral("clustered_events"),
                        false, 0, 0, QString(), QStringLiteral("strict"), QStringLiteral("error"),
                        QString(),
                        QStringLiteral("{\"clusteredIndex\":{\"key\":{\"_id\":1},\"unique\":true},\"changeStreamPreAndPostImages\":{\"enabled\":true}}"));
                    Harness::pumpMs(400);
                    h.check(h.lastErrorMsg.isEmpty(),
                            tag + QStringLiteral(": Clustered + Pre/Post-Images collection created (6.0+)"),
                            h.lastErrorMsg);
                }
            }

            // 12. Version-Adaptive Indexes (Partial + Collation, 4.2+ Wildcard, 4.4+ Hidden collMod toggle)
            {
                h.lastErrorMsg.clear();
                QVariantMap idxSpec;
                idxSpec.insert(QStringLiteral("name"), QStringLiteral("sku_partial_idx"));
                idxSpec.insert(QStringLiteral("keys"), QStringLiteral("{\n  \"sku\": 1\n}"));
                idxSpec.insert(QStringLiteral("unique"), false);
                idxSpec.insert(QStringLiteral("sparse"), false);
                idxSpec.insert(QStringLiteral("background"), detectedVer < 4.2);
                idxSpec.insert(QStringLiteral("hidden"), false);
                idxSpec.insert(QStringLiteral("ttl"), -1);
                idxSpec.insert(QStringLiteral("partialFilterExpression"), QStringLiteral("{\"qty\":{\"$gt\":0}}"));
                idxSpec.insert(QStringLiteral("collation"), QStringLiteral("{\"locale\":\"en\",\"strength\":2}"));
                h.backend->saveIndex(adminUuid, QStringLiteral("appdb"), QStringLiteral("items"), QString(), idxSpec);
                Harness::pumpMs(500);
                h.check(h.lastErrorMsg.isEmpty(),
                        tag + QStringLiteral(": create index (partialFilter + collation)"),
                        h.lastErrorMsg);

                if (detectedVer >= 4.2) {
                    h.lastErrorMsg.clear();
                    QVariantMap wcSpec;
                    wcSpec.insert(QStringLiteral("name"), QStringLiteral("wildcard_tags_idx"));
                    wcSpec.insert(QStringLiteral("keys"), QStringLiteral("{\"$**\": 1}"));
                    wcSpec.insert(QStringLiteral("wildcardProjection"), QStringLiteral("{\"tags\": 1}"));
                    wcSpec.insert(QStringLiteral("ttl"), -1);
                    h.backend->saveIndex(adminUuid, QStringLiteral("appdb"), QStringLiteral("items"), QString(), wcSpec);
                    Harness::pumpMs(500);
                    h.check(h.lastErrorMsg.isEmpty(),
                            tag + QStringLiteral(": Wildcard index ($** + wildcardProjection, 4.2+)"),
                            h.lastErrorMsg);
                }

                if (detectedVer >= 4.4) {
                    const QModelIndex appDbIdx = h.findDatabaseIndex(adminUuid, QStringLiteral("appdb"));
                    h.expandAndWait(appDbIdx);
                    const QModelIndex viewColl = h.findChild(appDbIdx, QStringLiteral("collection"), QStringLiteral("featured_items_view"));
                    const QString viewBadge = h.backend->serverTree()->data(viewColl, ServerTreeModel::BadgeTextRole).toString();
                    h.check(viewColl.isValid()
                                && viewBadge == QLatin1String("VIEW")
                                && !h.findChild(viewColl, QStringLiteral("indexesFolder")).isValid(),
                            tag + QStringLiteral(": Read-only view displays VIEW badge without IndexesFolder"),
                            viewBadge);

                    const QModelIndex itemsColl = h.findChild(appDbIdx, QStringLiteral("collection"), QStringLiteral("items"));
                    h.expandAndWait(itemsColl);
                    const QModelIndex idxFolder = h.findChild(itemsColl, QStringLiteral("indexesFolder"));
                    h.expandAndWait(idxFolder);
                    const QModelIndex skuIdx = h.findChild(idxFolder, QStringLiteral("index"), QStringLiteral("sku_partial_idx"));
                    const QVariantMap oldMeta = h.backend->serverTree()->data(skuIdx, ServerTreeModel::IndexMetaRole).toMap();

                    h.lastErrorMsg.clear();
                    h.backend->setIndexHidden(adminUuid, QStringLiteral("appdb"), QStringLiteral("items"), oldMeta, true);
                    Harness::waitUntil(5000, [&]() {
                        const QModelIndex cur = h.findChild(idxFolder, QStringLiteral("index"), QStringLiteral("sku_partial_idx"));
                        return h.backend->serverTree()->data(cur, ServerTreeModel::IndexMetaRole)
                                   .toMap().value(QStringLiteral("hidden")).toBool();
                    });
                    const QModelIndex afterHide = h.findChild(idxFolder, QStringLiteral("index"), QStringLiteral("sku_partial_idx"));
                    const bool isNowHidden = h.backend->serverTree()->data(afterHide, ServerTreeModel::IndexMetaRole)
                                                 .toMap().value(QStringLiteral("hidden")).toBool();
                    h.check(isNowHidden && h.lastErrorMsg.isEmpty(),
                            tag + QStringLiteral(": Hidden index collMod fast-path toggle (4.4+)"),
                            h.lastErrorMsg);
                }

                h.backend->dropIndex(adminUuid, QStringLiteral("appdb"), QStringLiteral("items"), QStringLiteral("sku_partial_idx"));
                if (detectedVer >= 4.2)
                    h.backend->dropIndex(adminUuid, QStringLiteral("appdb"), QStringLiteral("items"), QStringLiteral("wildcard_tags_idx"));
                Harness::pumpMs(300);
            }

            // 13. Stored Functions (db.system.js — hidden by default, shown with DEPRECATED badge when non-empty)
            {
                const QModelIndex appDbIdx = h.findDatabaseIndex(adminUuid, QStringLiteral("appdb"));
                h.expandAndWait(appDbIdx);
                h.check(!h.findChild(appDbIdx, QStringLiteral("functionsFolder")).isValid(),
                        tag + QStringLiteral(": Functions folder hidden when db.system.js is absent/empty"));

                h.lastErrorMsg.clear();
                h.backend->createOrUpdateFunction(
                    adminUuid, QStringLiteral("appdb"), QString(),
                    QStringLiteral("ragoAdd"), QStringLiteral("function(a, b) { return a + b; }"));
                Harness::waitUntil(5000, [&]() {
                    const QModelIndex fnFolder = h.findChild(appDbIdx, QStringLiteral("functionsFolder"));
                    return fnFolder.isValid()
                        && h.findChild(fnFolder, QStringLiteral("function"), QStringLiteral("ragoAdd")).isValid();
                });
                const QModelIndex fnFolder = h.findChild(appDbIdx, QStringLiteral("functionsFolder"));
                const QString fnBadge = h.backend->serverTree()->data(fnFolder, ServerTreeModel::BadgeTextRole).toString();
                const bool fnFound = h.findChild(fnFolder, QStringLiteral("function"), QStringLiteral("ragoAdd")).isValid();
                h.check(fnFound && fnBadge == QLatin1String("DEPRECATED") && h.lastErrorMsg.isEmpty(),
                        tag + QStringLiteral(": stored JS function dynamically reveals Functions (DEPRECATED) folder"),
                        h.lastErrorMsg);
                h.backend->dropFunction(adminUuid, QStringLiteral("appdb"), QStringLiteral("ragoAdd"));
                Harness::waitUntil(5000, [&]() {
                    return !h.findChild(appDbIdx, QStringLiteral("functionsFolder")).isValid();
                });
                h.check(!h.findChild(appDbIdx, QStringLiteral("functionsFolder")).isValid(),
                        tag + QStringLiteral(": dropping last stored JS function hides Functions folder again"));
            }

            // 14. Server Monitor & Collection Export/Import
            {
                const int beforeMon = h.backend->monitors().size();
                h.backend->openMonitor(adminUuid);
                Harness::waitUntil(6000, [&]() { return h.backend->monitors().size() > beforeMon; });
                auto *mon = h.backend->monitors().isEmpty()
                    ? nullptr
                    : qobject_cast<ServerMonitorController *>(h.backend->monitors().last().value<QObject *>());
                if (mon) {
                    Harness::waitUntil(8000, [&]() { return !mon->version().isEmpty() || !mon->errorText().isEmpty(); });
                }
                h.check(mon && mon->errorText().isEmpty() && mon->version().startsWith(expectedVerPrefix),
                        tag + QStringLiteral(": ServerMonitorController live serverStatus & currentOp"),
                        mon ? (mon->errorText().isEmpty() ? mon->version() : mon->errorText()) : QStringLiteral("no monitor"));

                const QString exportPath = h.tempConfigDir.filePath(tag + QStringLiteral("_items.ndjson"));
                bool transferDone = false;
                bool transferOk = false;
                int transferCount = 0;
                QString transferMsg;
                QMetaObject::Connection conn = QObject::connect(
                    h.backend.get(), &QmlBackend::transferFinished, h.backend.get(),
                    [&](bool ok, int count, const QString &msg) {
                        transferDone = true;
                        transferOk = ok;
                        transferCount = count;
                        transferMsg = msg;
                    });
                h.backend->startExportCollection(adminUuid, QStringLiteral("appdb"), QStringLiteral("items"), exportPath, 1);
                Harness::waitUntil(10000, [&]() { return transferDone; });
                h.check(transferOk && transferCount == 3,
                        tag + QStringLiteral(": NDJSON export via QmlBackend"),
                        transferMsg);

                transferDone = false;
                transferOk = false;
                transferCount = 0;
                h.backend->startImportCollection(adminUuid, QStringLiteral("appdb"), QStringLiteral("items_roundtrip"), exportPath, 1, true, true);
                Harness::waitUntil(10000, [&]() { return transferDone; });
                QObject::disconnect(conn);
                h.check(transferOk && transferCount == 3,
                        tag + QStringLiteral(": NDJSON import via QmlBackend"),
                        transferMsg);
                h.backend->dropCollection(adminUuid, QStringLiteral("appdb"), QStringLiteral("items_roundtrip"));
                Harness::pumpMs(250);
            }

            h.backend->disconnectServer(adminUuid, true);
        }

    }

    int runMatrixSelfTest(const QString &spec)
    {
        Harness h;
        const QStringList parts = spec.split(QLatin1Char(':'));
        if (parts.size() < 3) {
            h.check(false, QStringLiteral("invalid --matrix-selftest spec"), spec);
            return 1;
        }

        const QString mode = parts[0];
        if (mode == QLatin1String("noauth")) {
            const int port = parts[1].toInt();
            const QString fixturesDir = parts.mid(2).join(QLatin1Char(':'));
            Q_UNUSED(fixturesDir);
            runNoAuthSuite(h, port);
        } else if (mode == QLatin1String("version")) {
            if (parts.size() < 4) {
                h.check(false, QStringLiteral("version spec requires version:<port>:<major.minor>:<fixturesDir>"), spec);
                return 1;
            }
            const int port = parts[1].toInt();
            const QString expectedVer = parts[2];
            const QString fixturesDir = parts.mid(3).join(QLatin1Char(':'));
            runVersionSuite(h, port, expectedVer, fixturesDir);
        } else if (mode == QLatin1String("mtls")) {
            const int port = parts[1].toInt();
            const QString fixturesDir = parts.mid(2).join(QLatin1Char(':'));
            runStrictMtlsSuite(h, port, fixturesDir);
        } else if (mode == QLatin1String("rs")) {
            const QStringList ports = parts[1].split(QLatin1Char(','), Qt::SkipEmptyParts);
            const QString fixturesDir = parts.mid(2).join(QLatin1Char(':'));
            runReplicaSetSuite(h, ports, fixturesDir);
        } else if (mode == QLatin1String("sharded")) {
            const int port = parts[1].toInt();
            const QString fixturesDir = parts.mid(2).join(QLatin1Char(':'));
            runShardedSuite(h, port, fixturesDir);
        } else if (mode == QLatin1String("ssh")) {
            const int sshPort = parts[1].toInt();
            const QString fixturesDir = parts.mid(2).join(QLatin1Char(':'));
            runSshSuite(h, sshPort, fixturesDir);
        } else {
            h.check(false, QStringLiteral("unknown matrix mode"), mode);
        }

        h.check(h.failures == 0,
                h.failures == 0
                    ? QStringLiteral("suite '") + mode + QStringLiteral("' passed")
                    : QStringLiteral("suite '") + mode + QStringLiteral("' had failures"),
                QString::number(h.failures) + QStringLiteral(" failed"));
        return h.failures;
    }
}
