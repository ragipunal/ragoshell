#include "ragoshell-app/ScintillaQuickBackend.h"
#include "ragoshell/core/utils/QtUtils.h"

// ---- input -----------------------------------------------------------------

namespace
{
    int scintillaKey(int qtKey, int &modifiers)
    {
        switch (qtKey) {
        case Qt::Key_Down: return SCK_DOWN;
        case Qt::Key_Up: return SCK_UP;
        case Qt::Key_Left: return SCK_LEFT;
        case Qt::Key_Right: return SCK_RIGHT;
        case Qt::Key_Home: return SCK_HOME;
        case Qt::Key_End: return SCK_END;
        case Qt::Key_PageUp: return SCK_PRIOR;
        case Qt::Key_PageDown: return SCK_NEXT;
        case Qt::Key_Delete: return SCK_DELETE;
        case Qt::Key_Insert: return SCK_INSERT;
        case Qt::Key_Escape: return SCK_ESCAPE;
        case Qt::Key_Backspace: return SCK_BACK;
        case Qt::Key_Tab: return SCK_TAB;
        case Qt::Key_Backtab:
            modifiers |= SCMOD_SHIFT;
            return SCK_TAB;
        case Qt::Key_Return:
        case Qt::Key_Enter: return SCK_RETURN;
        case Qt::Key_Super_L: return SCK_WIN;
        case Qt::Key_Super_R: return SCK_RWIN;
        case Qt::Key_Menu: return SCK_MENU;
        default:
            return qtKey > 0x7f ? 0 : qtKey;
        }
    }
}

void ScintillaQuickItem::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = (mods & Qt::ControlModifier) != 0;
    const bool shift = (mods & Qt::ShiftModifier) != 0;
    const bool alt = (mods & Qt::AltModifier) != 0;
    const bool meta = (mods & Qt::MetaModifier) != 0;

    // Autocomplete: Ctrl+Space (Win/Linux) or Meta+Space / Alt+Esc (macOS)
    if (!isReadOnly() &&
        ((ctrl && event->key() == Qt::Key_Space) ||
         (meta && event->key() == Qt::Key_Space) ||
         (alt && event->key() == Qt::Key_Escape))) {
        emit autocompleteRequested();
        event->accept();
        return;
    }

    // Execute script/selection: Ctrl+Return / Ctrl+Enter (Cmd+Return on macOS)
    if (ctrl && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        emit executeRequested();
        event->accept();
        return;
    }

    // Find bar: Ctrl+F / Cmd+F
    if (event->matches(QKeySequence::Find) || (ctrl && !shift && !alt && event->key() == Qt::Key_F)) {
        emit findRequested();
        event->accept();
        return;
    }

    // Toggle line comment: Ctrl+/ (Cmd+/) or Ctrl+Shift+C
    if (!isReadOnly() &&
        ((ctrl && event->key() == Qt::Key_Slash) ||
         (ctrl && shift && event->key() == Qt::Key_C))) {
        toggleLineComment();
        event->accept();
        return;
    }

    // Format JSON/JS: Alt+Shift+F or Ctrl+Shift+I
    if (!isReadOnly() &&
        ((alt && shift && event->key() == Qt::Key_F) ||
         (ctrl && shift && event->key() == Qt::Key_I))) {
        emit formatRequested();
        event->accept();
        return;
    }

    // While a QML overlay (autocomplete) is open, navigation keys go to
    // the overlay — Scintilla would otherwise consume them.
    if (_overlayActive) {
        switch (event->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            emit overlayKeyPressed(static_cast<int>(event->key()));
            event->accept();
            return;
        default:
            break;
        }
    }

    int modifiers = 0;
    if (shift) modifiers |= SCMOD_SHIFT;
    if (ctrl) modifiers |= SCMOD_CTRL;
    if (alt) modifiers |= SCMOD_ALT;
    if (meta) modifiers |= SCMOD_META;

    int key = scintillaKey(event->key(), modifiers);
    if (key) {
        bool consumed = false;
        _backend->KeyDownWithModifiers(key, modifiers, &consumed);
        if (consumed) {
            event->accept();
            update();
            return;
        }
    }

    if (!isReadOnly()) {
        const QString text = RagoShell::QtUtils::normalizeSmartQuotes(event->text());
        // On Windows AltGr arrives as Ctrl+Alt, yet it still produces text
        // ('@', '{', '[', '\\' on Turkish/German layouts) that must be inserted.
        const bool altGr = ctrl && alt;
        if (!text.isEmpty() && text[0].isPrint() && (!ctrl || altGr) && !meta) {
            QByteArray bytes = _backend->IsUnicodeMode() ? text.toUtf8()
                                                         : text.toLatin1();
            _backend->AddCharUTF(bytes.data(), bytes.size());
            event->accept();
            update();
            return;
        }
    }
    QQuickPaintedItem::keyPressEvent(event);
}

