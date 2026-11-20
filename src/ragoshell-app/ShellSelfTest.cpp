#include "ragoshell-app/ShellSelfTest.h"

#include <cstdio>
#include <functional>
#include <vector>

#include <QDebug>
#include <QElapsedTimer>

#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/settings/ConnectionSettings.h"

namespace RagoShell
{
    namespace
    {
        struct Check
        {
            const char *name;
            const char *script;
            std::function<bool(const MongoShellExecResult &)> passes;
        };

        const MongoShellResult *last(const MongoShellExecResult &res)
        {
            return res.error() || res.results().empty() ? nullptr : &res.results().back();
        }

        bool outputIs(const MongoShellExecResult &res, const std::string &expected)
        {
            const MongoShellResult *r = last(res);
            return r && r->type() != "error" && r->response() == expected;
        }

        bool outputHas(const MongoShellExecResult &res, const std::string &part)
        {
            const MongoShellResult *r = last(res);
            return r && r->type() != "error" && r->response().find(part) != std::string::npos;
        }

        void report(bool pass, const std::string &name, const std::string &detail = std::string())
        {
            const std::string line = std::string("SHELLSELFTEST ") + (pass ? "PASS " : "FAIL ") + name
                                     + (detail.empty() ? "" : " -- " + detail);
            std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);
            // Android: `adb logcat` shows Qt's log, not stderr
            qWarning().noquote() << QString::fromStdString(line);
        }
    }

    int runShellSelfTest(const QString &hostAndPort)
    {
        const qsizetype colon = hostAndPort.lastIndexOf(QLatin1Char(':'));
        ConnectionSettings settings(/*isClone=*/false);
        settings.setServerHost((colon > 0 ? hostAndPort.left(colon) : hostAndPort).toStdString());
        settings.setServerPort(colon > 0 ? hostAndPort.mid(colon + 1).toInt() : 27017);
        settings.setDefaultDatabase("rago_selftest");

        ScriptEngine engine(&settings, 15);
        report(true, std::string("backend ") + engine.backendName());

        QElapsedTimer connectTimer;
        connectTimer.start();
        engine.init("", "rago_selftest");
        if (engine.failedScope()) {
            report(false, "connect", engine.lastInitError());
            return 1;
        }
        // The times are for docs/Shell.md: connect, then the first script of every check.
        report(true, "connect", std::to_string(connectTimer.elapsed()) + " ms");

        const std::vector<Check> checks = {
            { "drop database", "db.dropDatabase()",
              [](const MongoShellExecResult &r) { return outputHas(r, "\"dropped\":\"rago_selftest\""); } },
            { "insertOne", "db.t.insertOne({_id: 1, name: 'caf\\u00e9 \\u20ac', n: NumberLong('9007199254740993')})",
              [](const MongoShellExecResult &r) { return outputHas(r, "\"acknowledged\":true"); } },
            { "insertMany", "db.t.insertMany([{_id: 2, name: 'b'}, {_id: 3, name: 'c'}])",
              [](const MongoShellExecResult &r) { return outputHas(r, "\"insertedCount\":2"); } },
            { "count is a value", "db.t.countDocuments({})",
              [](const MongoShellExecResult &r) { return outputIs(r, "3"); } },
            { "find gives a query result with documents", "db.t.find().sort({_id: 1})",
              [](const MongoShellExecResult &r) {
                  const MongoShellResult *x = last(r);
                  return x && x->type() == "query" && x->documents().size() == 3;
              } },
            { "aggregate gives an aggregate result", "db.t.aggregate([{$match: {_id: {$gte: 2}}}])",
              [](const MongoShellExecResult &r) {
                  const MongoShellResult *x = last(r);
                  return x && x->type() == "aggregate" && x->documents().size() == 2;
              } },
            { "64-bit integer keeps its digits", "db.t.findOne({_id: 1}).n.toString()",
              [](const MongoShellExecResult &r) { return outputIs(r, "\"9007199254740993\""); } },
            { "unicode text", "db.t.findOne({_id: 1}).name.length",
              [](const MongoShellExecResult &r) { return outputIs(r, "6"); } },
            { "server result used in a loop",
              "var seen = 0; for (var i = 1; i <= 3; i++) { if (db.t.findOne({_id: i}).name) seen++ } seen",
              [](const MongoShellExecResult &r) { return outputIs(r, "3"); } },
            { "variables live on between statements", "var a = 5;\nconst b = a * 2;\nb",
              [](const MongoShellExecResult &r) { return outputIs(r, "10"); } },
            { "update", "db.t.updateOne({_id: 2}, {$set: {name: 'bb'}})",
              [](const MongoShellExecResult &r) { return outputHas(r, "\"modifiedCount\":1"); } },
            { "print", "print('hello', 1)",
              [](const MongoShellExecResult &r) { return outputIs(r, "hello 1"); } },
            { "server error is an error result", "db.t.find({$badoperator: 1})",
              [](const MongoShellExecResult &r) {
                  const MongoShellResult *x = last(r);
                  return x && x->type() == "error" && x->response().rfind("MongoServerError:", 0) == 0;
              } },
            { "syntax error is an error result", "this is not (valid",
              [](const MongoShellExecResult &r) {
                  const MongoShellResult *x = last(r);
                  return x && x->type() == "error" && x->response().find("SyntaxError") != std::string::npos;
              } },
            { "show collections", "show collections",
              [](const MongoShellExecResult &r) { return outputHas(r, "\"name\":\"t\""); } },
            { "clean up", "db.dropDatabase()",
              [](const MongoShellExecResult &r) { return outputHas(r, "\"ok\":1"); } },
        };

        int failures = 0;
        for (const Check &check : checks) {
            QElapsedTimer timer;
            timer.start();
            const MongoShellExecResult res = engine.exec(check.script, "rago_selftest");
            const qint64 ms = timer.elapsed();
            const bool pass = check.passes(res);
            std::string detail = pass ? std::to_string(ms) + " ms" : std::string();
            if (!pass) {
                ++failures;
                const MongoShellResult *r = res.results().empty() ? nullptr : &res.results().back();
                detail = res.error() ? res.errorMessage() : (r ? r->type() + ": " + r->response().substr(0, 200) : "no result");
            }
            report(pass, check.name, detail);
        }
        // Autocomplete: operators, the methods of db, and the collections of the current database.
        {
            engine.exec("db.t.insertOne({_id: 1})", "rago_selftest");
            const QStringList dollar = engine.complete("$ma", AutocompleteAll);
            const QStringList dbList = engine.complete("db.", AutocompleteAll);
            const QStringList collection = engine.complete("db.t.fi", AutocompleteAll);
            const QStringList cursor = engine.complete("db.t.find({}).", AutocompleteAll);
            const bool pass = dollar.contains(QStringLiteral("$match")) && dbList.contains(QStringLiteral("db.stats()"))
                              && dbList.contains(QStringLiteral("db.t")) && collection.contains(QStringLiteral("db.t.find({})"))
                              && cursor.contains(QStringLiteral("db.t.find({}).sort({})"))
                              && !cursor.contains(QStringLiteral("db.t"));
            if (!pass)
                ++failures;
            report(pass, "autocomplete", pass ? std::string()
                                              : "$ma: " + dollar.join(' ').toStdString() + " | db.: "
                                                    + dbList.join(' ').toStdString().substr(0, 120));
            engine.exec("db.dropDatabase()", "rago_selftest");
        }

        // The time limit: a script that loops ends with "timeout reached" at the limit, and the shell works again.
        {
            engine.changeTimeout(1);
            QElapsedTimer timer;
            timer.start();
            const MongoShellExecResult res = engine.exec("var n = 0; while (true) { n++ }", "rago_selftest");
            const bool pass = res.timeoutReached() && timer.elapsed() < 4000;
            if (!pass)
                ++failures;
            report(pass, "runaway script stops at the time limit",
                   pass ? std::string() : "timeoutReached=" + std::to_string(res.timeoutReached()) + " after "
                                              + std::to_string(timer.elapsed()) + " ms");
            engine.changeTimeout(15);
            const MongoShellExecResult again = engine.exec("1 + 1", "rago_selftest");
            const bool worksAgain = !again.error() && !again.results().empty() && again.results().back().response() == "2";
            if (!worksAgain)
                ++failures;
            report(worksAgain, "shell works again after the time limit");
        }

        report(failures == 0, failures == 0 ? "all checks passed" : std::to_string(failures) + " check(s) failed");
        return failures;
    }
}
