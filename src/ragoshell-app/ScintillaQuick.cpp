#include "ragoshell-app/ScintillaQuickBackend.h"

#include <QFontDatabase>

ScintillaQuickItem::ScintillaQuickItem(QQuickItem *parent)
    : QQuickPaintedItem(parent),
      _font(QFontDatabase::systemFont(QFontDatabase::FixedFont)),
      _backend(new ScintillaQuickBackend(this))
{
    _font.setPixelSize(_fontPixelSize);
    setFlag(QQuickItem::ItemHasContents, true);
    setOpaquePainting(true);
    setFillColor(QColor(15, 22, 18));
    setRenderTarget(QQuickPaintedItem::Image);
    setAcceptedMouseButtons(Qt::AllButtons);
    setActiveFocusOnTab(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);

    _backend->WndProc(SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    // QQuickPaintedItem already renders into an off-screen High-DPI QImage;
    // disabling Scintilla's internal QPixmap line double-buffer avoids per-line
    // QPixmap allocations and keeps glyphs razor-sharp at 125% / 150% DPI scaling.
    _backend->WndProc(SCI_SETBUFFEREDDRAW, 0, 0);
    _backend->WndProc(SCI_SETLEXER, SCLEX_CPP, 0);
    _backend->WndProc(SCI_SETSCROLLWIDTH, 1, 0);
    _backend->WndProc(SCI_SETSCROLLWIDTHTRACKING, 1, 0);
    _backend->WndProc(SCI_SETTABWIDTH, 4, 0);
    _backend->WndProc(SCI_SETUSETABS, 0, 0);

    // Enable brace/block folding in SCLEX_CPP
    _backend->WndProc(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"),
                      reinterpret_cast<sptr_t>("1"));
    _backend->WndProc(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.compact"),
                      reinterpret_cast<sptr_t>("0"));
    _backend->WndProc(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold.comment"),
                      reinterpret_cast<sptr_t>("1"));
    _backend->WndProc(SCI_SETAUTOMATICFOLD,
                      SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE, 0);

    static const char *jsKeywords =
        "async await break case catch class const continue debugger default "
        "delete do else export extends false finally for function if import "
        "in instanceof let new null of return super switch this throw true "
        "try typeof undefined var void while with yield";
    static const char *mongoKeywords =
        "db rs sh ObjectId ObjectID ISODate NumberLong NumberInt NumberDecimal "
        "Timestamp BinData HexData UUID RegExp DBRef MinKey MaxKey print printjson "
        "find findOne findAndModify findOneAndUpdate findOneAndDelete "
        "findOneAndReplace insert insertOne insertMany save update updateOne "
        "updateMany replaceOne deleteOne deleteMany remove aggregate count "
        "countDocuments estimatedDocumentCount distinct createIndex ensureIndex "
        "createIndexes dropIndex dropIndexes getIndexes reIndex drop dropDatabase "
        "createCollection renameCollection stats storageSize totalIndexSize dataSize "
        "totalSize serverStatus hostInfo version currentOp killOp printCollectionStats "
        "getCollection getCollectionNames getCollectionInfos getUsers getUser "
        "createUser updateUser dropUser getRoles runCommand adminCommand "
        "sort limit skip batchSize hint explain pretty toArray forEach map";
    _backend->WndProc(SCI_SETKEYWORDS, 0, reinterpret_cast<sptr_t>(jsKeywords));
    _backend->WndProc(SCI_SETKEYWORDS, 1, reinterpret_cast<sptr_t>(mongoKeywords));

    applyTheme();
    _backend->WndProc(SCI_SETFOCUS, 0, 0);
}

ScintillaQuickItem::~ScintillaQuickItem() = default;

ScintillaQuickBackend *ScintillaQuickItem::sci() const { return _backend.get(); }

void ScintillaQuickItem::requestUpdate()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
        return;
    }
    update();
}

