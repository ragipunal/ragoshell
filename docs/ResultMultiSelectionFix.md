# Result Tree & Table Multi-Document Selection Fix

Root-cause analysis, edge cases, and verification plan for `Ctrl+Click`, `Shift+Click`, `Ctrl+Shift+Click`, keyboard range selection (`Shift+Up` / `Shift+Down`), and `Ctrl+A` across `ShellResultTreeView.qml`, `ShellResultTableView.qml`, `ShellTabPage.qml`, and `ShellResultContextMenu.qml`.

---

## 1. Root Causes

1. **`populateContextFromCell()` overwriting selection state before `selectDocumentWithModifiers()` (`ShellResultTreeView.qml`, `ShellResultTableView.qml`, `ShellTabPage.qml`)**
   - In both `ShellResultTreeView.qml` (`onPressed`) and `ShellResultTableView.qml` (`onPressed`), `page.populateContextFromCell(docIdx, ...)` was called *before* `page.selectDocumentWithModifiers(docIdx, mouse.modifiers)`.
   - Inside `populateContextFromCell()`, `if (docIdx >= 0 && !isDocSelected(docIdx))` unconditionally reset `selectedDocIndices = [docIdx]` and `_selectionAnchorDoc = docIdx` (logic intended only for right-click / long-press on an unselected row).
   - Consequently:
     - **`Shift+Click` on row `B` after selecting row `A`**: `populateContextFromCell(B)` overwrote `_selectionAnchorDoc = B` first, so `selectDocumentWithModifiers(B, ShiftModifier)` computed `B..B` instead of `A..B`, selecting only `[B]`.
     - **`Ctrl+Click` on unselected row `B` after selecting row `A`**: `populateContextFromCell(B)` wiped `selectedDocIndices` to `[B]`, and `selectDocumentWithModifiers(B, ControlModifier)` then saw `B` already in `selectedDocIndices` and toggled it *off*, leaving `selectedDocIndices = []`.

2. **Indirect QML binding evaluation order on `isDocSelected(docIdx)`**
   - Delegate row highlight bindings called `page.isDocSelected(docIdx)` where `docIdx >= 0` short-circuited before reading `selectedDocIndices` when `docIdx == -1` during initial delegate creation, risking missed reactive updates on pooled delegates. Binding `docSelected` directly to `page.selectedDocIndices.indexOf(docIdx) !== -1 && docIdx >= 0` guarantees QML property subscription on every delegate cell.

3. **Unintentional tree expand/collapse during `Ctrl+Click` / `Shift+Click` (`ShellResultTreeView.qml`)**
   - Clicking within the left arrow zone (`mouse.x < arrowBound`) while holding `Shift` or `Ctrl` also triggered `treeView.toggleExpanded(row)`, shifting row positions mid-selection.

4. **Missing keyboard multi-selection & multi-document JSON copy**
   - `Ctrl+A` (Select All), `Up`/`Down`, `Shift+Up`/`Shift+Down` range extension, and `Ctrl+Shift+Click` additive range selection were not wired on the result views, and `copySelectedValueOrDoc(true)` (`Ctrl+Shift+C`) copied only the first selected document even when multiple documents were selected.

---

## 2. Edge Cases & Invariants

1. **Mouse modifier combinations (Tree & Table views)**
   - **Plain Left-Click on `d`**: `selectedDocIndices = [d]`, `_selectionAnchorDoc = d`, `_focusDocIndex = d`.
   - **`Shift+Click` on `d` (with anchor `a >= 0`)**: replaces `selectedDocIndices` with `[min(a, d) .. max(a, d)]`, keeps `_selectionAnchorDoc = a` so subsequent `Shift+Click` calls adjust the range from the original anchor, and sets `_focusDocIndex = d`.
   - **`Ctrl+Click` (`Cmd+Click` on macOS) on `d`**: toggles `d` in `selectedDocIndices` without clearing other selected documents; updates `_selectionAnchorDoc = d` and `_focusDocIndex = d`. Unselecting `d` must not be undone by `populateContextFromCell()`.
   - **`Ctrl+Shift+Click` on `d` (with anchor `a >= 0`)**: unions `[min(a, d) .. max(a, d)]` into existing `selectedDocIndices`, preserving previously `Ctrl`-selected rows outside the range.
   - **Right-Click / Long-Press on `d`**: calls `populateContextFromCell(..., ensureSelected = true)` — preserves the multi-selection when `d` is already selected (enabling right-click -> *Delete N Documents…* or *Copy Document JSON*), and selects `[d]` only when `d` was not part of the selection.
   - **Disclosure arrow click (`▸` / `▾`)**: toggles expansion only when neither `Ctrl` nor `Shift` is held.

2. **Keyboard navigation & shortcuts**
   - **`Ctrl+A` (`Cmd+A`)**: selects `[0 .. resultCount - 1]` when not inline-editing a cell.
   - **`Up` / `Down`**: moves selection to the previous/next document and scrolls it into view.
   - **`Shift+Up` / `Shift+Down`**: extends/shrinks range selection from `_selectionAnchorDoc` to the stepped `_focusDocIndex` and scrolls it into view.
   - **`Delete` (`Cmd+Backspace` on macOS)**: opens batch confirmation (`Delete %n Documents`) and deletes all `selectedDocIndices` via `ShellController::deleteDocumentsAt()`.
   - **`Ctrl+Shift+C` (*Copy Document JSON*)**: copies a single JSON object when 1 document is selected, or a formatted JSON array `[...]` when multiple documents are selected.

3. **Qt 6.12 `TableView` / `TreeView` pointer navigation**
   - Set `pointerNavigationEnabled: false` on both `treeView` and `tableView` so Qt Quick's internal pointer handler never competes with cell `MouseArea` modifier clicks or double-clicks.

---

## 3. Implementation & Verification Steps

1. Update `ShellTabPage.qml` (`selectDocumentWithModifiers`, `populateContextFromCell`, `selectAllDocuments`, `stepDocumentSelection`, `copySelectedValueOrDoc`, `_focusDocIndex`).
2. Update `ShellResultTreeView.qml` and `ShellResultTableView.qml` (`onPressed` ordering with `ensureSelected = false` for left button, direct `docSelected` binding, `pointerNavigationEnabled: false`, modifier guard on expand toggle, `Ctrl+A` and `(Shift+)Up/Down` handlers).
3. Add `Select All` (`Ctrl+A`) item to `ShellResultContextMenu.qml`.
4. Verify via automated selection self-test, `rago_unit_tests`, `ragoshell --check-qml`, and `node ci/check-i18n.mjs`.
