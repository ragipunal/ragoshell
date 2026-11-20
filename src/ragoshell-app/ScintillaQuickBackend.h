#pragma once

#include "ragoshell-app/ScintillaQuick.h"

#include <QGuiApplication>
#include <QClipboard>
#include <QDateTime>
#include <QImage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QtMath>
#include <QWheelEvent>
#include <QPainter>
#include <QPointer>
#include <QTextCharFormat>
#include <QThread>
#include <QWidget>

// Scintilla core headers (bundled in qscintilla-2.8.4). Same include order
// as ScintillaQt.h — the headers don't manage their own dependencies.
#include <algorithm>
#include <cstring>
#include "ILexer.h"
#include "Platform.h"
#include "Scintilla.h"
#include "SciLexer.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "CellBuffer.h"
#include "CharClassify.h"
#include "RunStyles.h"
#include "CaseFolder.h"
#include "Decoration.h"
#include "Document.h"
#include "Style.h"
#include "XPM.h"
#include "LineMarker.h"
#include "Indicator.h"
#include "ViewStyle.h"
#include "KeyMap.h"
#include "ContractionState.h"
#include "Selection.h"
#include "PositionCache.h"
#include "Editor.h"
#include "AutoComplete.h"
#include "CallTip.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "ScintillaBase.h"

#include "ragoshell/core/utils/ShellSignatures.h"

/**
 * @brief ScintillaBase subclass bound to a QML item.
 */
class ScintillaQuickBackend : public ScintillaBase
{
public:
    explicit ScintillaQuickBackend(ScintillaQuickItem *item) : _item(item)
    {
        displayPopupMenu = false;
        _host = new QWidget();
        _host->setAttribute(Qt::WA_DontShowOnScreen);
        wMain = _host;

        QObject::connect(&_tickTimer, &QTimer::timeout, _item, [this]() {
            if (!_item->isVisible() || (!_item->hasActiveFocus() && !_capturedMouse)) {
                _tickTimer.stop();
                timer.ticking = false;
                return;
            }
            const bool prevCaretOn = caret.on;
            const int prevTopLine = topLine;
            Tick();
            if (caret.on != prevCaretOn || topLine != prevTopLine || _capturedMouse)
                _item->requestUpdate();
        });
        Initialise();
    }

    ~ScintillaQuickBackend() override
    {
        wMain = 0;
        delete _host;
    }

    // ---- platform hooks ----------------------------------------------------
    void Initialise() override { SetTicking(false); }
    void Finalise() override { SetTicking(false); ScintillaBase::Finalise(); }

    void CreateCallTipWindow(PRectangle) override {}
    void AddToPopUp(const char *, int, bool) override {}

    void SetVerticalScrollPos() override
    {
        _item->updateScroll(_scrollMax, topLine, _scrollPage);
    }
    void SetHorizontalScrollPos() override { updateHorizontalScroll(); }
    bool ModifyScrollBars(int nMax, int nPage) override
    {
        _scrollMax = nMax - nPage + 1;
        _scrollPage = nPage;
        _item->updateScroll(_scrollMax, topLine, _scrollPage);
        updateHorizontalScroll();
        return false;
    }
    // Horizontal range in pixels: scrollWidth (longest line, tracked) minus
    // the visible text width; 0 when everything fits or lines wrap.
    void updateHorizontalScroll()
    {
        const int pageWidth = std::max(1, static_cast<int>(GetTextRectangle().Width()));
        const int maximum = Wrapping() ? 0 : std::max(0, scrollWidth - pageWidth);
        _item->updateHorizontalScroll(maximum, xOffset, pageWidth);
    }

    void Copy() override
    {
        if (!sel.Empty()) {
            SelectionText text;
            CopySelectionRange(&text);
            CopyToClipboard(text);
        }
    }
    void Paste() override
    {
        if (pdoc->IsReadOnly())
            return;
        QString text = QGuiApplication::clipboard()->text();
        if (text.isEmpty())
            return;
        QByteArray bytes = IsUnicodeMode() ? text.toUtf8() : text.toLatin1();
        if (bytes.isEmpty())
            return;

        std::string dest = Document::TransformLineEnds(bytes.constData(),
                                                       bytes.size(), pdoc->eolMode);
        UndoGroup ug(pdoc);
        ClearSelection();
        SelectionPosition start = sel.IsRectangular()
            ? sel.Rectangular().Start() : sel.Range(sel.Main()).Start();
        InsertPaste(start, dest.data(), static_cast<int>(dest.size()));
        NotifyChange();
        Redraw();
    }
    void CopyToClipboard(const SelectionText &selectedText) override
    {
        QString text = IsUnicodeMode()
            ? QString::fromUtf8(selectedText.Data(), selectedText.Length())
            : QString::fromLatin1(selectedText.Data(), selectedText.Length());
        QGuiApplication::clipboard()->setText(text);
    }
    void ClaimSelection() override {}

    void NotifyChange() override
    {
        if (!_item)
            return;
        if (QThread::currentThread() != _item->thread()) {
            QPointer<ScintillaQuickItem> itemPtr(_item);
            QMetaObject::invokeMethod(_item, [itemPtr]() {
                if (itemPtr)
                    emit itemPtr->textChanged();
            }, Qt::QueuedConnection);
            return;
        }
        emit _item->textChanged();
    }