void ScintillaQuickItem::updateScroll(int maximum, int value, int page)
{
    if (QThread::currentThread() != thread()) {
        QPointer<ScintillaQuickItem> self(this);
        QMetaObject::invokeMethod(this, [self, maximum, value, page]() {
            if (self)
                self->updateScroll(maximum, value, page);
        }, Qt::QueuedConnection);
        return;
    }
    if (_scrollMax == maximum && _scrollValue == value && _scrollPage == page)
        return;
    _scrollMax = maximum;
    _scrollValue = value;
    _scrollPage = page;
    emit scrollChanged();
}

void ScintillaQuickItem::updateHorizontalScroll(int maximum, int value, int page)
{
    if (QThread::currentThread() != thread()) {
        QPointer<ScintillaQuickItem> self(this);
        QMetaObject::invokeMethod(this, [self, maximum, value, page]() {
            if (self)
                self->updateHorizontalScroll(maximum, value, page);
        }, Qt::QueuedConnection);
        return;
    }
    if (_hScrollMax == maximum && _hScrollValue == value && _hScrollPage == page)
        return;
    _hScrollMax = maximum;
    _hScrollValue = value;
    _hScrollPage = page;
    emit horizontalScrollChanged();
}

void ScintillaQuickItem::scrollHorizontallyTo(int x)
{
    // SCI_SETXOFFSET redraws and calls SetHorizontalScrollPos().
    _backend->WndProc(SCI_SETXOFFSET, std::clamp(x, 0, std::max(0, _hScrollMax)), 0);
    update();
}

int ScintillaQuickItem::textLength() const
{
    return static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
}

bool ScintillaQuickItem::isModified() const
{
    return _backend->WndProc(SCI_GETMODIFY, 0, 0) != 0;
}

void ScintillaQuickItem::setSavePoint()
{
    // Emits SCN_SAVEPOINTREACHED -> modifiedChanged when it was modified.
    _backend->WndProc(SCI_SETSAVEPOINT, 0, 0);
}

QString ScintillaQuickItem::text() const
{
    int len = static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
    if (len <= 0)
        return QString();
    QByteArray buf(len + 1, '\0');
    _backend->WndProc(SCI_GETTEXT, len + 1,
                      reinterpret_cast<sptr_t>(buf.data()));
    return QString::fromUtf8(buf.constData(), len);
}

void ScintillaQuickItem::setText(const QString &text)
{
    const int curLen = static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
    if (curLen == 0 && text.isEmpty())
        return;

    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() == curLen) {
        const char *existing = reinterpret_cast<const char *>(
            _backend->WndProc(SCI_GETCHARACTERPOINTER, 0, 0));
        if (existing && (curLen == 0 || std::memcmp(existing, utf8.constData(), static_cast<size_t>(curLen)) == 0))
            return;
    }

    static constexpr int kLargeDocThresholdBytes = 128 * 1024;
    static constexpr int kInitialLexViewportBytes = 32 * 1024;
    const bool isLarge = utf8.size() > kLargeDocThresholdBytes;

    if (isLarge != _largeDocumentMode) {
        _largeDocumentMode = isLarge;
        _backend->WndProc(SCI_SETSCROLLWIDTHTRACKING, isLarge ? 0 : 1, 0);
        if (isLarge)
            _backend->WndProc(SCI_SETSCROLLWIDTH, 2400, 0);
        _backend->WndProc(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"),
                          reinterpret_cast<sptr_t>(isLarge ? "0" : "1"));
        const bool showFoldMargin = _folding && !isLarge;
        _backend->WndProc(SCI_SETMARGINWIDTHN, 2, showFoldMargin ? 14 : 0);
        _backend->WndProc(SCI_SETMARGINSENSITIVEN, 2, showFoldMargin ? 1 : 0);
    }

    if (!isLarge)
        _backend->WndProc(SCI_SETSCROLLWIDTH, 1, 0);
    if (_readOnly)
        _backend->WndProc(SCI_SETREADONLY, 0, 0);
    _backend->WndProc(SCI_SETTEXT, 0,
                      reinterpret_cast<sptr_t>(utf8.constData()));
    _backend->WndProc(SCI_EMPTYUNDOBUFFER, 0, 0);
    if (isLarge) {
        // Only colourise the initial viewport; Scintilla lexes visible lines on scroll.
        _backend->WndProc(SCI_COLOURISE, 0, std::min<int>(utf8.size(), kInitialLexViewportBytes));
    } else {
        _backend->WndProc(SCI_COLOURISE, 0, -1);
    }
    if (_readOnly)
        _backend->WndProc(SCI_SETREADONLY, 1, 0);
    emit textChanged();
    update();
}

