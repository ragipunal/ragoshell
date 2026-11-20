# Explorer Tree Double-Click & Async Expansion Fix

Root-cause analysis, edge cases, and verification plan for database/folder double-click expansion and asynchronous child population in the Connected Explorer tree (`ConnectionsPane.qml` + `ServerTreeModel`).

---

## 1. Root Causes

1. **Qt 6.12 `TreeView` / `TreeViewDelegate` swallowing `onDoubleClicked` (`ConnectionsPane.qml`)**
   - `QQuickTreeView` inherits `QQuickTableView`, where `pointerNavigationEnabled` defaults to `true`.
   - In Qt 6.12, when `pointerNavigationEnabled` is `true`, `QQuickTreeViewDelegate::mousePressEvent` intercepts left-button presses for internal pointer navigation in a way that prevents `QQuickAbstractButton` from ever emitting `doubleClicked()`. Two rapid clicks on a database, server, folder, collection, replica-set member, index, or function row emit `clicked()` twice and never invoke `onDoubleClicked: pane.activateTreeRow(treeDelegate)`.
   - Setting `pointerNavigationEnabled: false` on `serverTree` restores `TreeViewDelegate.onDoubleClicked` delivery while keeping `keyNavigationEnabled: true` (arrow keys, `Return`/`Enter`) and `Flickable` wheel/touch scrolling intact.

2. **`ServerTreeModel::replaceChildren` transiently dropping `rowCount` to `0` (`ServerTreeModel.cpp`)**
   - Unloaded `Database`, `FunctionsFolder`, and `IndexesFolder` nodes start with a single `Placeholder` (`"Loading…"`) child so `hasChildren` is `true`.
   - When asynchronous loading completes (`setDatabaseCollections`, `setDatabaseFunctions`, `setCollectionIndexes`, `updateReplicaSetFolder`, `setServerDatabases`), `ServerTreeModel::replaceChildren` previously removed all existing children (`0 .. oldCount - 1`) and called `endRemoveRows()` *before* inserting `newChildren`.
   - Inside `endRemoveRows()`, `parentNode->children` was empty (`rowCount(pIdx) == 0`, `hasChildren(pIdx) == false`). Qt 6's `QQmlTreeModelToTableModel::modelRowsRemoved` treats a parent whose child count hits `0` as having no children and immediately collapses `pIdx`, causing freshly loaded databases (`ip (3)`, `nfs (78)`, `press (2)`) to snap shut right as their collections arrive.
   - Additionally, `qDeleteAll(parentNode->children)` (and `delete node` / `delete folder` in `removeServerNode` / `setDatabaseFunctions`) ran *before* `endRemoveRows()`, leaving freed `Node*` `internalPointer()` values visible during Qt's persistent-index cleanup.

3. **Duplicate async load requests on expand (`ServerTreeModelPopulate.cpp`)**
   - Both `TreeView.onExpanded` and `TreeViewDelegate.onExpandedChanged` call `Backend.serverTree.expandIndex(...)`.
   - Because `MongoDatabase::loadCollections()` and `loadFunctions()` publish their `*LoadingEvent` asynchronously through `EventBus`, `node->loading` remained `false` across synchronous back-to-back `expandIndex` calls, dispatching duplicate `listCollections` / `listIndexes` / `system.js` requests.

---

## 2. Edge Cases & Invariants

1. **Desktop single-click vs. double-click on row body**
   - Single left-click selects the row (`ItemSelectionModel.Current | ItemSelectionModel.ClearAndSelect`) and gives `serverTree` active focus without toggling expansion.
   - Double left-click calls `pane.activateTreeRow(treeDelegate)`: toggles expansion once for `database`, `server`, `rsFolder`, `systemDbFolder`, `systemCollFolder`, `indexesFolder`, and `functionsFolder` (refreshing `rsFolder` when newly expanded), and opens the shell / editor for `collection`, `rsMember`, `index`, and `function` without toggling `Indexes`.

2. **Disclosure indicator (`▸` / `▾`) clicks**
   - Single left-click on the disclosure arrow selects the row and toggles expansion (including on `collection` rows on both desktop and mobile, expanding `Indexes` without opening a shell).
   - An accidental fast double-click directly on the disclosure arrow absorbs `onDoubleClicked` so the first click's expand/collapse action is not immediately undone.
   - Both LTR and RTL (`mirrored`) indicator positions remain aligned with `TreeViewDelegate`'s indentation math.

3. **Context menu triggers (right-click, `⋮` action button, mobile long-press)**
   - Right-clicking a row, clicking its `⋮` button, or long-pressing on touch selects that row and opens `nodeMenu` without toggling expansion or activating the row on touch release.

4. **Model mutation invariants**
   - When `newChildren` is non-empty, `replaceChildren` inserts `newChildren` at `[oldCount .. oldCount + newCount - 1]` *before* removing `[0 .. oldCount - 1]`, guaranteeing `rowCount(pIdx) >= 1` throughout so expanded nodes stay expanded across initial lazy load and subsequent refreshes.
   - When `newChildren` is empty (e.g. a genuinely empty database with 0 collections), removing `[0 .. oldCount - 1]` leaves `rowCount(pIdx) == 0` and hides the disclosure arrow cleanly.
   - Removed `Node*` instances are deleted strictly *after* `endRemoveRows()`.
   - `expandIndex` marks `loading = true` synchronously before dispatching async load requests so repeated calls in the same tick are idempotent.

---

## 3. Implementation & Verification Steps

1. **`src/ragoshell-app/ServerTreeModel.cpp` & `ServerTreeModelPopulate.cpp`**:
   - Update `replaceChildren` to insert new rows before removing old rows and defer `qDeleteAll` until after `endRemoveRows()`.
   - Defer `delete` until after `endRemoveRows()` in `removeServerNode` and `setDatabaseFunctions`, and clone preserved `FunctionsFolder` state cleanly into `newChildren` in `setDatabaseCollections`.
   - Make `expandIndex` set the corresponding loading state synchronously before initiating async loads.
2. **`src/ragoshell-app/qml/ConnectionsPane.qml`**:
   - Set `pointerNavigationEnabled: false` on `serverTree`, add `pane.selectTreeRow(row)`, wire `treeDelegate.onPressed` and context-menu selection, and add a dedicated `MouseArea` inside `treeDelegate.indicator`.
3. **Automated Self-Test (`src/ragoshell-app/ExplorerSelfTest.*`, `--explorer-selftest`)**:
   - Verify double-click expand/collapse on unloaded and loaded `Database` nodes, async `setDatabaseCollections` / `setCollectionIndexes` expansion retention, single-click selection without expansion, indicator single/double click, `Collection` double-click shell activation, and idempotent `expandIndex` loading.