    void NotifyParent(SCNotification scn) override
    {
        if (!_item)
            return;

        // If called from the scene graph render thread (e.g. during Editor::Paint -> NotifyUpdateUI),
        // we must not mutate QML state, emit QML signals, or invoke GUI-thread APIs directly.
        // Queue the notification to be processed on the main GUI thread.
        if (QThread::currentThread() != _item->thread()) {
            QPointer<ScintillaQuickItem> itemPtr(_item);
            QMetaObject::invokeMethod(_item, [this, itemPtr, scn]() {
                if (itemPtr)
                    NotifyParent(scn);
            }, Qt::QueuedConnection);
            return;
        }

        emit _item->notified(scn.nmhdr.code, scn.position);
        if (scn.nmhdr.code == SCN_CHARADDED) {
            // An uncommitted IME composition is not typing (no autocomplete).
            if (!_item->_insertingPreedit)
                emit _item->charAdded(scn.ch);
        } else if (scn.nmhdr.code == SCN_SAVEPOINTREACHED ||
                   scn.nmhdr.code == SCN_SAVEPOINTLEFT) {
            emit _item->modifiedChanged();
        } else if (scn.nmhdr.code == SCN_MARGINCLICK && scn.margin == 2) {
            const int line = static_cast<int>(WndProc(SCI_LINEFROMPOSITION, scn.position, 0));
            const int level = static_cast<int>(WndProc(SCI_GETFOLDLEVEL, line, 0));
            if (level & SC_FOLDLEVELHEADERFLAG)
                WndProc(SCI_TOGGLEFOLD, line, 0);
        } else if (scn.nmhdr.code == SCN_UPDATEUI) {
            _item->updateBraceHighlight();
            const int s0 = static_cast<int>(WndProc(SCI_GETSELECTIONSTART, 0, 0));
            const int s1 = static_cast<int>(WndProc(SCI_GETSELECTIONEND, 0, 0));
            if (s0 != _item->_lastSelStart || s1 != _item->_lastSelEnd) {
                _item->_lastSelStart = s0;
                _item->_lastSelEnd = s1;
                emit _item->selectionChanged();
            }
            emit _item->caretPositionChanged();
            _item->updateInputMethod();
        }
        _item->requestUpdate();
    }

    void SetTicking(bool on) override
    {
        const bool shouldRun = on && _item && _item->isVisible() &&
                               ((_item->hasActiveFocus() && !_item->isReadOnly()) || _capturedMouse);
        if (timer.ticking != shouldRun) {
            timer.ticking = shouldRun;
            if (timer.ticking)
                _tickTimer.start(timer.tickSize);
            else
                _tickTimer.stop();
        }
        timer.ticksToWait = caret.period;
    }
    void SetMouseCapture(bool on) override
    {
        if (mouseDownCaptures) {
            if (on) _item->grabMouse();
            else _item->ungrabMouse();
        }
        _capturedMouse = on;
        SetTicking(on || (_item && _item->hasActiveFocus()));
    }
    bool HaveMouseCapture() override { return _capturedMouse; }

    sptr_t DefWndProc(unsigned int, uptr_t, sptr_t) override { return 0; }

    PRectangle GetClientRectangle() override
    {
        return PRectangle(0, 0, _item->width(), _item->height());
    }

    void paintInto(QPainter *painter, const QRectF &bounds)
    {
        if (bounds.width() <= 0.0 || bounds.height() <= 0.0)
            return;

        qreal dpr = 1.0;
        if (const QPaintDevice *dev = painter->device())
            dpr = dev->devicePixelRatioF();
        if (dpr <= 0.0 && _item->window())
            dpr = _item->window()->effectiveDevicePixelRatio();
        if (dpr <= 0.0)
            dpr = 1.0;

        // QQuickPaintedItem backs its texture with Format_ARGB32_Premultiplied,
        // which causes Qt's raster engine to disable ClearType LCD subpixel
        // antialiasing. Painting onto an opaque Format_RGB32 surface at exact
        // native device pixels activates true ClearType subpixel glyph rendering.
        const int pxW = std::max(1, qCeil(bounds.width() * dpr));
        const int pxH = std::max(1, qCeil(bounds.height() * dpr));
        if (_rgbSurface.width() != pxW || _rgbSurface.height() != pxH ||
            _rgbSurface.format() != QImage::Format_RGB32) {
            _rgbSurface = QImage(pxW, pxH, QImage::Format_RGB32);
        }
        _rgbSurface.setDevicePixelRatio(dpr);
        const int dpm = qRound((96.0 * dpr) / 0.0254);
        _rgbSurface.setDotsPerMeterX(dpm);
        _rgbSurface.setDotsPerMeterY(dpm);

        Surface *sw = Surface::Allocate(SC_TECHNOLOGY_DEFAULT);
        if (!sw)
            return;

        paintState = painting;

        rcPaint.left = bounds.left();
        rcPaint.top = bounds.top();
        rcPaint.right = bounds.right() + 1;
        rcPaint.bottom = bounds.bottom() + 1;

        PRectangle rcText = GetTextRectangle();
        paintingAllText = rcPaint.Contains(rcText);

        QPainter rgbPainter(&_rgbSurface);
        rgbPainter.setRenderHint(QPainter::TextAntialiasing, true);

        sw->Init(&rgbPainter);
        sw->SetUnicodeMode(CodePage() == SC_CP_UTF8);
        Paint(sw, rcPaint);

        delete sw;
        rgbPainter.end();
        paintState = notPainting;

        painter->drawImage(QPointF(0.0, 0.0), _rgbSurface);
    }

    ScintillaQuickItem *_item;
    QWidget *_host = nullptr;
    QImage _rgbSurface;
    QTimer _tickTimer;
    bool _capturedMouse = false;
    int _scrollMax = 0;
    int _scrollPage = 0;

    friend class ScintillaQuickItem;
};