void ScintillaQuickItem::keyReleaseEvent(QKeyEvent *event)
{
    event->accept();
}

void ScintillaQuickItem::mousePressEvent(QMouseEvent *event)
{
    finishComposition();
    forceActiveFocus();
    if (event->button() == Qt::RightButton) {
        emit contextMenuRequested(event->position().x(), event->position().y());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        unsigned clickTime = static_cast<unsigned>(
            QDateTime::currentMSecsSinceEpoch() & 0x7fffffff);
        _backend->ButtonDown(Point(event->position().x(), event->position().y()),
                             clickTime,
                             event->modifiers() & Qt::ShiftModifier,
                             event->modifiers() & Qt::ControlModifier,
                             event->modifiers() & Qt::AltModifier);
    }
    update();
}

void ScintillaQuickItem::mouseMoveEvent(QMouseEvent *event)
{
    _backend->ButtonMove(Point(event->position().x(), event->position().y()));
    update();
}

void ScintillaQuickItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        _backend->ButtonUp(Point(event->position().x(), event->position().y()), 0,
                           event->modifiers() & Qt::ControlModifier);
    update();
}

void ScintillaQuickItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        unsigned clickTime = static_cast<unsigned>(
            _backend->lastClickTime + Platform::DoubleClickTime() - 1);
        _backend->ButtonDown(Point(event->position().x(), event->position().y()),
                             clickTime, false, false, false);
    }
    update();
}

void ScintillaQuickItem::wheelEvent(QWheelEvent *event)
{
    const QPoint angle = event->angleDelta();
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool horizontalDominant = shift
        ? (angle.y() != 0 || angle.x() != 0)
        : (std::abs(angle.x()) > std::abs(angle.y()));

    if (horizontalDominant) {
        const int hDelta = angle.x() != 0 ? angle.x() : angle.y();
        if (_hScrollMax > 0 && hDelta != 0) {
            if ((_wheelAccumX > 0 && hDelta < 0) || (_wheelAccumX < 0 && hDelta > 0))
                _wheelAccumX = 0;
            _wheelAccumX += hDelta;
            const int step = std::max(8, _hScrollPage / 8);
            const int px = (_wheelAccumX * step) / 120;
            if (px != 0) {
                _wheelAccumX -= (px * 120) / step;
                scrollHorizontallyTo(_hScrollValue - px);
            }
        }
        event->accept();
        return;
    }

    if (angle.y() != 0) {
        if ((_wheelAccumY > 0 && angle.y() < 0) || (_wheelAccumY < 0 && angle.y() > 0))
            _wheelAccumY = 0;
        _wheelAccumY += angle.y();
        // 40 angleDelta units = 1 line (a standard 120-unit mouse notch scrolls 3 lines;
        // fine trackpad deltas < 120 accumulate smoothly instead of truncating to 0).
        const int lines = _wheelAccumY / 40;
        if (lines != 0) {
            _wheelAccumY %= 40;
            const int topLine = static_cast<int>(
                _backend->WndProc(SCI_GETFIRSTVISIBLELINE, 0, 0));
            _backend->ScrollTo(topLine - lines);
            update();
        }
    }
    event->accept();
}

