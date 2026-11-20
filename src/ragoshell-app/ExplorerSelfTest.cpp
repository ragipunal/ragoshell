#include "ragoshell-app/ExplorerSelfTest.h"

#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>

#include "ragoshell/core/domain/MongoCollection.h"
#include "ragoshell/core/domain/MongoCollectionInfo.h"
#include "ragoshell/core/domain/MongoDatabase.h"
#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/ServerTreeModel.h"

namespace
{
    int failures = 0;

    void check(const char *name, bool ok, const QString &detail = QString())
    {
        if (!ok)
            ++failures;
        qInfo("%s  %s%s", ok ? "PASS" : "FAIL", name,
              ok || detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -- ") + detail));
    }

    QQuickItem *findTreeView(QQuickItem *root)
    {
        if (!root)
            return nullptr;
        if (root->inherits("QQuickTreeView"))
            return root;
        const QList<QQuickItem *> children = root->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *found = findTreeView(child))
                return found;
        }
        return nullptr;
    }

    void polishAndLayout(QQuickWindow &window, QQuickItem *treeView)
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        if (treeView)
            QMetaObject::invokeMethod(treeView, "forceLayout");
        QCoreApplication::processEvents();
        (void)window.grabWindow();
        QCoreApplication::processEvents();
    }

    bool isRowExpanded(QQuickItem *treeView, int row)
    {
        bool expanded = false;
        QMetaObject::invokeMethod(treeView, "isExpanded",
                                  Q_RETURN_ARG(bool, expanded),
                                  Q_ARG(int, row));
        return expanded;
    }

    QQuickItem *delegateAtRow(QQuickItem *treeView, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeView, "itemAtCell",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    void sendSingleClick(QQuickWindow &window, QQuickItem *target, const QPointF &localPos, ulong &ts)
    {
        ts += 1000; // Strictly outside mouseDoubleClickInterval from any previous interaction.
        const QPointF scenePos = target->mapToScene(localPos);
        const QPointF globalPos = window.mapToGlobal(scenePos);

        QMouseEvent press(QEvent::MouseButtonPress, scenePos, scenePos, globalPos,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        press.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &press);

        ts += 20;
        QMouseEvent release(QEvent::MouseButtonRelease, scenePos, scenePos, globalPos,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        release.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &release);
        QCoreApplication::processEvents();
    }

    void sendDoubleClick(QQuickWindow &window, QQuickItem *target, const QPointF &localPos, ulong &ts)
    {
        ts += 1000; // Start a fresh double-click gesture.
        const QPointF scenePos = target->mapToScene(localPos);
        const QPointF globalPos = window.mapToGlobal(scenePos);

        QMouseEvent press1(QEvent::MouseButtonPress, scenePos, scenePos, globalPos,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        press1.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &press1);

        ts += 20;
        QMouseEvent release1(QEvent::MouseButtonRelease, scenePos, scenePos, globalPos,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        release1.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &release1);

        ts += 30;
        QMouseEvent press2(QEvent::MouseButtonPress, scenePos, scenePos, globalPos,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        press2.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &press2);

        QMouseEvent dbl(QEvent::MouseButtonDblClick, scenePos, scenePos, globalPos,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        dbl.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &dbl);

        ts += 20;
        QMouseEvent release2(QEvent::MouseButtonRelease, scenePos, scenePos, globalPos,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        release2.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &release2);
        QCoreApplication::processEvents();
    }
}