QString ScintillaQuickItem::selectedText() const
{
    const int s0 = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONSTART, 0, 0));
    const int s1 = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONEND, 0, 0));
    if (s1 <= s0)
        return QString();
    QByteArray buf(s1 - s0 + 1, '\0');
    TextRange tr;
    tr.chrg.cpMin = s0;
    tr.chrg.cpMax = s1;
    tr.lpstrText = buf.data();
    _backend->WndProc(SCI_GETTEXTRANGE, 0, reinterpret_cast<sptr_t>(&tr));
    return QString::fromUtf8(buf.constData(), s1 - s0);
}

bool ScintillaQuickItem::hasSelection() const
{
    const int s0 = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONSTART, 0, 0));
    const int s1 = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONEND, 0, 0));
    return s1 > s0;
}

int ScintillaQuickItem::caretPosition() const
{
    return static_cast<int>(_backend->WndProc(SCI_GETCURRENTPOS, 0, 0));
}

int ScintillaQuickItem::scrollMaximum() const { return _scrollMax; }
int ScintillaQuickItem::scrollValue() const { return _scrollValue; }
int ScintillaQuickItem::scrollPage() const { return _scrollPage; }

QFont ScintillaQuickItem::font() const { return _font; }

void ScintillaQuickItem::setFont(const QFont &font)
{
    if (_font == font)
        return;
    _font = font;
    if (_font.pixelSize() > 0)
        _fontPixelSize = _font.pixelSize();
    else if (_font.pointSizeF() > 0.0)
        _fontPixelSize = qRound(_font.pointSizeF() * 96.0 / 72.0);
    _font.setPixelSize(_fontPixelSize);
    applyTheme();
    _backend->ChangeSize();
    emit fontChanged();
    update();
}

QString ScintillaQuickItem::fontFamily() const
{
    return _font.family();
}

void ScintillaQuickItem::setFontFamily(const QString &family)
{
    const QString trimmed = family.trimmed();
    if (trimmed.isEmpty() || _font.family().compare(trimmed, Qt::CaseInsensitive) == 0)
        return;
    _font.setFamily(trimmed);
    _font.setPixelSize(_fontPixelSize);
    applyTheme();
    _backend->ChangeSize();
    emit fontChanged();
    update();
}

void ScintillaQuickItem::setFontSize(int pixelSize)
{
    const int clamped = std::clamp(pixelSize, 10, 28);
    if (_fontPixelSize == clamped && _font.pixelSize() == clamped)
        return;
    _fontPixelSize = clamped;
    _font.setPixelSize(clamped);
    applyTheme();
    _backend->ChangeSize();
    emit fontChanged();
    update();
}

void ScintillaQuickItem::setDarkMode(bool dark)
{
    if (_darkMode == dark)
        return;
    _darkMode = dark;
    applyTheme();
    emit darkModeChanged();
    update();
}

void ScintillaQuickItem::setLineNumbers(bool show)
{
    if (_lineNumbers == show)
        return;
    _lineNumbers = show;
    const int numWidth = _lineNumbers
        ? static_cast<int>(_backend->WndProc(SCI_TEXTWIDTH, STYLE_LINENUMBER,
                                             reinterpret_cast<sptr_t>("_9999")))
        : 0;
    _backend->WndProc(SCI_SETMARGINWIDTHN, 0, numWidth);
    emit lineNumbersChanged();
    update();
}