void ScintillaQuickItem::syncTickingState()
{
    if (_backend)
        _backend->SetTicking(isVisible() && ((hasActiveFocus() && !_readOnly) || _backend->_capturedMouse));
}

void ScintillaQuickItem::focusInEvent(QFocusEvent *event)
{
    _backend->WndProc(SCI_SETFOCUS, 1, 0);
    syncTickingState();
    update();
    QQuickPaintedItem::focusInEvent(event);
}

void ScintillaQuickItem::focusOutEvent(QFocusEvent *event)
{
    finishComposition();
    _backend->WndProc(SCI_SETFOCUS, 0, 0);
    syncTickingState();
    update();
    QQuickPaintedItem::focusOutEvent(event);
}

void ScintillaQuickItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == QQuickItem::ItemVisibleHasChanged) {
        syncTickingState();
        if (value.boolValue)
            update();
    } else if (change == QQuickItem::ItemDevicePixelRatioHasChanged ||
               change == QQuickItem::ItemSceneChange) {
        if (_backend) {
            applyTheme();
            _backend->ChangeSize();
            update();
        }
    }
}

void ScintillaQuickItem::geometryChange(const QRectF &newGeometry,
                                        const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    _backend->ChangeSize();
    update();
}

// ---- Input methods (CJK composition) --------------------------------------
// Follows QScintilla's InputMethod.cpp: the composition is shown inline,
// outside the undo history, underlined with indicators from 24 up. Unlike
// QScintilla 2.8.4, positions from Qt (in characters) are converted to bytes
// and an empty event while composing clears the composition (IME cancel).

namespace
{
    constexpr int kInputMethodIndicator = 24;   // INDIC_CONTAINER range, unused elsewhere

    QString utf8Range(ScintillaQuickBackend *sci, int start, int end)
    {
        if (end <= start)
            return {};
        QByteArray buffer(end - start + 1, '\0');
        Sci_TextRange range;
        range.chrg.cpMin = start;
        range.chrg.cpMax = end;
        range.lpstrText = buffer.data();
        sci->WndProc(SCI_GETTEXTRANGE, 0, reinterpret_cast<sptr_t>(&range));
        return QString::fromUtf8(buffer.constData(), end - start);
    }
}

void ScintillaQuickItem::removePreedit()
{
    if (_preeditPos < 0)
        return;
    ScintillaQuickBackend *s = _backend.get();
    const bool recording = s->recordingMacro;
    s->recordingMacro = false;
    _backend->WndProc(SCI_SETUNDOCOLLECTION, 0, 0);
    _backend->WndProc(SCI_DELETERANGE, _preeditPos, _preeditBytes);
    _backend->WndProc(SCI_SETUNDOCOLLECTION, 1, 0);
    s->recordingMacro = recording;
    s->SetSelection(_preeditPos, _preeditPos);
    _preeditPos = -1;
    _preeditBytes = 0;
    _preeditString.clear();
}

void ScintillaQuickItem::finishComposition()
{
    if (!composing())
        return;
    QByteArray bytes = _preeditString.toUtf8();
    removePreedit();
    if (!bytes.isEmpty())
        _backend->AddCharUTF(bytes.data(), static_cast<unsigned int>(bytes.size()));
    // The input method must drop its state, or it would commit the same
    // text again (possibly into another item).
    QGuiApplication::inputMethod()->reset();
    emit composingChanged();
    update();
}

void ScintillaQuickItem::updateInputMethod()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, &ScintillaQuickItem::updateInputMethod, Qt::QueuedConnection);
        return;
    }
    if (hasActiveFocus())
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition
                                               | Qt::ImAnchorPosition | Qt::ImSurroundingText
                                               | Qt::ImCurrentSelection);
}