int RagoShell::runExplorerSelfTest(QQmlApplicationEngine *engine, QmlBackend *backend)
{
    failures = 0;
    if (!engine || !backend || !backend->serverTree()) {
        check("engine and backend initialized", false);
        return failures;
    }

    ServerTreeModel *tree = backend->serverTree();

    QQuickWindow window;
    window.resize(360, 640);

    QQmlComponent paneComp(engine, QStringLiteral("RagoShell"), QStringLiteral("ConnectionsPane"));
    QObject *paneObj = paneComp.create();
    auto *paneItem = qobject_cast<QQuickItem *>(paneObj);
    check("ConnectionsPane instantiates without QML errors",
          paneItem != nullptr && !paneComp.isError(),
          paneComp.errorString());
    if (!paneItem) {
        delete paneObj;
        return failures;
    }

    paneItem->setParentItem(window.contentItem());
    paneItem->setWidth(360);
    paneItem->setHeight(640);
    window.show();

    QQuickItem *treeView = findTreeView(paneItem);
    check("ConnectionsPane contains serverTree QQuickTreeView", treeView != nullptr);
    if (!treeView) {
        delete paneObj;
        return failures;
    }

    auto *conn = new ConnectionSettings(false);
    conn->setConnectionName("Radore Test");
    MongoServer srv(1, conn, ConnectionPrimary);
    MongoDatabase dbNfs(&srv, "nfs");
    MongoDatabase dbTwitter(&srv, "twitter");

    tree->addServerNode(&srv, QStringLiteral("selftest-srv-uuid"), QStringLiteral("4.0.28"));
    tree->setServerDatabases(&srv, {&dbNfs, &dbTwitter});
    polishAndLayout(window, treeView);

    check("server root auto-expands on populate",
          isRowExpanded(treeView, 0) && treeView->property("rows").toInt() == 3,
          QStringLiteral("rows=%1").arg(treeView->property("rows").toInt()));
    check("databases start collapsed",
          !isRowExpanded(treeView, 1) && !isRowExpanded(treeView, 2));

    ulong ts = 1000;

    // 1. Desktop single-click on a database label selects it without expanding.
    QQuickItem *nfsDel = delegateAtRow(treeView, 1);
    check("nfs delegate materialized at row 1", nfsDel != nullptr);
    if (nfsDel) {
        sendSingleClick(window, nfsDel, QPointF(130, nfsDel->height() * 0.5), ts);
        polishAndLayout(window, treeView);
        check("single-click on DB label selects row without expanding",
              treeView->property("currentRow").toInt() == 1 && !isRowExpanded(treeView, 1));
    }

    // 2. Desktop double-click on an unloaded database ("twitter" at row 2) expands it immediately.
    QQuickItem *twitterDel = delegateAtRow(treeView, 2);
    check("twitter delegate materialized at row 2", twitterDel != nullptr);
    if (twitterDel) {
        sendDoubleClick(window, twitterDel, QPointF(130, twitterDel->height() * 0.5), ts);
        polishAndLayout(window, treeView);
        const ServerTreeModel::Node *twNode = tree->findDatabaseNode(&dbTwitter);
        check("double-click on unloaded DB label expands row",
              isRowExpanded(treeView, 2) && treeView->property("rows").toInt() == 4,
              QStringLiteral("expanded=%1 rows=%2")
                  .arg(isRowExpanded(treeView, 2))
                  .arg(treeView->property("rows").toInt()));
        check("expandIndex marks DB loading synchronously",
              twNode != nullptr && twNode->loading);
    }

    // 3. Async collection population replaces "Loading…" placeholder WITHOUT collapsing the expanded DB.
    MongoCollection collTweets(&dbTwitter, MongoCollectionInfo("twitter.tweets"));
    MongoCollection collUsers(&dbTwitter, MongoCollectionInfo("twitter.users"));
    tree->setDatabaseCollections(&dbTwitter, {&collTweets, &collUsers});
    polishAndLayout(window, treeView);
    check("async setDatabaseCollections keeps expanded DB open with its collections",
          isRowExpanded(treeView, 2) && treeView->property("rows").toInt() == 5,
          QStringLiteral("expanded=%1 rows=%2")
              .arg(isRowExpanded(treeView, 2))
              .arg(treeView->property("rows").toInt()));

    // 4. Refreshing collections on an already-expanded DB keeps it expanded.
    MongoCollection collMentions(&dbTwitter, MongoCollectionInfo("twitter.mentions"));
    tree->setDatabaseCollections(&dbTwitter, {&collTweets, &collUsers, &collMentions});
    polishAndLayout(window, treeView);
    check("refreshing collections keeps expanded DB open",
          isRowExpanded(treeView, 2) && treeView->property("rows").toInt() == 6);

    // 5. Double-clicking an expanded DB label collapses it; double-clicking again re-expands it.
    twitterDel = delegateAtRow(treeView, 2);
    if (twitterDel) {
        sendDoubleClick(window, twitterDel, QPointF(130, twitterDel->height() * 0.5), ts);
        polishAndLayout(window, treeView);
        check("second double-click on DB label collapses it",
              !isRowExpanded(treeView, 2) && treeView->property("rows").toInt() == 3);

        twitterDel = delegateAtRow(treeView, 2);
        sendDoubleClick(window, twitterDel, QPointF(130, twitterDel->height() * 0.5), ts);
        polishAndLayout(window, treeView);
        check("third double-click on loaded DB label re-expands it",
              isRowExpanded(treeView, 2) && treeView->property("rows").toInt() == 6);

        // Collapse twitter again so row indices below are simple.
        twitterDel = delegateAtRow(treeView, 2);
        sendDoubleClick(window, twitterDel, QPointF(130, twitterDel->height() * 0.5), ts);
        polishAndLayout(window, treeView);
    }

    // 6. Disclosure indicator arrow ("▸") single-click and accidental fast double-click.
    nfsDel = delegateAtRow(treeView, 1);
    if (nfsDel) {
        QQuickItem *indicator = nfsDel->property("indicator").value<QQuickItem *>();
        check("DB delegate has visible disclosure indicator", indicator && indicator->isVisible());
        if (indicator) {
            sendSingleClick(window, indicator, QPointF(indicator->width() * 0.5, indicator->height() * 0.5), ts);
            polishAndLayout(window, treeView);
            check("single-click on disclosure indicator expands DB", isRowExpanded(treeView, 1));

            MongoCollection collFiles(&dbNfs, MongoCollectionInfo("nfs.files"));
            tree->setDatabaseCollections(&dbNfs, {&collFiles});
            polishAndLayout(window, treeView);
            check("indicator-expanded DB stays expanded after setDatabaseCollections",
                  isRowExpanded(treeView, 1) && treeView->property("rows").toInt() == 4);

            // Collapse via indicator single-click.
            nfsDel = delegateAtRow(treeView, 1);
            indicator = nfsDel ? nfsDel->property("indicator").value<QQuickItem *>() : nullptr;
            if (indicator) {
                sendSingleClick(window, indicator, QPointF(indicator->width() * 0.5, indicator->height() * 0.5), ts);
                polishAndLayout(window, treeView);
                check("single-click on disclosure indicator collapses DB", !isRowExpanded(treeView, 1));
            }

            // Fast double-click directly on indicator expands and does NOT undo itself.
            nfsDel = delegateAtRow(treeView, 1);
            indicator = nfsDel ? nfsDel->property("indicator").value<QQuickItem *>() : nullptr;
            if (indicator) {
                sendDoubleClick(window, indicator, QPointF(indicator->width() * 0.5, indicator->height() * 0.5), ts);
                polishAndLayout(window, treeView);
                check("fast double-click on disclosure indicator keeps DB expanded",
                      isRowExpanded(treeView, 1));
            }

            // 7. Expanding Collection's Indexes folder and populating indexes keeps Indexes expanded.
            QQuickItem *collDel = delegateAtRow(treeView, 2); // nfs.files at row 2
            QQuickItem *collInd = collDel ? collDel->property("indicator").value<QQuickItem *>() : nullptr;
            if (collInd) {
                sendSingleClick(window, collInd, QPointF(collInd->width() * 0.5, collInd->height() * 0.5), ts);
                polishAndLayout(window, treeView);
                check("clicking collection indicator expands Indexes folder row", isRowExpanded(treeView, 2));

                QQuickItem *idxFolderDel = delegateAtRow(treeView, 3); // Indexes folder at row 3
                if (idxFolderDel) {
                    sendDoubleClick(window, idxFolderDel, QPointF(140, idxFolderDel->height() * 0.5), ts);
                    polishAndLayout(window, treeView);
                    check("double-clicking Indexes folder expands it", isRowExpanded(treeView, 3));

                    IndexInfo idIdx(MongoCollectionInfo("nfs.files"), "_id_", "{ \"_id\" : 1 }");
                    tree->setCollectionIndexes(&srv, QStringLiteral("nfs"), QStringLiteral("files"), {idIdx});
                    polishAndLayout(window, treeView);
                    check("async setCollectionIndexes keeps Indexes folder expanded",
                          isRowExpanded(treeView, 3));
                }
            }
        }
    }

    tree->removeServerNode(&srv);
    polishAndLayout(window, treeView);
    delete paneObj;

    qInfo("%s", failures ? qPrintable(QStringLiteral("%1 check(s) failed").arg(failures))
                         : "all checks passed");
    return failures;
}
