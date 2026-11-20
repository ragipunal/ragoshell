# Document Editor, Duplicate Template & CRUD Audit Fixes

Plan and threat/edge-case analysis for transforming `_id`-changed edits into inserts, pre-filling empty string `_id` templates with caret placement on non-`ObjectId` document duplication, and fixing bugs uncovered across the document CRUD, BSON/JSON normalization, inline cell editing, UI shortcut, layout, and localization layers.

---

## 1. Threat Model & Edge Cases

1. **Preventing accidental overwrite when `_id` is changed during Edit (`overwrite=false`)**
   - In `MongoClient`, `InsertDocumentRequest(..., /*overwrite=*/true)` executes `replace_one({_id}, doc, upsert=true)`, whereas `InsertDocumentRequest(..., /*overwrite=*/false)` executes `insert_one(doc)`.
   - If a user edits document `A` and changes `_id` to `B`, sending `overwrite=true` would silently overwrite an existing document `B` in the collection.
   - **Rule**: When `origId.get_value() != newId.get_value()` (value or BSON type changed), `ShellController::submitEditJson` MUST send `InsertDocumentRequest(..., /*overwrite=*/false)` (`insert_one`). The original document `A` remains untouched, a new document `B` is inserted, and if `B` already exists MongoDB returns `E11000 duplicate key error` without mutating existing data.
   - Removing the `_id` key completely while in `edit` mode remains rejected (`The '_id' field cannot be removed when editing an existing document.`) so accidental deletion of the `_id` line does not silently insert an auto-`ObjectId` duplicate.

2. **Preserving user edits on asynchronous MongoDB write errors**
   - `ShellController::submitInsertJson`, `ShellController::submitEditJson`, and `QmlBackend::insertDocumentsJson` dispatch writes asynchronously to `MongoWorker`.
   - Previously, `DocumentEditorDialog.trySave()` called `dlg.accept()` immediately after local JSON validation passed. Any server-side failure (`E11000 duplicate key`, `$jsonSchema` validation error, authorization failure, primary step-down) arrived later and the user's edited JSON was already lost.
   - **Rule**: `DocumentEditorDialog` stays open in a busy `_saving` state until `crudCompleted(ok, message)` (or `Backend.documentInsertFinished(ok, message)` for Explorer inserts) arrives. On `ok == true`, the dialog closes via `dlg.accept()`. On `ok == false`, `_saving` resets and the server error is shown inside the dialog's error banner with the editor buffer intact.

3. **UTF-16 vs UTF-8 caret positioning in Scintilla (`SC_CP_UTF8`)**
   - `QString` indices are UTF-16 code units, while `ScintillaQuickItem` operates in `SC_CP_UTF8` where `SCI_SETSEL` / `SCI_GOTOPOS` require UTF-8 byte positions.
   - **Rule**: `ScintillaQuickItem::setCaretPositionUtf16(int utf16Offset)` clamps `utf16Offset` to `[0, text.size()]`, converts the prefix `text.left(utf16Offset).toUtf8().size()` to an exact UTF-8 byte offset, and calls `SCI_SETSEL(bytePos, bytePos)` + `SCI_SCROLLCARET`.
   - When duplicating a document:
     - If `_id` is `ObjectId` (`bsoncxx::type::k_oid`) or absent, `_id` is omitted so MongoDB generates a fresh `ObjectId` on insert.
     - If `_id` is any other BSON type (`String`, `Int32`, `Int64`, `Double`, `Decimal128`, `UUID`/`Binary`, `Date`, `Object`, etc.), `_id` is emitted first as `"_id" : ""` and `caretOffset` points strictly between the two quotes (`""`).

4. **String-literal safety & legacy UUID round-tripping in `MongoJsonUtils`**
   - Constructor normalization (`ObjectId(...)`, `ISODate(...)`, `Date(...)`, `NumberLong(...)`, `NumberInt(...)`, `NumberDecimal(...)`, `Timestamp(...)`, `BinData(...)`, `HexData(...)`, `UUID`/`LUUID`/`NUUID`/`CSUUID`/`JUUID`/`PYUUID`, `DBRef(...)`, `MinKey()`, `MaxKey()`, `undefined`, `NaN`, `±Infinity`) MUST run via `transformOutsideStrings` so JSON string values containing shell-like text (e.g. `"Date()"`, `"ObjectId(\"...\")"`) are never mutated.
   - Legacy subtype-`03` UUID literals (`LUUID`, `JUUID`, `NUUID`, `CSUUID`, `PYUUID`) must serialize back to `$binary` `subType: "03"` using `HexUtils::uuidToHex(..., encoding)` byte ordering rather than RFC-4122 `subType: "04"`. Otherwise editing a document with a legacy UUID `_id` would falsely appear to have changed `_id`.

5. **Inline scalar cell editing fidelity**
   - `BsonTreeItem::value()` (`rawValue`) stores unquoted `k_string` values for `String` nodes. Stripping surrounding quotes in `ShellTabPage.startInPlaceEdit` or `MongoJsonUtils::appendParsedScalar` when `typeHint == "String"` corrupts strings that legitimately start and end with quotes or contain backslashes, and coercing `"null"` / `"true"` / `"false"` mutates `String` fields into `Null` / `Boolean`.
   - Multi-line strings (`\n` / `\r`) and dotted field names (`a.b`) cannot be safely round-tripped through a single-line dotted-path inline `TextField`; `BsonTreeModel::isEditableScalarItem` returns `false` for them so double-clicking opens the full `DocumentEditorDialog`.
   - Validation errors returned by `ShellController::updateScalarCell` must update `_errorText` and emit `errorChanged()` so invalid inline scalar edits are surfaced in the result error banner.

