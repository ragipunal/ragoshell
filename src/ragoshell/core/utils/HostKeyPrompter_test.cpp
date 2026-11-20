#include <gtest/gtest.h>

#include <thread>

#include <QElapsedTimer>

#include "ragoshell/core/utils/HostKeyPrompter.h"

using namespace RagoShell;

namespace
{
    HostKeyQuestion question()
    {
        HostKeyQuestion q;
        q.host = QStringLiteral("db.example.com");
        q.port = 22;
        q.keyType = QStringLiteral("ssh-ed25519");
        q.fingerprint = QStringLiteral("SHA256:abc");
        return q;
    }

    // Every test starts and ends without a handler.
    class HostKeyPrompterTest : public ::testing::Test
    {
    protected:
        void SetUp() override { HostKeyPrompter::instance().setHandler(nullptr); }
        void TearDown() override { HostKeyPrompter::instance().setHandler(nullptr); }
    };
}

TEST(HostKeyFingerprintTest, matches_the_openssh_format)
{
    unsigned char zeros[32] = {};
    EXPECT_EQ(hostKeyFingerprint(zeros), QStringLiteral("SHA256:") + QString(43, QLatin1Char('A')));   // no '=' padding

    unsigned char bytes[32];
    for (int i = 0; i < 32; ++i)
        bytes[i] = static_cast<unsigned char>(i);
    EXPECT_EQ(hostKeyFingerprint(bytes), QStringLiteral("SHA256:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"));

    EXPECT_TRUE(hostKeyFingerprint(nullptr).isEmpty());
}

TEST_F(HostKeyPrompterTest, without_a_handler_the_key_is_not_trusted)
{
    EXPECT_FALSE(HostKeyPrompter::instance().ask(question(), 1000));
}

TEST_F(HostKeyPrompterTest, the_answer_of_the_ui_thread_reaches_the_worker)
{
    HostKeyPrompter &prompter = HostKeyPrompter::instance();
    std::vector<std::thread> uiThreads;
    HostKeyQuestion seen;
    prompter.setHandler([&](const HostKeyQuestion &q) {
        seen = q;
        uiThreads.emplace_back([&prompter, id = q.id]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            prompter.answer(id, true);
        });
    });

    EXPECT_TRUE(prompter.ask(question(), 5000));
    EXPECT_GT(seen.id, 0);
    EXPECT_EQ(seen.host, QStringLiteral("db.example.com"));
    for (auto &t : uiThreads)
        t.join();

    // And a refusal.
    prompter.setHandler([&](const HostKeyQuestion &q) { prompter.answer(q.id, false); });   // answered at once
    EXPECT_FALSE(prompter.ask(question(), 5000));
}

TEST_F(HostKeyPrompterTest, no_answer_in_time_means_not_trusted_and_a_late_answer_is_ignored)
{
    HostKeyPrompter &prompter = HostKeyPrompter::instance();
    int lastId = 0;
    prompter.setHandler([&](const HostKeyQuestion &q) { lastId = q.id; });

    QElapsedTimer timer;
    timer.start();
    EXPECT_FALSE(prompter.ask(question(), 150));
    EXPECT_GE(timer.elapsed(), 140);
    EXPECT_LT(timer.elapsed(), 3000);

    prompter.answer(lastId, true);        // the dialog was clicked after the timeout: nothing happens
    prompter.answer(9999, true);          // unknown id
    prompter.setHandler([&](const HostKeyQuestion &q) { prompter.answer(q.id, true); });
    EXPECT_TRUE(prompter.ask(question(), 1000));   // the next question is not affected
}
