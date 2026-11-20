// What the embedded shell needs from QJSEngine (docs/Shell.md): the JavaScript that core/engine/js/shell-runtime.js is
// written in, and what the host needs from the engine: stopping a runaway script from another thread, a fresh global
// object per run, running on a worker thread, and calling into C++. A Qt upgrade that breaks one of these breaks the shell.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QThread>

namespace
{
    QCoreApplication *ensureApp()
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "rago_unit_tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
            return &app;
        }
        return QCoreApplication::instance();
    }

    QString run(QJSEngine &engine, const QString &script)
    {
        const QJSValue result = engine.evaluate(script);
        if (result.isError())
            return QStringLiteral("ERROR: ") + result.toString();
        return result.toString();
    }
}

class JsEngineCapabilitiesTest : public ::testing::Test
{
protected:
    void SetUp() override { ensureApp(); }
};

TEST_F(JsEngineCapabilitiesTest, the_language_features_of_the_shell_runtime_work)
{
    QJSEngine e;
    struct Case { const char *name; const char *script; const char *expect; };
    const Case cases[] = {
        { "arrow function", "((a, b) => a + b)(2, 3)", "5" },
        { "let / const in a loop", "const f = []; for (let i = 0; i < 3; ++i) f.push(() => i); f.map(g => g()).join()", "0,1,2" },
        { "template literal", "const n = 'x'; `a${n}b${1 + 1}`", "axb2" },
        { "destructuring", "const { a, b: [c] } = { a: 1, b: [2] }; a + c", "3" },
        { "spread and rest", "const f = (...r) => r.length; f(...[1, 2, 3], 4)", "4" },
        { "default parameters", "((a, b = 5) => a + b)(1)", "6" },
        { "class with getter and static", "class A { constructor() { this.x = 1 } get y() { return this.x + 1 } static z() { return 7 } }; new A().y + A.z()", "9" },
        { "class inheritance", "class A { f() { return 1 } } class B extends A { f() { return super.f() + 1 } } new B().f()", "2" },
        { "Object.entries / Object.assign", "Object.entries(Object.assign({ a: 1 }, { b: 2 })).join(';')", "a,1;b,2" },
        { "Array.from, includes, find", "Array.from({ length: 3 }, (_, i) => i * 2).includes(4) && [1, 2, 3].find(x => x > 1)", "2" },
        { "optional chaining", "const o = { a: { b: 1 } }; String(o?.a?.b) + String(o?.x?.y)", "1undefined" },
        { "nullish coalescing", "String(null ?? 'd') + String(0 ?? 'd')", "d0" },
        { "generator", "function* g() { yield 1; yield 2 } [...g()].join()", "1,2" },
        { "Map and Set", "const m = new Map([[1, 'a']]); const s = new Set([1, 1, 2]); m.get(1) + s.size", "a2" },
        { "Symbol.iterator", "const o = { *[Symbol.iterator]() { yield 5 } }; [...o][0]", "5" },
        { "JSON round trip", "JSON.stringify(JSON.parse('{\"a\":[1,{\"b\":null}]}'))", "{\"a\":[1,{\"b\":null}]}" },
        { "Date", "new Date(Date.UTC(2026, 9, 28)).toISOString()", "2026-10-28T00:00:00.000Z" },
        { "typeof, instanceof, Array.isArray", "[typeof (() => 1), [] instanceof Array, Array.isArray([])].join()", "function,true,true" },
        { "try / catch / finally and Error", "let r = ''; try { null.x } catch (e) { r += e.name } finally { r += '!' } r", "TypeError!" },
        { "Promise exists", "typeof Promise", "function" },
        { "Reflect and Object.defineProperty", "const o = {}; Object.defineProperty(o, 'k', { get() { return 3 } }); Reflect.ownKeys(o).length + o.k", "4" },
    };
    for (const Case &c : cases)
        EXPECT_EQ(run(e, QString::fromUtf8(c.script)), QString::fromUtf8(c.expect)) << c.name;
}

// The shell's `db.orders.find()` works because db and every collection are Proxies.
TEST_F(JsEngineCapabilitiesTest, proxy_supports_db_dot_collection_and_dot_dot_names)
{
    QJSEngine e;
    const QString script = QStringLiteral(R"js(
        const calls = [];
        function collection(name) {
            return new Proxy({ find(q) { calls.push(name + '.find(' + JSON.stringify(q) + ')'); return name; } }, {
                get(target, prop) {
                    if (prop in target) return target[prop];
                    if (typeof prop === 'symbol') return undefined;
                    return collection(name + '.' + String(prop));        // db.system.js, db.fs.files
                }
            });
        }
        const db = new Proxy({ getCollection: n => collection(String(n)) }, {
            get(t, prop) { return prop in t ? t[prop] : collection(String(prop)); },
            has(t, prop) { return true; }
        });
        db.orders.find({ a: 1 });
        db['my-coll'].find({});
        db.fs.files.find({});
        db.getCollection('x').find(1);
        calls.join(' | ');
    )js");
    EXPECT_EQ(run(e, script),
              QStringLiteral("orders.find({\"a\":1}) | my-coll.find({}) | fs.files.find({}) | x.find(1)"));
}