---

## 2. Implementation Steps

### Step 1 — Core BSON/JSON Utilities (`src/ragoshell/core/utils/MongoJsonUtils.*`, `BsonTreeModel.cpp`)
- Add `MongoJsonUtils::DuplicateTemplateResult` and `MongoJsonUtils::buildDuplicateDocumentTemplate(bsoncxx::document::view srcDoc, UUIDEncoding uuidEncoding, SupportedTimes timeFormat)` returning `{ json, caretUtf16Offset }`:
  - Non-`ObjectId` `_id` → prepends `"_id" : ""` and computes the exact UTF-16 offset between `""`.
  - `ObjectId` or missing `_id` → omits `_id` (falling back to `"{\n    \n}"` with caret at offset `6` when no other fields remain).
- Refactor `normalizeShellJsonToExtendedJson` to run all shell constructor/literal replacements strictly outside quoted JSON strings (`transformOutsideStrings`), add accurate `LUUID`/`JUUID`/`NUUID`/`CSUUID`/`PYUUID` (`subType: "03"`) vs `UUID` (`subType: "04"`) handling, numeric `Date(<ms>)`/`ISODate(<ms>)`, `HexData(sub, "hex")`, `DBRef("coll", "oid")`, `undefined`, `NaN`, and `±Infinity`.
- Fix `appendParsedScalar` so `typeHint == "String"` preserves exact string contents (including `"true"`, `"false"`, `"null"`, surrounding quotes, and backslashes), while untyped new table cells (`typeHint.isEmpty()`) still support scalar literal inference.
- Tighten `BsonTreeModel::isEditableScalarItem` to route multi-line strings and keys containing `.` to `DocumentEditorDialog`.

### Step 2 — Unit Tests (`src/ragoshell/core/utils/MongoJsonUtils_test.cpp`)
- Add tests covering:
  - `buildDuplicateDocumentTemplate` for `ObjectId` `_id` vs `String`, `Int32`, `Int64`, and `UUID` `_id` (verifying `"_id" : ""` and exact caret offset between `""`, including UTF-8 Turkish text).
  - String literals containing `Date()`, `ISODate(...)`, `ObjectId(...)`, `NumberInt(...)`, `MinKey()`, `UUID(...)` remaining untouched.
  - Legacy UUID (`LUUID`, `NUUID`, `CSUUID`, `JUUID`, `PYUUID`) and `UUID` binary subtype + byte-order preservation (`origId == newId`).
  - `Date(-86400000)`, `HexData`, `DBRef`, `NaN`, `Infinity`, `-Infinity`, `undefined` parsing.
  - `updateScalarFieldAtPath` preserving `"true"`, `"false"`, `"null"`, `"\"quoted\""`, and `"a\\nb"` as `k_string` when `typeHint == "String"`.

### Step 3 — Scintilla Caret & Shell/Backend CRUD (`ScintillaQuick.*`, `ShellController*`, `QmlBackend*`)
- Add `Q_INVOKABLE void setCaretPositionUtf16(int utf16Offset)` to `ScintillaQuickItem` and `setCaretPosition(utf16Offset)` on `RagoCodeEditor.qml`.
- Update `ShellController::submitEditJson`: detect `idChanged = (origId.get_value() != newId.get_value())` and dispatch `InsertDocumentRequest(..., /*overwrite=*/!idChanged)`.
- Add `Q_INVOKABLE bool isEditIdChanged(int docIndex, const QString &jsonText) const` and `Q_INVOKABLE QVariantMap duplicateTemplateAt(int docIndex) const` to `ShellController`.
- Surface `ShellController::updateScalarCell` validation errors via `_errorText` / `errorChanged()`, and clear stale `_errorText` when a subsequent CRUD operation succeeds.
- Make `QmlBackend::insertDocumentsJson` dispatch `InsertDocumentRequest(this, ...)` and emit `documentInsertFinished(bool ok, const QString &message)` on `InsertDocumentResponse` so Explorer document inserts also report async errors cleanly.

### Step 4 — QML Dialogs, Shortcuts, Layout & Turkish Localization (`src/ragoshell-app/qml/*`, `ragoshell_tr.ts`)
- Update `DocumentEditorDialog.qml`:
  - Accept optional `initialCaretOffset` in `openForInsert` (defaulting `"{\n    \n}"` to offset `6`), applying `codeEditor.setCaretPosition(...)` and `codeEditor.focusEditor()` on open.
  - Track `_editWillInsert` in `edit` mode when `_id` differs from the original document, dynamically switching the primary action button to `qsTr("Insert")` (`+`) and the dialog title to `qsTr("Insert Document")`.
  - Keep the dialog open while `_saving` until `targetShell.crudCompleted` or `Backend.documentInsertFinished` completes; display server errors inline without losing user input.
- Update `ShellTabPage.qml`:
  - `triggerDuplicateDocument` uses `duplicateTemplateAt(docIdx)` to pass both `json` and `caretOffset` to `openForInsert`.
  - `startInPlaceEdit` stops stripping quotes from `String` values.
- Fix `MainShortcuts.qml` (`Ctrl+B` / `Ctrl+E` calls `shortcutsHost.app.toggleExplorer()` instead of overwriting `explorerPane.visible`).
- Add `Layout.minimumWidth: 0` to all direct children of `StackLayout` in `ShellResultsViewport.qml`.
- Fix Turkish vowel-harmony suffixes for `%1` (`RagoShell'den`, `RagoShell'i`) and resolve the 11 `unfinished` entries in `src/ragoshell-app/i18n/ragoshell_tr.ts`.
