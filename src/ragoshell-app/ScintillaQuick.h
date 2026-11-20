#pragma once

#include <QPointF>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>
#include <QTimer>
#include <memory>

class ScintillaQuickBackend;

/**
 * @brief QML item hosting a Scintilla editor surface.
 *
 * Owns a ScintillaBase subclass (ScintillaQuickBackend) that renders through
 * QPainter into the item's scene-graph node via QQuickPaintedItem.
 */
class ScintillaQuickItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(ScintillaEditor)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged FINAL)
    Q_PROPERTY(int textLength READ textLength NOTIFY textChanged FINAL)
    /// True when the document differs from the last setSavePoint() (undo back
    /// to that point clears it). O(1), unlike comparing `text`.
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged FINAL)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int caretPosition READ caretPosition NOTIFY caretPositionChanged FINAL)
    Q_PROPERTY(int scrollMaximum READ scrollMaximum NOTIFY scrollChanged FINAL)
    Q_PROPERTY(int scrollValue READ scrollValue NOTIFY scrollChanged FINAL)
    Q_PROPERTY(int scrollPage READ scrollPage NOTIFY scrollChanged FINAL)
    /// Horizontal scroll in pixels (maximum = 0 when all lines fit).
    Q_PROPERTY(int horizontalScrollMaximum READ horizontalScrollMaximum NOTIFY horizontalScrollChanged FINAL)
    Q_PROPERTY(int horizontalScrollValue READ horizontalScrollValue NOTIFY horizontalScrollChanged FINAL)
    Q_PROPERTY(int horizontalScrollPage READ horizontalScrollPage NOTIFY horizontalScrollChanged FINAL)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged FINAL)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged FINAL)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontChanged FINAL)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged FINAL)
    Q_PROPERTY(bool lineNumbers READ lineNumbers WRITE setLineNumbers NOTIFY lineNumbersChanged FINAL)
    Q_PROPERTY(bool readOnly READ isReadOnly WRITE setReadOnly NOTIFY readOnlyChanged FINAL)
    Q_PROPERTY(bool folding READ folding WRITE setFolding NOTIFY foldingChanged FINAL)
    /// When a QML overlay (autocomplete list) is open, navigation keys
    /// are routed to overlayKeyPressed instead of the document.
    Q_PROPERTY(bool overlayActive READ overlayActive WRITE setOverlayActive
               NOTIFY overlayActiveChanged FINAL)
    /// True while an input method (Chinese, Japanese, Korean IME) shows its
    /// uncommitted composition inline in the document.
    Q_PROPERTY(bool composing READ composing NOTIFY composingChanged FINAL)