void ScintillaQuickItem::inputMethodEvent(QInputMethodEvent *event)
{
    if (_readOnly) {
        event->ignore();
        return;
    }
    const bool wasComposing = composing();
    if (event->commitString().isEmpty() && event->preeditString().isEmpty()
        && event->replacementLength() == 0 && !wasComposing) {
        event->accept();   // nothing to do (some platforms send these on focus changes)
        return;
    }

    ScintillaQuickBackend *s = _backend.get();
    removePreedit();

    // Committed text replaces the selection, or the characters the input
    // method names relative to the caret (e.g. Korean jamo, reconversion).
    const QString commitText = RagoShell::QtUtils::normalizeSmartQuotes(event->commitString());
    if (!commitText.isEmpty() || event->replacementLength() > 0) {
        if (event->replacementStart() != 0 || event->replacementLength() > 0) {
            int start = static_cast<int>(_backend->WndProc(SCI_GETCURRENTPOS, 0, 0));
            for (int i = 0; i < -event->replacementStart(); ++i)
                start = s->pdoc->NextPosition(start, -1);
            for (int i = 0; i < event->replacementStart(); ++i)
                start = s->pdoc->NextPosition(start, 1);
            int end = start;
            for (int i = 0; i < event->replacementLength(); ++i)
                end = s->pdoc->NextPosition(end, 1);
            s->SetSelection(end, start);
        }
        QByteArray bytes = commitText.toUtf8();
        if (bytes.isEmpty())
            s->ClearSelection();
        else
            s->AddCharUTF(bytes.data(), static_cast<unsigned int>(bytes.size()));
    }

    const QString preedit = RagoShell::QtUtils::normalizeSmartQuotes(event->preeditString());
    if (!preedit.isEmpty()) {
        s->ClearSelection();
        const int pos = static_cast<int>(_backend->WndProc(SCI_GETCURRENTPOS, 0, 0));
        QByteArray bytes = preedit.toUtf8();
        const bool recording = s->recordingMacro;
        s->recordingMacro = false;
        _insertingPreedit = true;
        _backend->WndProc(SCI_SETUNDOCOLLECTION, 0, 0);
        s->AddCharUTF(bytes.data(), static_cast<unsigned int>(bytes.size()));
        _backend->WndProc(SCI_SETUNDOCOLLECTION, 1, 0);
        _insertingPreedit = false;
        s->recordingMacro = recording;

        _preeditPos = pos;
        _preeditBytes = bytes.size();
        _preeditString = preedit;
        s->SetSelection(pos + _preeditBytes, pos + _preeditBytes);
        s->ShowCaretAtCurrentPosition();

        int indicator = kInputMethodIndicator;
        for (const QInputMethodEvent::Attribute &a : event->attributes()) {
            const int at = pos + static_cast<int>(preedit.left(a.start).toUtf8().size());
            if (a.type == QInputMethodEvent::Cursor) {
                s->SetSelection(at, at);
                if (a.length == 0)
                    s->DropCaret();
            } else if (a.type == QInputMethodEvent::TextFormat && indicator <= INDIC_MAX) {
                const QTextCharFormat format = a.value.value<QTextFormat>().toCharFormat();
                if (format.underlineStyle() == QTextCharFormat::NoUnderline)
                    continue;
                const int length = static_cast<int>(preedit.mid(a.start, a.length).toUtf8().size());
                const QColor color = format.underlineColor().isValid()
                                         ? format.underlineColor()
                                         : (_darkMode ? QColor(Qt::white) : QColor(Qt::black));
                _backend->WndProc(SCI_INDICSETSTYLE, indicator,
                                  format.underlineStyle() == QTextCharFormat::DashUnderline
                                      ? INDIC_DASH : INDIC_COMPOSITIONTHICK);
                _backend->WndProc(SCI_INDICSETFORE, indicator,
                                  color.red() | (color.green() << 8) | (color.blue() << 16));
                _backend->WndProc(SCI_SETINDICATORCURRENT, indicator, 0);
                _backend->WndProc(SCI_INDICATORFILLRANGE, at, length);
                ++indicator;
            }
        }
    }

    if (wasComposing != composing())
        emit composingChanged();
    updateInputMethod();
    event->accept();
    update();
}

