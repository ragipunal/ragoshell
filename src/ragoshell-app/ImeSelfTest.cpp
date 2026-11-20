#include "ragoshell-app/ImeSelfTest.h"

#include <QDebug>

#include <QCoreApplication>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextCharFormat>

#include "ragoshell-app/PlatformInfo.h"
#include "ragoshell-app/ScintillaQuick.h"

// Scintilla message numbers used below (Scintilla.h is not included here).
namespace
{
    constexpr unsigned kSciGotoPos = 2025;
    constexpr unsigned kSciSetSel = 2160;

    int failures = 0;

    void check(const char *name, bool ok, const QString &detail = QString())
    {
        if (!ok)
            ++failures;
        qInfo("%s  %s%s", ok ? "PASS" : "FAIL", name,
              ok || detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -- ") + detail));
    }

    class MockInputReceiver : public QObject
    {
    public:
        bool password = false;
        QString lastKeyText;
        int lastKeyCode = 0;
        QString lastCommit;
        QString lastPreedit;

        bool event(QEvent *e) override
        {
            if (e->type() == QEvent::InputMethodQuery) {
                auto *q = static_cast<QInputMethodQueryEvent *>(e);
                if (q->queries() & Qt::ImHints) {
                    q->setValue(Qt::ImHints,
                                password ? int(Qt::ImhHiddenText | Qt::ImhSensitiveData) : 0);
                }
                q->accept();
                return true;
            }
            if (e->type() == QEvent::KeyPress) {
                auto *ke = static_cast<QKeyEvent *>(e);
                lastKeyText = ke->text();
                lastKeyCode = ke->key();
                ke->accept();
                return true;
            }
            if (e->type() == QEvent::InputMethod) {
                auto *ime = static_cast<QInputMethodEvent *>(e);
                lastCommit = ime->commitString();
                lastPreedit = ime->preeditString();
                ime->accept();
                return true;
            }
            return QObject::event(e);
        }
    };

    QList<QInputMethodEvent::Attribute> composingAttributes(const QString &preedit)
    {
        QTextCharFormat underline;
        underline.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        return {
            QInputMethodEvent::Attribute(QInputMethodEvent::TextFormat, 0,
                                         static_cast<int>(preedit.size()), underline),
            QInputMethodEvent::Attribute(QInputMethodEvent::Cursor,
                                         static_cast<int>(preedit.size()), 1, QVariant()),
        };
    }

    void preedit(ScintillaQuickItem &editor, const QString &text)
    {
        QInputMethodEvent event(text, text.isEmpty() ? QList<QInputMethodEvent::Attribute>()
                                                     : composingAttributes(text));
        QCoreApplication::sendEvent(&editor, &event);
    }

    void commit(ScintillaQuickItem &editor, const QString &text, int replaceFrom = 0, int replaceLength = 0)
    {
        QInputMethodEvent event;
        event.setCommitString(text, replaceFrom, replaceLength);
        QCoreApplication::sendEvent(&editor, &event);
    }

    void reset(ScintillaQuickItem &editor, const QString &text)
    {
        editor.setReadOnly(false);
        editor.setText(text);
        editor.setSavePoint();
        editor.send(kSciGotoPos, static_cast<unsigned long>(text.toUtf8().size()));
    }

    QString query(ScintillaQuickItem &editor, Qt::InputMethodQuery q)
    {
        return editor.inputMethodQuery(q).toString();
    }
}

int RagoShell::runInputMethodSelfTest()
{
    ScintillaQuickItem editor;
    editor.setSize(QSizeF(600, 300));
    int charsAdded = 0;
    QObject::connect(&editor, &ScintillaQuickItem::charAdded, [&charsAdded](int) { ++charsAdded; });

    check("accepts input methods", editor.flags().testFlag(QQuickItem::ItemAcceptsInputMethod)
                                       && editor.inputMethodQuery(Qt::ImEnabled).toBool());

    // Japanese: compose に → にほ, commit 日本.
    reset(editor, QStringLiteral("db.x"));
    preedit(editor, QStringLiteral("に"));
    check("preedit shown inline", editor.text() == QStringLiteral("db.xに"), editor.text());
    check("composing while preediting", editor.composing());
    check("preedit is not typing (no charAdded)", charsAdded == 0, QString::number(charsAdded));
    check("preedit leaves the document unmodified", !editor.isModified());
    check("surrounding text excludes preedit", query(editor, Qt::ImSurroundingText) == QStringLiteral("db.x"),
          query(editor, Qt::ImSurroundingText));
    check("cursor position at composition start", editor.inputMethodQuery(Qt::ImCursorPosition).toInt() == 4);
    const QRectF rect = editor.inputMethodQuery(Qt::ImCursorRectangle).toRectF();
    check("cursor rectangle for the candidate window", rect.height() > 0 && rect.x() > 0,
          QStringLiteral("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()));
    preedit(editor, QStringLiteral("にほ"));
    check("preedit replaced, not appended", editor.text() == QStringLiteral("db.xにほ"), editor.text());
    commit(editor, QStringLiteral("日本"));
    check("commit replaces the preedit", editor.text() == QStringLiteral("db.x日本"), editor.text());
    check("not composing after commit", !editor.composing());
    check("committed text marks the document modified", editor.isModified());
    check("cursor position counts characters", editor.inputMethodQuery(Qt::ImCursorPosition).toInt() == 6,
          QString::number(editor.inputMethodQuery(Qt::ImCursorPosition).toInt()));
    editor.undo();
    check("one undo removes the commit, preedit never in history", editor.text() == QStringLiteral("db.x"),
          editor.text());

    // Cancel: an empty event while composing removes the composition.
    reset(editor, QStringLiteral("db.x"));
    preedit(editor, QStringLiteral("かな"));
    preedit(editor, QString());
    check("cancel removes the composition", editor.text() == QStringLiteral("db.x") && !editor.composing(),
          editor.text());

    // Korean-style replacement of the character before the caret.
    reset(editor, QStringLiteral("ab"));
    commit(editor, QStringLiteral("c"), -1, 1);
    check("replacement range relative to the caret", editor.text() == QStringLiteral("ac"), editor.text());
    reset(editor, QStringLiteral("x한"));
    commit(editor, QStringLiteral("글"), -1, 1);
    check("replacement counts characters, not bytes", editor.text() == QStringLiteral("x글"), editor.text());

    // Commit over a selection.
    reset(editor, QStringLiteral("hello world"));
    editor.send(kSciSetSel, 0, 5);
    commit(editor, QStringLiteral("こんにちは"));
    check("commit replaces the selection", editor.text() == QStringLiteral("こんにちは world"), editor.text());

    // Focus loss keeps the uncommitted text as ordinary, undoable text.
    reset(editor, QStringLiteral("q "));
    preedit(editor, QStringLiteral("未確定"));
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&editor, &focusOut);
    check("focus loss keeps the composition as text", editor.text() == QStringLiteral("q 未確定")
                                                          && !editor.composing(), editor.text());
    editor.undo();
    check("kept composition is undoable", editor.text() == QStringLiteral("q "), editor.text());

    // A click finishes the composition too.
    reset(editor, QStringLiteral("q "));
    preedit(editor, QStringLiteral("中文"));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &release);
    check("click keeps the composition as text", editor.text() == QStringLiteral("q 中文")
                                                     && !editor.composing(), editor.text());

    // Surrounding text is the current line; surrogate pairs count as two.
    reset(editor, QStringLiteral("line1\nab😀"));
    check("surrounding text is the current line", query(editor, Qt::ImSurroundingText) == QStringLiteral("ab😀"),
          query(editor, Qt::ImSurroundingText));
    check("cursor position in UTF-16 units", editor.inputMethodQuery(Qt::ImCursorPosition).toInt() == 4,
          QString::number(editor.inputMethodQuery(Qt::ImCursorPosition).toInt()));

    // Read-only: no input method, events are ignored, a composition is dropped.
    reset(editor, QStringLiteral("ro"));
    preedit(editor, QStringLiteral("あ"));
    editor.setReadOnly(true);
    check("read-only drops the composition", editor.text() == QStringLiteral("ro") && !editor.composing(),
          editor.text());
    check("read-only disables the input method",
          !editor.flags().testFlag(QQuickItem::ItemAcceptsInputMethod)
              && !editor.inputMethodQuery(Qt::ImEnabled).toBool());
    commit(editor, QStringLiteral("x"));
    check("read-only ignores commits", editor.text() == QStringLiteral("ro"), editor.text());
    editor.setReadOnly(false);
    check("editable again accepts input methods", editor.flags().testFlag(QQuickItem::ItemAcceptsInputMethod));

    // Smart quotes (iOS/macOS curly quotes): normalized on keyboard & IME input, preserved in setText().
    int lastCharAdded = 0;
    QObject::connect(&editor, &ScintillaQuickItem::charAdded, [&lastCharAdded](int ch) { lastCharAdded = ch; });

    reset(editor, QString());
    {
        QKeyEvent leftSingle(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf16(u"\u2018"));
        QCoreApplication::sendEvent(&editor, &leftSingle);
        check("curly single quote KeyPress normalized to ASCII '",
              editor.text() == QStringLiteral("'") && lastCharAdded == '\'', editor.text());

        QKeyEvent rightDouble(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf16(u"\u201D"));
        QCoreApplication::sendEvent(&editor, &rightDouble);
        check("curly double quote KeyPress normalized to ASCII \"",
              editor.text() == QStringLiteral("'\"") && lastCharAdded == '"', editor.text());
    }

    reset(editor, QStringLiteral("\""));
    commit(editor, QString::fromUtf16(u"\u201C"), -1, 1); // iOS retroactive smart-quote replacement
    check("iOS retroactive smart-quote IME replacement stays straight \"",
          editor.text() == QStringLiteral("\""), editor.text());

    reset(editor, QString());
    preedit(editor, QString::fromUtf16(u"\u2018test\u2019"));
    check("curly quotes in preedit normalized inline",
          editor.text() == QStringLiteral("'test'"), editor.text());
    commit(editor, QString::fromUtf16(u"{ \u201Csku\u201D: \u2018A1\u2019 }"));
    check("curly quotes in IME commit normalized to valid query syntax",
          editor.text() == QStringLiteral("{ \"sku\": 'A1' }"), editor.text());

    const QString existingCurlyDoc = QString::fromUtf16(u"{ \"note\": \"Don\u2019t mutate DB text\" }");
    editor.setText(existingCurlyDoc);
    check("setText preserves existing curly quotes in loaded DB documents",
          editor.text() == existingCurlyDoc, editor.text());

    // Application-wide PlatformInfo filter: normalizes non-password inputs, preserves password fields.
    {
        PlatformInfo platformFilter;
        MockInputReceiver field;

        QKeyEvent curlyKey(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf16(u"\u2019"));
        QCoreApplication::sendEvent(&field, &curlyKey);
        check("PlatformInfo normalizes curly KeyPress for text fields",
              field.lastKeyText == QStringLiteral("'") && field.lastKeyCode == Qt::Key_Apostrophe,
              field.lastKeyText);

        QInputMethodEvent curlyIme(QString::fromUtf16(u"\u201Cp\u201D"), {});
        curlyIme.setCommitString(QString::fromUtf16(u"\u2018c\u2019"));
        QCoreApplication::sendEvent(&field, &curlyIme);
        check("PlatformInfo normalizes curly IME commit/preedit for text fields",
              field.lastCommit == QStringLiteral("'c'") && field.lastPreedit == QStringLiteral("\"p\""),
              field.lastCommit + QLatin1Char('/') + field.lastPreedit);

        field.password = true;
        QKeyEvent pwKey(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf16(u"\u2019"));
        QCoreApplication::sendEvent(&field, &pwKey);
        check("PlatformInfo leaves password field curly quotes untouched",
              field.lastKeyText == QString::fromUtf16(u"\u2019"), field.lastKeyText);
    }

    qInfo("%s", failures ? qPrintable(QStringLiteral("%1 check(s) failed").arg(failures))
                         : "all checks passed");
    return failures;
}