void ScintillaQuickItem::setReadOnly(bool readOnly)
{
    if (_readOnly == readOnly) {
        _backend->WndProc(SCI_SETREADONLY, readOnly ? 1 : 0, 0);
        return;
    }
    if (readOnly && composing()) {
        removePreedit();
        QGuiApplication::inputMethod()->reset();
        emit composingChanged();
    }
    _readOnly = readOnly;
    _backend->WndProc(SCI_SETREADONLY, readOnly ? 1 : 0, 0);
    _backend->WndProc(SCI_SETCARETLINEVISIBLE, readOnly ? 0 : 1, 0);
    setFlag(QQuickItem::ItemAcceptsInputMethod, !readOnly);
    if (hasActiveFocus())
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
    syncTickingState();
    emit readOnlyChanged();
    update();
}

void ScintillaQuickItem::setFolding(bool enable)
{
    if (_folding == enable)
        return;
    _folding = enable;
    const bool showMargin = _folding && !_largeDocumentMode;
    const int foldMarginW = std::max(14, _fontPixelSize + 2);
    _backend->WndProc(SCI_SETMARGINWIDTHN, 2, showMargin ? foldMarginW : 0);
    _backend->WndProc(SCI_SETMARGINSENSITIVEN, 2, showMargin ? 1 : 0);
    emit foldingChanged();
    update();
}