QVariant ScintillaQuickItem::inputMethodQuery(Qt::InputMethodQuery query) const
{
    ScintillaQuickBackend *s = _backend.get();
    // While composing, Qt expects positions and text without the composition.
    const int pos = composing() ? _preeditPos
                                : static_cast<int>(s->WndProc(SCI_GETCURRENTPOS, 0, 0));
    switch (query) {
    case Qt::ImEnabled:
        return !_readOnly;
    case Qt::ImHints:
        return static_cast<int>(Qt::ImhMultiLine | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    case Qt::ImFont:
        return _font;
    case Qt::ImCursorRectangle: {
        const int line = static_cast<int>(s->WndProc(SCI_LINEFROMPOSITION, pos, 0));
        const qreal x = s->WndProc(SCI_POINTXFROMPOSITION, 0, pos);
        const qreal y = s->WndProc(SCI_POINTYFROMPOSITION, 0, pos);
        const qreal h = s->WndProc(SCI_TEXTHEIGHT, line, 0);
        const qreal w = std::max<qreal>(1, s->WndProc(SCI_GETCARETWIDTH, 0, 0));
        return QRectF(x, y, w, h);
    }
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
    case Qt::ImSurroundingText:
    case Qt::ImTextBeforeCursor:
    case Qt::ImTextAfterCursor: {
        const int line = static_cast<int>(s->WndProc(SCI_LINEFROMPOSITION, pos, 0));
        const int lineStart = static_cast<int>(s->WndProc(SCI_POSITIONFROMLINE, line, 0));
        const int lineEnd = static_cast<int>(s->WndProc(SCI_GETLINEENDPOSITION, line, 0));
        const QString before = utf8Range(s, lineStart, pos);
        const int afterStart = composing() ? pos + _preeditBytes : pos;
        const QString after = utf8Range(s, afterStart, std::max(afterStart, lineEnd));
        if (query == Qt::ImTextBeforeCursor)
            return before;
        if (query == Qt::ImTextAfterCursor)
            return after;
        if (query == Qt::ImSurroundingText)
            return before + after;
        if (query == Qt::ImCursorPosition || composing())
            return static_cast<int>(before.size());
        const int anchor = std::clamp(static_cast<int>(s->WndProc(SCI_GETANCHOR, 0, 0)),
                                      lineStart, lineEnd);
        return anchor <= pos
                   ? static_cast<int>(utf8Range(s, lineStart, anchor).size())
                   : static_cast<int>(before.size() + utf8Range(s, pos, anchor).size());
    }
    case Qt::ImCurrentSelection:
        return composing() ? QString() : selectedText();
    default:
        return QQuickPaintedItem::inputMethodQuery(query);
    }
}

int ScintillaQuickItem::bytePosBeforeCaret(int pos, int utf16CharsBack) const
{
    if (utf16CharsBack <= 0 || pos <= 0)
        return std::max(0, pos);
    ScintillaQuickBackend *s = _backend.get();
    int scanStart = pos;
    for (int i = 0; i < utf16CharsBack && scanStart > 0; ++i)
        scanStart = s->pdoc->NextPosition(scanStart, -1);
    const QString chunk = utf8Range(s, scanStart, pos);
    if (chunk.size() <= utf16CharsBack)
        return scanStart;
    const int keepUnits = static_cast<int>(chunk.size()) - utf16CharsBack;
    return scanStart + static_cast<int>(chunk.left(keepUnits).toUtf8().size());
}

QString ScintillaQuickItem::textBeforeCaret(int maxChars)
{
    ScintillaQuickBackend *s = _backend.get();
    const int pos = static_cast<int>(s->WndProc(SCI_GETCURRENTPOS, 0, 0));
    int start = pos;
    for (int i = 0; i < maxChars && start > 0; ++i)
        start = s->pdoc->NextPosition(start, -1);
    return utf8Range(s, start, pos);
}

QString ScintillaQuickItem::textAfterCaret(int maxChars)
{
    if (maxChars <= 0)
        return {};
    ScintillaQuickBackend *s = _backend.get();
    const int docLen = static_cast<int>(s->WndProc(SCI_GETLENGTH, 0, 0));
    const int pos = static_cast<int>(s->WndProc(SCI_GETCURRENTPOS, 0, 0));
    int end = pos;
    for (int i = 0; i < maxChars && end < docLen; ++i)
        end = s->pdoc->NextPosition(end, 1);
    return utf8Range(s, pos, end);
}

QPointF ScintillaQuickItem::pointBeforeCaret(int utf16CharsBack)
{
    ScintillaQuickBackend *s = _backend.get();
    const int pos = static_cast<int>(s->WndProc(SCI_GETCURRENTPOS, 0, 0));
    return pointFromPosition(bytePosBeforeCaret(pos, utf16CharsBack));
}

void ScintillaQuickItem::replaceBeforeCaret(int utf16CharsBack, const QString &insertText,
                                            int caretOffsetFromEnd)
{
    if (_readOnly)
        return;
    finishComposition();
    ScintillaQuickBackend *s = _backend.get();
    const int pos = static_cast<int>(s->WndProc(SCI_GETCURRENTPOS, 0, 0));
    const int startPos = bytePosBeforeCaret(pos, utf16CharsBack);
    const QByteArray utf8 = insertText.toUtf8();

    s->WndProc(SCI_BEGINUNDOACTION, 0, 0);
    s->WndProc(SCI_SETTARGETSTART, startPos, 0);
    s->WndProc(SCI_SETTARGETEND, pos, 0);
    s->WndProc(SCI_REPLACETARGET, static_cast<uptr_t>(utf8.size()),
               reinterpret_cast<sptr_t>(utf8.constData()));
    s->WndProc(SCI_ENDUNDOACTION, 0, 0);

    int caretUnits = static_cast<int>(insertText.size()) + caretOffsetFromEnd;
    caretUnits = std::clamp(caretUnits, 0, static_cast<int>(insertText.size()));
    const int targetBytePos = startPos + static_cast<int>(insertText.left(caretUnits).toUtf8().size());
    s->WndProc(SCI_GOTOPOS, targetBytePos, 0);
    s->WndProc(SCI_SCROLLCARET, 0, 0);
    updateBraceHighlight();
    updateInputMethod();
    update();
}

QVariantMap ScintillaQuickItem::shellCallTip()
{
    constexpr int kLookBackChars = 4000;
    ScintillaQuickBackend *s = _backend.get();
    const int pos = static_cast<int>(s->WndProc(SCI_GETCURRENTPOS, 0, 0));
    int start = pos;
    for (int i = 0; i < kLookBackChars && start > 0; ++i)
        start = s->pdoc->NextPosition(start, -1);
    const QString text = utf8Range(s, start, pos);
    const auto tip = RagoShell::ShellSignatures::lookup(text);
    if (!tip.found)
        return {};
    // parenOffset counts UTF-16 units; Scintilla positions are UTF-8 bytes.
    const int parenPos = start + static_cast<int>(text.left(text.size() - tip.parenOffset).toUtf8().size());
    return {
        {QStringLiteral("name"), tip.name},
        {QStringLiteral("params"), tip.params},
        {QStringLiteral("active"), tip.activeParam},
        {QStringLiteral("x"), static_cast<qreal>(s->WndProc(SCI_POINTXFROMPOSITION, 0, parenPos))},
        {QStringLiteral("y"), static_cast<qreal>(s->WndProc(SCI_POINTYFROMPOSITION, 0, parenPos))},
    };
}
