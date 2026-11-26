#include "ragoshell-app/ResultSelectionSelfTest.h"

#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantList>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/events/MongoShellEvents.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell-app/ShellController.h"

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

    QList<int> selectedIndices(QObject *pageItem)
    {
        QList<int> out;
        const QVariantList raw = pageItem->property("selectedDocIndices").toList();
        out.reserve(raw.size());
        for (const QVariant &v : raw)
            out.append(v.toInt());
        return out;
    }

    QString formatIndices(const QList<int> &list)
    {
        QStringList parts;
        for (int v : list)
            parts << QString::number(v);
        return QStringLiteral("[") + parts.join(QLatin1String(",")) + QStringLiteral("]");
    }

    QQuickItem *findByClass(QQuickItem *root, const char *className, bool exactTree)
    {
        if (!root)
            return nullptr;
        const bool isTree = root->inherits("QQuickTreeView");
        const bool isHeader = root->inherits("QQuickHeaderViewBase");
        if (exactTree ? isTree : (root->inherits(className) && !isTree && !isHeader))
            return root;
        const QList<QQuickItem *> children = root->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *found = findByClass(child, className, exactTree))
                return found;
        }
        return nullptr;
    }

    void polishView(QQuickWindow &window, QQuickItem *view)
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        if (view)
            QMetaObject::invokeMethod(view, "forceLayout");
        QCoreApplication::processEvents();
        (void)window.grabWindow();
        QCoreApplication::processEvents();
    }

    QQuickItem *cellAt(QQuickItem *view, int col, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(view, "itemAtCell",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(QPoint, QPoint(col, row)));
        return item;
    }

    bool isRowExpanded(QQuickItem *treeView, int row)
    {
        bool expanded = false;
        QMetaObject::invokeMethod(treeView, "isExpanded",
                                  Q_RETURN_ARG(bool, expanded),
                                  Q_ARG(int, row));
        return expanded;
    }

    void sendClick(QQuickWindow &window, QQuickItem *target, const QPointF &localPos,
                   Qt::MouseButton button, Qt::KeyboardModifiers mods, ulong &ts)
    {
        ts += 1000; // Outside double-click interval
        const QPointF scenePos = target->mapToScene(localPos);
        const QPointF globalPos = window.mapToGlobal(scenePos);

        QMouseEvent press(QEvent::MouseButtonPress, scenePos, scenePos, globalPos,
                          button, button, mods);
        press.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &press);

        ts += 20;
        QMouseEvent release(QEvent::MouseButtonRelease, scenePos, scenePos, globalPos,
                            button, Qt::NoButton, mods);
        release.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &release);
        QCoreApplication::processEvents();
    }

    void sendKey(QQuickWindow &window, int key, Qt::KeyboardModifiers mods)
    {
        QKeyEvent press(QEvent::KeyPress, key, mods);
        QCoreApplication::sendEvent(&window, &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods);
        QCoreApplication::sendEvent(&window, &release);
        QCoreApplication::processEvents();
    }
}