void ScintillaQuickItem::updateBraceHighlight()
{
    if (_largeDocumentMode) {
        _backend->WndProc(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
        return;
    }
    auto isBrace = [](int ch) {
        return ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}';
    };
    const int pos = caretPosition();
    int bracePos = -1;
    if (pos > 0 && isBrace(static_cast<int>(_backend->WndProc(SCI_GETCHARAT, pos - 1, 0))))
        bracePos = pos - 1;
    else if (isBrace(static_cast<int>(_backend->WndProc(SCI_GETCHARAT, pos, 0))))
        bracePos = pos;

    if (bracePos >= 0) {
        const int matchPos = static_cast<int>(_backend->WndProc(SCI_BRACEMATCH, bracePos, 0));
        if (matchPos >= 0)
            _backend->WndProc(SCI_BRACEHIGHLIGHT, bracePos, matchPos);
        else
            _backend->WndProc(SCI_BRACEBADLIGHT, bracePos, 0);
    } else {
        _backend->WndProc(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
    }
}

void ScintillaQuickItem::applyTheme()
{
    auto rgb = [](int r, int g, int b) -> sptr_t {
        return static_cast<sptr_t>((r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16));
    };

    setFillColor(_darkMode ? QColor(15, 22, 18) : QColor(255, 255, 255));

    // Style sizes are pixel sizes (the vendored PlatQt Font::Create uses
    // QFont::setPixelSize), the same unit as AppSettings.codeFontSize.
    QByteArray family = _font.family().toUtf8();
    _backend->WndProc(SCI_SETFONTQUALITY, SC_EFF_QUALITY_LCD_OPTIMIZED, 0);
    _backend->WndProc(SCI_SETEXTRAASCENT, 2, 0);
    _backend->WndProc(SCI_SETEXTRADESCENT, 2, 0);
    _backend->WndProc(SCI_STYLESETFONT, STYLE_DEFAULT,
                      reinterpret_cast<sptr_t>(family.constData()));
    const int pixelSizeScaled = std::max(10, _fontPixelSize) * SC_FONT_SIZE_MULTIPLIER;
    _backend->WndProc(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT,
                      static_cast<sptr_t>(pixelSizeScaled));

    const sptr_t foldMarginBg = _darkMode ? rgb(12, 19, 15) : rgb(243, 246, 244);
    const sptr_t foldMarkerFg = _darkMode ? rgb(15, 22, 18) : rgb(255, 255, 255);
    const sptr_t foldMarkerBg = _darkMode ? rgb(96, 132, 112) : rgb(130, 152, 140);

    if (_darkMode) {
        _backend->WndProc(SCI_STYLESETBACK, STYLE_DEFAULT, rgb(15, 22, 18));   // #0f1612
        _backend->WndProc(SCI_STYLESETFORE, STYLE_DEFAULT, rgb(230, 240, 234)); // #e6f0ea
        _backend->WndProc(SCI_STYLECLEARALL, 0, 0);

        _backend->WndProc(SCI_SETCARETFORE, rgb(0, 230, 104), 0);              // #00e668
        _backend->WndProc(SCI_SETCARETLINEVISIBLE, _readOnly ? 0 : 1, 0);
        _backend->WndProc(SCI_SETCARETLINEBACK, rgb(22, 34, 27), 0);           // #16221b
        _backend->WndProc(SCI_SETSELBACK, 1, rgb(28, 66, 49));                 // #1c4231
        _backend->WndProc(SCI_STYLESETBACK, STYLE_LINENUMBER, rgb(12, 19, 15));// #0c130f
        _backend->WndProc(SCI_STYLESETFORE, STYLE_LINENUMBER, rgb(96, 118, 106));

        _backend->WndProc(SCI_STYLESETFORE, STYLE_BRACELIGHT, rgb(0, 230, 104));
        _backend->WndProc(SCI_STYLESETBACK, STYLE_BRACELIGHT, rgb(26, 62, 44));
        _backend->WndProc(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
        _backend->WndProc(SCI_STYLESETFORE, STYLE_BRACEBAD, rgb(255, 92, 92));
        _backend->WndProc(SCI_STYLESETBACK, STYLE_BRACEBAD, rgb(68, 24, 24));
        _backend->WndProc(SCI_STYLESETBOLD, STYLE_BRACEBAD, 1);

        _backend->WndProc(SCI_STYLESETFORE, SCE_C_COMMENT, rgb(100, 130, 112));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_COMMENTLINE, rgb(100, 130, 112));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_COMMENTDOC, rgb(100, 130, 112));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_WORD, rgb(86, 214, 150));    // JS keywords
        _backend->WndProc(SCI_STYLESETBOLD, SCE_C_WORD, 1);
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_WORD2, rgb(79, 193, 255));   // Mongo globals
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_STRING, rgb(229, 192, 123)); // strings
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_CHARACTER, rgb(229, 192, 123));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_NUMBER, rgb(209, 154, 102)); // numbers
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_OPERATOR, rgb(156, 184, 168));
    } else {
        _backend->WndProc(SCI_STYLESETBACK, STYLE_DEFAULT, rgb(255, 255, 255));
        _backend->WndProc(SCI_STYLESETFORE, STYLE_DEFAULT, rgb(20, 32, 26));   // #14201a
        _backend->WndProc(SCI_STYLECLEARALL, 0, 0);

        _backend->WndProc(SCI_SETCARETFORE, rgb(0, 163, 68), 0);               // #00a344
        _backend->WndProc(SCI_SETCARETLINEVISIBLE, _readOnly ? 0 : 1, 0);
        _backend->WndProc(SCI_SETCARETLINEBACK, rgb(243, 248, 245), 0);
        _backend->WndProc(SCI_SETSELBACK, 1, rgb(197, 235, 216));              // #c5ebd8
        _backend->WndProc(SCI_STYLESETBACK, STYLE_LINENUMBER, rgb(243, 246, 244));
        _backend->WndProc(SCI_STYLESETFORE, STYLE_LINENUMBER, rgb(130, 148, 138));

        _backend->WndProc(SCI_STYLESETFORE, STYLE_BRACELIGHT, rgb(0, 135, 58));
        _backend->WndProc(SCI_STYLESETBACK, STYLE_BRACELIGHT, rgb(212, 245, 226));
        _backend->WndProc(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
        _backend->WndProc(SCI_STYLESETFORE, STYLE_BRACEBAD, rgb(210, 40, 40));
        _backend->WndProc(SCI_STYLESETBACK, STYLE_BRACEBAD, rgb(253, 226, 226));
        _backend->WndProc(SCI_STYLESETBOLD, STYLE_BRACEBAD, 1);

        _backend->WndProc(SCI_STYLESETFORE, SCE_C_COMMENT, rgb(115, 136, 124));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_COMMENTLINE, rgb(115, 136, 124));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_COMMENTDOC, rgb(115, 136, 124));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_WORD, rgb(0, 135, 58));      // JS keywords
        _backend->WndProc(SCI_STYLESETBOLD, SCE_C_WORD, 1);
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_WORD2, rgb(9, 105, 218));    // Mongo globals
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_STRING, rgb(163, 72, 18));   // strings
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_CHARACTER, rgb(163, 72, 18));
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_NUMBER, rgb(149, 56, 0));    // numbers
        _backend->WndProc(SCI_STYLESETFORE, SCE_C_OPERATOR, rgb(70, 92, 80));
    }

    const int numWidth = _lineNumbers
        ? static_cast<int>(_backend->WndProc(SCI_TEXTWIDTH, STYLE_LINENUMBER,
                                             reinterpret_cast<sptr_t>("_9999")))
        : 0;
    _backend->WndProc(SCI_SETMARGINWIDTHN, 0, numWidth);
    _backend->WndProc(SCI_SETMARGINWIDTHN, 1, 0);

    // Margin 2: Code folding tree
    const bool showFoldMargin = _folding && !_largeDocumentMode;
    const int foldMarginW = std::max(14, _fontPixelSize + 2);
    _backend->WndProc(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
    _backend->WndProc(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    _backend->WndProc(SCI_SETMARGINWIDTHN, 2, showFoldMargin ? foldMarginW : 0);
    _backend->WndProc(SCI_SETMARGINSENSITIVEN, 2, showFoldMargin ? 1 : 0);
    _backend->WndProc(SCI_SETFOLDMARGINCOLOUR, 1, foldMarginBg);
    _backend->WndProc(SCI_SETFOLDMARGINHICOLOUR, 1, foldMarginBg);

    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    _backend->WndProc(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

    for (int m = SC_MARKNUM_FOLDEREND; m <= SC_MARKNUM_FOLDEROPEN; ++m) {
        _backend->WndProc(SCI_MARKERSETFORE, m, foldMarkerFg);
        _backend->WndProc(SCI_MARKERSETBACK, m, foldMarkerBg);
    }

    _backend->WndProc(SCI_SETMARGINLEFT, 0, 8);
    const int docLen = static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
    if (docLen > 128 * 1024)
        _backend->WndProc(SCI_COLOURISE, 0, 32 * 1024);
    else
        _backend->WndProc(SCI_COLOURISE, 0, -1);
}

long ScintillaQuickItem::send(unsigned int msg, unsigned long wParam, long lParam)
{
    long result = _backend->WndProc(msg, wParam, lParam);
    update();
    return result;
}

void ScintillaQuickItem::scrollTo(int line)
{
    _backend->ScrollTo(line);
    update();
}

void ScintillaQuickItem::setCaretPositionUtf16(int utf16Offset)
{
    const QString cur = text();
    const int clamped = std::clamp(utf16Offset, 0, static_cast<int>(cur.size()));
    const int bytePos = static_cast<int>(cur.left(clamped).toUtf8().size());
    const int docLen = static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
    const int safeBytePos = std::clamp(bytePos, 0, docLen);
    const int line = static_cast<int>(_backend->WndProc(SCI_LINEFROMPOSITION, safeBytePos, 0));
    _backend->WndProc(SCI_ENSUREVISIBLEENFORCEPOLICY, line, 0);
    _backend->WndProc(SCI_SETSEL, safeBytePos, safeBytePos);
    _backend->WndProc(SCI_SCROLLCARET, 0, 0);
    update();
}

void ScintillaQuickItem::cut()
{
    if (!_readOnly) {
        _backend->WndProc(SCI_CUT, 0, 0);
        update();
    }
}

void ScintillaQuickItem::copy()
{
    _backend->WndProc(SCI_COPY, 0, 0);
}

void ScintillaQuickItem::paste()
{
    if (!_readOnly) {
        _backend->Paste();
        update();
    }
}

void ScintillaQuickItem::selectAll()
{
    _backend->WndProc(SCI_SELECTALL, 0, 0);
    update();
}

void ScintillaQuickItem::undo()
{
    if (!_readOnly) {
        _backend->WndProc(SCI_UNDO, 0, 0);
        update();
    }
}

void ScintillaQuickItem::redo()
{
    if (!_readOnly) {
        _backend->WndProc(SCI_REDO, 0, 0);
        update();
    }
}

void ScintillaQuickItem::replaceAllText(const QString &newText)
{
    if (_readOnly)
        return;
    const QByteArray utf8 = newText.toUtf8();
    const int oldPos = caretPosition();
    if (!_largeDocumentMode)
        _backend->WndProc(SCI_SETSCROLLWIDTH, 1, 0);
    _backend->WndProc(SCI_BEGINUNDOACTION, 0, 0);
    _backend->WndProc(SCI_SELECTALL, 0, 0);
    _backend->WndProc(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(utf8.constData()));
    _backend->WndProc(SCI_ENDUNDOACTION, 0, 0);
    const int newLen = static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
    _backend->WndProc(SCI_GOTOPOS, std::min(oldPos, newLen), 0);
    _backend->WndProc(SCI_COLOURISE, 0, -1);
    update();
}

void ScintillaQuickItem::toggleLineComment()
{
    if (_readOnly)
        return;
    int selStart = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONSTART, 0, 0));
    int selEnd = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONEND, 0, 0));
    int startLine = static_cast<int>(_backend->WndProc(SCI_LINEFROMPOSITION, selStart, 0));
    int endLine = static_cast<int>(_backend->WndProc(SCI_LINEFROMPOSITION, selEnd, 0));
    if (selEnd > selStart &&
        selEnd == static_cast<int>(_backend->WndProc(SCI_POSITIONFROMLINE, endLine, 0)) &&
        endLine > startLine) {
        --endLine;
    }

    // Check if every non-empty line in [startLine..endLine] already starts with "//"
    bool allCommented = true;
    bool anyNonEmpty = false;
    for (int line = startLine; line <= endLine; ++line) {
        const int lineStart = static_cast<int>(_backend->WndProc(SCI_POSITIONFROMLINE, line, 0));
        const int lineEnd = static_cast<int>(_backend->WndProc(SCI_GETLINEENDPOSITION, line, 0));
        int p = lineStart;
        while (p < lineEnd) {
            const int ch = static_cast<int>(_backend->WndProc(SCI_GETCHARAT, p, 0));
            if (ch != ' ' && ch != '\t')
                break;
            ++p;
        }
        if (p < lineEnd) {
            anyNonEmpty = true;
            const int c0 = static_cast<int>(_backend->WndProc(SCI_GETCHARAT, p, 0));
            const int c1 = (p + 1 < lineEnd)
                ? static_cast<int>(_backend->WndProc(SCI_GETCHARAT, p + 1, 0)) : 0;
            if (c0 != '/' || c1 != '/') {
                allCommented = false;
                break;
            }
        }
    }

    _backend->WndProc(SCI_BEGINUNDOACTION, 0, 0);
    for (int line = startLine; line <= endLine; ++line) {
        const int lineStart = static_cast<int>(_backend->WndProc(SCI_POSITIONFROMLINE, line, 0));
        const int lineEnd = static_cast<int>(_backend->WndProc(SCI_GETLINEENDPOSITION, line, 0));
        if (anyNonEmpty && allCommented) {
            int p = lineStart;
            while (p < lineEnd) {
                const int ch = static_cast<int>(_backend->WndProc(SCI_GETCHARAT, p, 0));
                if (ch != ' ' && ch != '\t')
                    break;
                ++p;
            }
            if (p + 1 < lineEnd &&
                _backend->WndProc(SCI_GETCHARAT, p, 0) == '/' &&
                _backend->WndProc(SCI_GETCHARAT, p + 1, 0) == '/') {
                int removeLen = 2;
                if (p + 2 < lineEnd && _backend->WndProc(SCI_GETCHARAT, p + 2, 0) == ' ')
                    removeLen = 3;
                _backend->WndProc(SCI_DELETERANGE, p, removeLen);
            }
        } else {
            _backend->WndProc(SCI_INSERTTEXT, lineStart, reinterpret_cast<sptr_t>("// "));
        }
    }
    _backend->WndProc(SCI_ENDUNDOACTION, 0, 0);
    update();
}

