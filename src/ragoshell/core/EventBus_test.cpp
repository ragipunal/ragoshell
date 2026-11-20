#include "gtest/gtest.h"

#include <atomic>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include "ragoshell/core/Event.h"
#include "ragoshell/core/EventBus.h"

namespace RagoShell
{
    class BusPingEvent : public Event
    {
        R_EVENT
        BusPingEvent(QObject *sender, int seq) : Event(sender), seq(seq) {}
        int seq;
    };
    R_REGISTER_EVENT(BusPingEvent)

    class BusPingReceiver : public QObject
    {
        Q_OBJECT
    public:
        std::atomic<int> lastSeq{ -1 };
        std::atomic<int> count{ 0 };

    public Q_SLOTS:
        void handle(BusPingEvent *ev)
        {
            lastSeq.store(ev->seq);
            count.fetch_add(1);
        }
    };
}

namespace
{
    void ensureCoreApp()
    {
        if (QCoreApplication::instance())
            return;
        static int argc = 1;
        static char arg0[] = "rago_unit_tests";
        static char *argv[] = { arg0, nullptr };
        static QCoreApplication app(argc, argv);
    }
}

TEST(EventBus, DeliversAcrossRepeatedlyDestroyedAndReusedWorkerThreads)
{
    ensureCoreApp();
    RagoShell::EventBus bus;

    for (int i = 0; i < 30; ++i) {
        auto *thread = new QThread();
        auto *receiver = new RagoShell::BusPingReceiver();
        receiver->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, receiver, &QObject::deleteLater);
        thread->start();

        bus.send(receiver, new RagoShell::BusPingEvent(nullptr, i));

        QElapsedTimer timer;
        timer.start();
        while (receiver->lastSeq.load() != i && timer.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(2);
        }
        EXPECT_EQ(receiver->lastSeq.load(), i) << "Iteration " << i;

        thread->quit();
        ASSERT_TRUE(thread->wait(2000));
        delete thread;
    }
}

TEST(EventBus, SkipsReceiverDestroyedBeforeQueuedDelivery)
{
    ensureCoreApp();
    RagoShell::EventBus bus;

    auto *thread = new QThread();
    auto *survivor = new RagoShell::BusPingReceiver();
    auto *ephemeral = new RagoShell::BusPingReceiver();
    survivor->moveToThread(thread);
    ephemeral->moveToThread(thread);

    // Queue events BEFORE starting the thread's event loop, then delete ephemeral on main thread
    // before starting thread so EventWrapper holds a nulled QPointer alongside survivor.
    QList<QObject *> targets{ ephemeral, survivor };
    bus.send(targets, new RagoShell::BusPingEvent(nullptr, 77));
    delete ephemeral;

    QObject::connect(thread, &QThread::finished, survivor, &QObject::deleteLater);
    thread->start();

    QElapsedTimer timer;
    timer.start();
    while (survivor->lastSeq.load() != 77 && timer.elapsed() < 2000) {
        QThread::msleep(2);
    }
    EXPECT_EQ(survivor->lastSeq.load(), 77);
    EXPECT_EQ(survivor->count.load(), 1);

    thread->quit();
    ASSERT_TRUE(thread->wait(2000));
    delete thread;
}

#include "EventBus_test.moc"
