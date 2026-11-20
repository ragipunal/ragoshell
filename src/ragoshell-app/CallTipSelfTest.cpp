#include "ragoshell-app/CallTipSelfTest.h"

#include <QDebug>

#include <QStringList>

#include "ragoshell-app/ScintillaQuick.h"

namespace
{
    constexpr unsigned kSciGotoPos = 2025;
    int failures = 0;

    void check(const char *name, bool ok, const QString &detail = QString())
    {
        if (!ok)
            ++failures;
        qInfo("%s  %s%s", ok ? "PASS" : "FAIL", name,
              ok || detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -- ") + detail));
    }

    QVariantMap tipAtEnd(ScintillaQuickItem &editor, const QString &text)
    {
        editor.setText(text);
        editor.send(kSciGotoPos, static_cast<unsigned long>(text.toUtf8().size()));
        return editor.shellCallTip();
    }
}

int RagoShell::runCallTipSelfTest()
{
    ScintillaQuickItem editor;
    editor.setSize(QSizeF(800, 300));

    QVariantMap tip = tipAtEnd(editor, QStringLiteral("db.items.find({ tag: 'a' }, "));
    check("tip for find()", tip.value("name").toString() == QStringLiteral("find"), tip.value("name").toString());
    check("second parameter is active", tip.value("active").toInt() == 1);
    check("parameters listed", tip.value("params").toStringList()
                                   == QStringList({"filter", "projection", "options"}));

    // Non-ASCII text before and inside the call: x must be Scintilla's
    // position of the '(' byte, not of a character count.
    const QString beforeParen = QStringLiteral("x = 'ü😀'; db.items.find");
    tip = tipAtEnd(editor, beforeParen + QStringLiteral("({ a: 'é' }, "));
    const qreal expectedX = editor.pointFromPosition(static_cast<int>(beforeParen.toUtf8().size())).x();
    check("x at the '(' with UTF-8 text around it",
          tip.value("name").toString() == QStringLiteral("find") && expectedX > 0
              && qFuzzyCompare(tip.value("x").toReal(), expectedX),
          QStringLiteral("%1 vs %2").arg(tip.value("x").toReal()).arg(expectedX));

    tip = tipAtEnd(editor, QStringLiteral("db.items.find({}).sort({ n: -1 }).limit("));
    check("cursor method on a chain", tip.value("name").toString() == QStringLiteral("limit"));

    tip = tipAtEnd(editor, QStringLiteral("line one\ndb.items.aggregate([{ $match: {} }, "));
    const qreal y = tip.value("y").toReal();
    check("y on the call's line", tip.value("name").toString() == QStringLiteral("aggregate") && y > 0,
          QString::number(y));

    check("no tip after the call closed", tipAtEnd(editor, QStringLiteral("db.items.find({})")).isEmpty());
    check("no tip inside a string", tipAtEnd(editor, QStringLiteral("db.items.find({ a: 'b, ")).isEmpty());

    // UTF-8 multi-byte (em-dash + Turkish + surrogate pair) atomic undoable completion replacement.
    const QString header = QStringLiteral("// localhost — sorgu😀\ndb.müş");
    editor.setText(header);
    editor.setSavePoint();
    editor.send(kSciGotoPos, static_cast<unsigned long>(header.toUtf8().size()));
    editor.replaceBeforeCaret(3, QStringLiteral("müşteriler.find({})"), -2);
    check("replaceBeforeCaret preserves multi-byte UTF-8 prefix",
          editor.text() == QStringLiteral("// localhost — sorgu😀\ndb.müşteriler.find({})"),
          editor.text());
    check("replaceBeforeCaret positions caret inside ({})",
          editor.textBeforeCaret(64).endsWith(QStringLiteral("find({"))
              && editor.textAfterCaret(4) == QStringLiteral("})"),
          editor.textAfterCaret(4));
    editor.undo();
    check("single undo reverts completion without wiping prior text",
          editor.text() == header, editor.text());

    qInfo("%s", failures ? qPrintable(QStringLiteral("%1 check(s) failed").arg(failures))
                         : "all checks passed");
    return failures;
}
