#include "ragoshell-app/DraftsSelfTest.h"

#include <QDebug>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ragoshell-app/ScriptDraftModel.h"

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

    RagoShell::ScriptDraft draft(const QString &id, const QString &script)
    {
        RagoShell::ScriptDraft d;
        d.id = id;
        d.serverUuid = QStringLiteral("u1");
        d.serverName = QStringLiteral("local");
        d.database = QStringLiteral("samples");
        d.title = QStringLiteral("tab ") + id;
        d.script = script;
        return d;
    }

    QStringList ids(const RagoShell::ScriptDraftModel &model)
    {
        QStringList out;
        for (int row = 0; row < model.rowCount(); ++row)
            out << model.data(model.index(row), RagoShell::ScriptDraftModel::IdRole).toString();
        return out;
    }
}

int RagoShell::runDraftsSelfTest()
{
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("drafts.json"));

    // A session with three dirty tabs that ends without saving them.
    {
        ScriptDraftModel session(file);
        check("an empty folder has nothing to recover", session.rowCount() == 0 && !session.takeStartupPrompt());

        session.update(draft("a", "db.a.find({})"));
        session.update(draft("b", "db.b.find({})"));
        session.update(draft("c", "db.c.find({})"));
        check("drafts of open tabs are not in the recovery list", session.rowCount() == 0);
        check("the drafts are on disk at once", QFileInfo::exists(file));

        session.update(draft("b", "db.b.find({ more: 1 })"));       // typing goes on in tab b
        session.remove("c");                                          // tab c was closed
        session.update(draft("a", "   "));                            // tab a: text deleted
    }

    // The next start: only tab b is left to recover.
    {
        ScriptDraftModel next(file);
        check("the draft of the unclosed tab is found", ids(next) == QStringList{"b"});
        ScriptDraft found;
        check("it has the latest text and the connection",
              next.find("b", &found) && found.script == QStringLiteral("db.b.find({ more: 1 })")
                  && found.serverUuid == QStringLiteral("u1") && found.database == QStringLiteral("samples"));
        check("the startup prompt shows once", next.takeStartupPrompt() && !next.takeStartupPrompt());

        next.update(draft("d", "db.d.find({})"));
        check("a new tab's draft does not join the list", ids(next) == QStringList{"b"});

        next.claim("b");                                              // a tab took the draft over
        check("a claimed draft leaves the list but stays stored", next.rowCount() == 0 && next.find("b", nullptr));
    }

    // A claimed draft whose tab is still open when the app dies comes back.
    {
        ScriptDraftModel again(file);
        check("drafts of a tab that never closed come back", ids(again).size() == 2);

        again.discard(ids(again).first());
        check("discard removes one", again.rowCount() == 1);
        again.discardAll();
        check("discard all empties the list and removes the file",
              again.rowCount() == 0 && !QFileInfo::exists(file));
    }

    {
        ScriptDraftModel off(file);
        off.update(draft("e", "db.e.find({})"));
    }
    {
        ScriptDraftModel quiet(file);
        quiet.setStartupPromptEnabled(false);
        check("dev runs turn the startup prompt off", quiet.rowCount() == 1 && !quiet.takeStartupPrompt());
    }

    qInfo("%s", failures ? qPrintable(QStringLiteral("%1 check(s) failed").arg(failures))
                         : "all checks passed");
    return failures;
}