public:
    explicit ScintillaQuickItem(QQuickItem *parent = nullptr);
    ~ScintillaQuickItem() override;

    QString text() const;
    void setText(const QString &text);
    int textLength() const;

    bool isModified() const;
    Q_INVOKABLE void setSavePoint();

    QString selectedText() const;
    bool hasSelection() const;

    int caretPosition() const;
    int scrollMaximum() const;
    int scrollValue() const;
    int scrollPage() const;
    int horizontalScrollMaximum() const { return _hScrollMax; }
    int horizontalScrollValue() const { return _hScrollValue; }
    int horizontalScrollPage() const { return _hScrollPage; }

    QFont font() const;
    void setFont(const QFont &font);

    QString fontFamily() const;
    void setFontFamily(const QString &family);

    int fontSize() const { return _fontPixelSize; }
    void setFontSize(int pixelSize);

    bool darkMode() const { return _darkMode; }
    void setDarkMode(bool dark);

    bool lineNumbers() const { return _lineNumbers; }
    void setLineNumbers(bool show);

    bool isReadOnly() const { return _readOnly; }
    Q_INVOKABLE void setReadOnly(bool readOnly);

    bool folding() const { return _folding; }
    void setFolding(bool enable);

    bool overlayActive() const { return _overlayActive; }
    void setOverlayActive(bool active) {
        if (_overlayActive == active)
            return;
        _overlayActive = active;
        emit overlayActiveChanged();
    }

    bool composing() const { return _preeditPos >= 0; }
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

    /// Direct Scintilla SCI_* message access for advanced use.
    Q_INVOKABLE long send(unsigned int msg, unsigned long wParam = 0,
                          long lParam = 0);
    Q_INVOKABLE void scrollTo(int line);
    /// Scroll to a horizontal pixel offset (0..horizontalScrollMaximum).
    Q_INVOKABLE void scrollHorizontallyTo(int x);
    /// Pixel coordinates of a document position, in item space — used
    /// to anchor QML overlays (autocomplete list) at the caret.
    Q_INVOKABLE QPointF pointFromPosition(int pos);
    /// Height of one text line in pixels (for overlay placement).
    Q_INVOKABLE int lineHeight();
    /// Mongo shell call tip at the caret (see ShellSignatures):
    /// { name, params, active, x, y } with x / y at the call's '(' in item
    /// coordinates, or an empty map outside a known call.
    Q_INVOKABLE QVariantMap shellCallTip();
    /// Up to maxChars characters before the caret (no full-document read).
    Q_INVOKABLE QString textBeforeCaret(int maxChars);
    /// Up to maxChars characters after the caret (no full-document read).
    Q_INVOKABLE QString textAfterCaret(int maxChars);
    /// Pixel coordinates utf16CharsBack UTF-16 units before the caret, in item space.
    Q_INVOKABLE QPointF pointBeforeCaret(int utf16CharsBack);
    /// Atomically replace utf16CharsBack UTF-16 units before the caret as one undo step.
    Q_INVOKABLE void replaceBeforeCaret(int utf16CharsBack, const QString &insertText,
                                        int caretOffsetFromEnd = 0);

    // Standard editing & navigation helpers
    Q_INVOKABLE void setCaretPositionUtf16(int utf16Offset);
    Q_INVOKABLE void cut();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void replaceAllText(const QString &newText);
    Q_INVOKABLE void toggleLineComment();
    Q_INVOKABLE bool findText(const QString &query, bool forward = true, bool matchCase = false);
    Q_INVOKABLE void foldAll(bool expand);

    void paint(QPainter *painter) override;

    /// Called by the backend whenever Scintilla content changes.
    void requestUpdate();
    /// Called by the backend when Scintilla scroll range/position changes.
    void updateScroll(int maximum, int value, int page);
    void updateHorizontalScroll(int maximum, int value, int page);

Q_SIGNALS:
    void textChanged();
    void modifiedChanged();
    void selectionChanged();
    void caretPositionChanged();
    void scrollChanged();
    void horizontalScrollChanged();
    void fontChanged();
    /// SCNotification bridge — QML/C++ subscribers get Scintilla events.
    void notified(int code, int position);
    void charAdded(int ch);
    /// Ctrl+Space pressed — QML shows the autocomplete overlay.
    void autocompleteRequested();
    void findRequested();
    void executeRequested();
    void formatRequested();
    void contextMenuRequested(qreal x, qreal y);
    /// Navigation key while overlayActive (Qt::Key_* value as int).
    void overlayKeyPressed(int key);
    void overlayActiveChanged();
    void darkModeChanged();
    void lineNumbersChanged();
    void readOnlyChanged();
    void foldingChanged();
    void composingChanged();

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry,
                        const QRectF &oldGeometry) override;

private:
    void applyTheme();
    void updateBraceHighlight();
    void syncTickingState();
    /// Removes the inline composition (never part of the undo history).
    void removePreedit();
    /// Keeps a composition the input method never committed (click, focus
    /// loss) as ordinary, undoable text.
    void finishComposition();
    /// Tells the input method that the caret / surrounding text moved.
    void updateInputMethod();
    int bytePosBeforeCaret(int pos, int utf16CharsBack) const;
    ScintillaQuickBackend *sci() const;
    int _scrollMax = 0;
    int _scrollPage = 0;
    int _scrollValue = 0;
    int _hScrollMax = 0;
    int _hScrollPage = 0;
    int _hScrollValue = 0;
    int _wheelAccumY = 0;
    int _wheelAccumX = 0;
    int _lastSelStart = 0;
    int _lastSelEnd = 0;
    bool _overlayActive = false;
    bool _darkMode = true;
    bool _lineNumbers = true;
    bool _readOnly = false;
    bool _folding = true;
    bool _largeDocumentMode = false;
    int _preeditPos = -1;       ///< byte position of the inline composition
    int _preeditBytes = 0;
    QString _preeditString;
    bool _insertingPreedit = false;
    int _fontPixelSize = 13;
    QFont _font;
    std::unique_ptr<ScintillaQuickBackend> _backend;
    friend class ScintillaQuickBackend;
};