TEST_F(JsEngineCapabilitiesTest, a_fresh_global_object_per_run_keeps_runs_apart_and_lets_variables_live_on_within_one)
{
    QJSEngine e;
    // The runtime is loaded once; every eval gets its own globals (the vm.createContext of the former Node shell).
    const QJSValue make = e.evaluate(QStringLiteral(
        "(function (src) { const g = { db: 1, print: x => x };"
        " return new Function('g', 'with (g) { return eval(' + JSON.stringify(src) + ') }')(g); })"));
    ASSERT_TRUE(make.isCallable());
    EXPECT_EQ(make.call({ QJSValue(QStringLiteral("var a = 5; a + db")) }).toString(), QStringLiteral("6"));
    // A second run does not see `a` of the first.
    EXPECT_EQ(make.call({ QJSValue(QStringLiteral("typeof a")) }).toString(), QStringLiteral("undefined"));

    // Several statements of one run share variables, as in the shell.
    QJSEngine e2;
    e2.evaluate(QStringLiteral("var counter = 1"));
    EXPECT_EQ(e2.evaluate(QStringLiteral("counter += 1")).toInt(), 2);
    EXPECT_EQ(e2.evaluate(QStringLiteral("counter * 10")).toInt(), 20);
}

TEST_F(JsEngineCapabilitiesTest, a_runaway_script_is_stopped_from_another_thread_and_the_engine_is_usable_again)
{
    QJSEngine e;
    std::atomic<bool> started{false};
    std::thread stopper([&]() {
        while (!started.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        e.setInterrupted(true);
    });

    started = true;
    QElapsedTimer timer;
    timer.start();
    const QJSValue result = e.evaluate(QStringLiteral("let n = 0; while (true) { n++ }"));
    const qint64 elapsed = timer.elapsed();
    stopper.join();

    EXPECT_TRUE(result.isError());
    EXPECT_LT(elapsed, 2000) << "the loop was not interrupted";
    EXPECT_GE(elapsed, 50);

    e.setInterrupted(false);
    EXPECT_EQ(e.evaluate(QStringLiteral("1 + 1")).toInt(), 2);                 // and it works again
    EXPECT_TRUE(e.evaluate(QStringLiteral("var kept = 3; kept")).toInt() == 3);
}

// The shell runs on the MongoWorker thread: the engine must live and be used on a worker thread.
TEST_F(JsEngineCapabilitiesTest, the_engine_runs_on_a_worker_thread)
{
    QString result;
    QThread *thread = QThread::create([&result]() {
        QJSEngine engine;
        result = run(engine, QStringLiteral("[1, 2, 3].map(x => x * x).join('+')"));
    });
    thread->start();
    ASSERT_TRUE(thread->wait(10000));
    delete thread;
    EXPECT_EQ(result, QStringLiteral("1+4+9"));
}

namespace
{
    // A host object of the kind the shell host will be: synchronous calls into C++.
    class Host : public QObject
    {
        Q_OBJECT
    public:
        int calls = 0;
        Q_INVOKABLE QJSValue echo(const QJSValue &v) { ++calls; return v; }
        Q_INVOKABLE QString describe(const QString &s, int n) { ++calls; return s + QString::number(n); }
        Q_INVOKABLE void fail() { ++calls; qWarning("host: fail() called"); }
    };
}

TEST_F(JsEngineCapabilitiesTest, a_cpp_host_object_can_be_called_synchronously_and_exchanges_objects_and_arrays)
{
    QJSEngine e;
    Host host;
    e.globalObject().setProperty(QStringLiteral("__host"), e.newQObject(&host));

    EXPECT_EQ(run(e, QStringLiteral("__host.describe('n=', 4)")), QStringLiteral("n=4"));
    EXPECT_EQ(run(e, QStringLiteral("JSON.stringify(__host.echo({ a: [1, 'x', { b: null }], c: true }))")),
              QStringLiteral("{\"a\":[1,\"x\",{\"b\":null}],\"c\":true}"));
    EXPECT_EQ(host.calls, 2);
}

TEST_F(JsEngineCapabilitiesTest, exceptions_carry_name_message_and_line)
{
    QJSEngine e;
    const QJSValue r = e.evaluate(QStringLiteral("function f() {\n  throw new RangeError('too big');\n}\nf();"), QStringLiteral("shell"));
    ASSERT_TRUE(r.isError());
    EXPECT_EQ(r.property(QStringLiteral("name")).toString(), QStringLiteral("RangeError"));
    EXPECT_EQ(r.property(QStringLiteral("message")).toString(), QStringLiteral("too big"));
    EXPECT_EQ(r.property(QStringLiteral("lineNumber")).toInt(), 2);

    const QJSValue syntax = e.evaluate(QStringLiteral("var x = ;"));
    EXPECT_TRUE(syntax.isError());
    EXPECT_EQ(syntax.property(QStringLiteral("name")).toString(), QStringLiteral("SyntaxError"));
}

TEST_F(JsEngineCapabilitiesTest, a_cpu_heavy_script_finishes_within_a_sane_bound)
{
    QJSEngine e;
    QElapsedTimer timer;
    timer.start();
    const QJSValue r = e.evaluate(QStringLiteral(
        "let s = 0; for (let i = 0; i < 3000000; ++i) { s += i % 7 } const o = []; "
        "for (let i = 0; i < 100000; ++i) o.push({ i, t: 'x' + i }); s + o.length"));
    const qint64 ms = timer.elapsed();
    ASSERT_FALSE(r.isError());
    std::cerr << "[ INFO ] 3M loop iterations + 100k objects: " << ms << " ms\n";
    EXPECT_LT(ms, 30000);                                                    // a sanity bound, not a benchmark
}

#include "JsEngineCapabilities_test.moc"