int RagoShell::runResultSelectionSelfTest(QQmlApplicationEngine *engine)
{
    failures = 0;
    if (!engine) {
        check("engine initialized", false);
        return failures;
    }

    auto *settings = AppRegistry::instance().settingsManager();
    const bool origAutoExpand = settings ? settings->autoExpand() : false;
    if (settings)
        settings->setAutoExpand(false);

    ShellController ctl(nullptr, QStringLiteral("news"));
    ctl.setViewMode(0);

    // Build 10 BSON documents with nested child objects so hasChildren is true in TreeView.
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::sub_document;
    std::vector<MongoDocumentPtr> docs;
    docs.reserve(10);
    for (int i = 0; i < 10; ++i) {
        bsoncxx::builder::basic::document d;
        d.append(kvp("_id", QStringLiteral("doc-%1").arg(i).toStdString()));
        d.append(kvp("url", QStringLiteral("https://example.org/item-%1").arg(i).toStdString()));
        d.append(kvp("meta", [i](sub_document sub) {
            sub.append(kvp("rank", i));
        }));
        docs.push_back(std::make_shared<MongoDocument>(d.extract()));
    }

    DocumentListLoadedEvent loadedEv(nullptr, 0, MongoQueryInfo(), "db.news.find({})", docs);
    ctl.handle(&loadedEv);

    QQuickWindow window;
    window.resize(960, 720);

    QQmlComponent pageComp(engine, QStringLiteral("RagoShell"), QStringLiteral("ShellTabPage"));
    QVariantMap initProps{{QStringLiteral("shell"), QVariant::fromValue(static_cast<QObject *>(&ctl))}};
    QObject *pageObj = pageComp.createWithInitialProperties(initProps);
    auto *pageItem = qobject_cast<QQuickItem *>(pageObj);
    check("ShellTabPage instantiates without QML errors",
          pageItem != nullptr && !pageComp.isError(),
          pageComp.errorString());
    if (!pageItem) {
        delete pageObj;
        if (settings)
            settings->setAutoExpand(origAutoExpand);
        return failures;
    }

    pageItem->setParentItem(window.contentItem());
    pageItem->setWidth(960);
    pageItem->setHeight(720);
    window.show();

    // Re-emit result populated state now that ShellTabPage's Connections are active.
    ctl.handle(&loadedEv);

    QQuickItem *treeView = findByClass(pageItem, "QQuickTreeView", true);
    QQuickItem *tableView = findByClass(pageItem, "QQuickTableView", false);
    check("TreeView and TableView located", treeView != nullptr && tableView != nullptr);
    if (!treeView || !tableView) {
        delete pageObj;
        if (settings)
            settings->setAutoExpand(origAutoExpand);
        return failures;
    }

    polishView(window, treeView);
    ulong ts = 1000;

    // ---- 1. TreeView multi-selection ---------------------------------------
    QQuickItem *row2 = cellAt(treeView, 0, 2);
    check("TreeView row 2 delegate materialized", row2 != nullptr);
    if (row2) {
        sendClick(window, row2, QPointF(120, row2->height() * 0.5), Qt::LeftButton, Qt::NoModifier, ts);
        polishView(window, treeView);
        check("TreeView: single click selects [2]",
              selectedIndices(pageItem) == QList<int>{2},
              formatIndices(selectedIndices(pageItem)));
    }

    QQuickItem *row6 = cellAt(treeView, 0, 6);
    if (row6) {
        sendClick(window, row6, QPointF(120, row6->height() * 0.5), Qt::LeftButton, Qt::ShiftModifier, ts);
        polishView(window, treeView);
        const QList<int> want{2, 3, 4, 5, 6};
        check("TreeView: Shift+Click selects range [2..6]",
              selectedIndices(pageItem) == want,
              formatIndices(selectedIndices(pageItem)));
        check("TreeView: delegate docSelected property updates reactively on range",
              row6->property("docSelected").toBool() && row2->property("docSelected").toBool());
    }

    QQuickItem *row4 = cellAt(treeView, 0, 4);
    if (row4) {
        sendClick(window, row4, QPointF(120, row4->height() * 0.5), Qt::LeftButton, Qt::ShiftModifier, ts);
        polishView(window, treeView);
        const QList<int> want{2, 3, 4};
        check("TreeView: subsequent Shift+Click adjusts range from anchor [2..4]",
              selectedIndices(pageItem) == want,
              formatIndices(selectedIndices(pageItem)));
    }

    QQuickItem *row3 = cellAt(treeView, 0, 3);
    if (row3) {
        sendClick(window, row3, QPointF(120, row3->height() * 0.5), Qt::LeftButton, Qt::ControlModifier, ts);
        polishView(window, treeView);
        const QList<int> want{2, 4};
        check("TreeView: Ctrl+Click on selected row 3 unselects it -> [2,4]",
              selectedIndices(pageItem) == want,
              formatIndices(selectedIndices(pageItem)));
    }

    QQuickItem *row7 = cellAt(treeView, 0, 7);
    if (row7) {
        sendClick(window, row7, QPointF(120, row7->height() * 0.5), Qt::LeftButton, Qt::ControlModifier, ts);
        polishView(window, treeView);
        const QList<int> want{2, 4, 7};
        check("TreeView: Ctrl+Click on unselected row 7 adds it -> [2,4,7]",
              selectedIndices(pageItem) == want,
              formatIndices(selectedIndices(pageItem)));
    }

    QQuickItem *row9 = cellAt(treeView, 0, 9);
    if (row9) {
        sendClick(window, row9, QPointF(120, row9->height() * 0.5), Qt::LeftButton,
                  Qt::ControlModifier | Qt::ShiftModifier, ts);
        polishView(window, treeView);
        const QList<int> want{2, 4, 7, 8, 9};
        check("TreeView: Ctrl+Shift+Click unions range [7..9] -> [2,4,7,8,9]",
              selectedIndices(pageItem) == want,
              formatIndices(selectedIndices(pageItem)));
    }

    QQuickItem *row8 = cellAt(treeView, 0, 8);
    if (row8) {
        // Right-clicking an already-selected row must preserve multi-selection for batch delete.
        const QPointF scenePos = row8->mapToScene(QPointF(120, row8->height() * 0.5));
        const QPointF globalPos = window.mapToGlobal(scenePos);
        ts += 1000;
        QMouseEvent rpress(QEvent::MouseButtonPress, scenePos, scenePos, globalPos,
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        rpress.setTimestamp(ts);
        QCoreApplication::sendEvent(&window, &rpress);
        QCoreApplication::processEvents();
        const QList<int> want{2, 4, 7, 8, 9};
        check("TreeView: Right-press on selected row preserves multi-selection [2,4,7,8,9]",
              selectedIndices(pageItem) == want,
              formatIndices(selectedIndices(pageItem)));
    }

    // Shift+Click near the left expand arrow (x=14) must select range without expanding the node.
    QQuickItem *row1 = cellAt(treeView, 0, 1);
    if (row1 && row3) {
        sendClick(window, row1, QPointF(120, row1->height() * 0.5), Qt::LeftButton, Qt::NoModifier, ts);
        sendClick(window, row3, QPointF(14, row3->height() * 0.5), Qt::LeftButton, Qt::ShiftModifier, ts);
        polishView(window, treeView);
        const QList<int> want{1, 2, 3};
        check("TreeView: Shift+Click over arrow zone selects [1,2,3] without expanding",
              selectedIndices(pageItem) == want && !isRowExpanded(treeView, 3),
              formatIndices(selectedIndices(pageItem)));
    }

    // Keyboard Shift+Down extends selection to [1,2,3,4], and Ctrl+A selects all 10 documents.
    sendKey(window, Qt::Key_Down, Qt::ShiftModifier);
    check("TreeView: Shift+Down extends selection to [1,2,3,4]",
          selectedIndices(pageItem) == (QList<int>{1, 2, 3, 4}),
          formatIndices(selectedIndices(pageItem)));

    sendKey(window, Qt::Key_A, Qt::ControlModifier);
    check("TreeView: Ctrl+A selects all 10 documents",
          selectedIndices(pageItem).size() == 10,
          formatIndices(selectedIndices(pageItem)));

    // ---- 2. TableView multi-selection --------------------------------------
    ctl.setViewMode(1);
    polishView(window, tableView);

    QQuickItem *tRow1 = cellAt(tableView, 0, 1);
    QQuickItem *tRow5 = cellAt(tableView, 0, 5);
    QQuickItem *tRow3 = cellAt(tableView, 0, 3);
    check("TableView cells materialized", tRow1 != nullptr && tRow5 != nullptr && tRow3 != nullptr);
    if (tRow1 && tRow5 && tRow3) {
        sendClick(window, tRow1, QPointF(40, tRow1->height() * 0.5), Qt::LeftButton, Qt::NoModifier, ts);
        sendClick(window, tRow5, QPointF(40, tRow5->height() * 0.5), Qt::LeftButton, Qt::ShiftModifier, ts);
        polishView(window, tableView);
        check("TableView: Shift+Click selects range [1..5]",
              selectedIndices(pageItem) == (QList<int>{1, 2, 3, 4, 5}),
              formatIndices(selectedIndices(pageItem)));

        sendClick(window, tRow3, QPointF(40, tRow3->height() * 0.5), Qt::LeftButton, Qt::ControlModifier, ts);
        polishView(window, tableView);
        check("TableView: Ctrl+Click toggles off row 3 -> [1,2,4,5]",
              selectedIndices(pageItem) == (QList<int>{1, 2, 4, 5}),
              formatIndices(selectedIndices(pageItem)));

        sendKey(window, Qt::Key_A, Qt::ControlModifier);
        check("TableView: Ctrl+A selects all 10 documents",
              selectedIndices(pageItem).size() == 10,
              formatIndices(selectedIndices(pageItem)));
    }

    delete pageObj;
    if (settings)
        settings->setAutoExpand(origAutoExpand);

    qInfo("%s", failures ? qPrintable(QStringLiteral("%1 check(s) failed").arg(failures))
                         : "all checks passed");
    return failures;
}