bool ScintillaQuickItem::findText(const QString &query, bool forward, bool matchCase)
{
    if (query.isEmpty())
        return false;
    const QByteArray needle = query.toUtf8();
    const int docLen = static_cast<int>(_backend->WndProc(SCI_GETLENGTH, 0, 0));
    if (docLen <= 0)
        return false;

    const int flags = matchCase ? SCFIND_MATCHCASE : 0;
    _backend->WndProc(SCI_SETSEARCHFLAGS, flags, 0);

    const int selStart = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONSTART, 0, 0));
    const int selEnd = static_cast<int>(_backend->WndProc(SCI_GETSELECTIONEND, 0, 0));

    auto searchRange = [&](int fromPos, int toPos) -> int {
        _backend->WndProc(SCI_SETTARGETSTART, fromPos, 0);
        _backend->WndProc(SCI_SETTARGETEND, toPos, 0);
        return static_cast<int>(_backend->WndProc(
            SCI_SEARCHINTARGET, needle.size(),
            reinterpret_cast<sptr_t>(needle.constData())));
    };

    int hit = -1;
    if (forward) {
        hit = searchRange(selEnd, docLen);
        if (hit < 0 && selEnd > 0)
            hit = searchRange(0, selEnd);
    } else {
        hit = searchRange(selStart, 0);
        if (hit < 0 && selStart < docLen)
            hit = searchRange(docLen, selStart);
    }

    if (hit >= 0) {
        const int hitEnd = static_cast<int>(_backend->WndProc(SCI_GETTARGETEND, 0, 0));
        const int line = static_cast<int>(_backend->WndProc(SCI_LINEFROMPOSITION, hit, 0));
        _backend->WndProc(SCI_ENSUREVISIBLEENFORCEPOLICY, line, 0);
        _backend->WndProc(SCI_SETSEL, hit, hitEnd);
        _backend->WndProc(SCI_SCROLLCARET, 0, 0);
        update();
        return true;
    }
    return false;
}

void ScintillaQuickItem::foldAll(bool expand)
{
    if (_largeDocumentMode)
        return;
    _backend->WndProc(SCI_COLOURISE, 0, -1);
    const int maxLine = static_cast<int>(_backend->WndProc(SCI_GETLINECOUNT, 0, 0));
    for (int line = 0; line < maxLine; ++line) {
        const int level = static_cast<int>(_backend->WndProc(SCI_GETFOLDLEVEL, line, 0));
        if (level & SC_FOLDLEVELHEADERFLAG) {
            const bool isExpanded = _backend->WndProc(SCI_GETFOLDEXPANDED, line, 0) != 0;
            if (expand && !isExpanded)
                _backend->WndProc(SCI_TOGGLEFOLD, line, 0);
            else if (!expand && isExpanded && (level & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELBASE)
                _backend->WndProc(SCI_TOGGLEFOLD, line, 0);
        }
    }
    update();
}

QPointF ScintillaQuickItem::pointFromPosition(int pos)
{
    const long x = _backend->WndProc(SCI_POINTXFROMPOSITION, 0, pos);
    const long y = _backend->WndProc(SCI_POINTYFROMPOSITION, 0, pos);
    return QPointF(x, y);
}

int ScintillaQuickItem::lineHeight()
{
    return static_cast<int>(_backend->WndProc(SCI_TEXTHEIGHT, 0, 0));
}

void ScintillaQuickItem::paint(QPainter *painter)
{
    _backend->paintInto(painter, boundingRect());
}

