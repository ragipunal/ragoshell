#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ragoshell/core/mongodb/SshTunnelWorker.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/ssh/ssh.h"

using namespace RagoShell;

namespace
{
    std::once_flag gSshInitOnce;

    void ensureSshSubmoduleInitialized()
    {
        std::call_once(gSshInitOnce, []() {
            rbm_ssh_init();
        });
    }

    ConnectionSettings *makeTestSshSettings()
    {
        auto *conn = new ConnectionSettings(true);
        conn->setConnectionName("Prod via Bastion");
        conn->setServerHost("10.0.0.42");
        conn->setServerPort(27017);
        conn->sshSettings()->setEnabled(true);
        conn->sshSettings()->setHost("bastion.example.internal");
        conn->sshSettings()->setPort(22);
        conn->sshSettings()->setUserName("deploy");
        conn->sshSettings()->setAuthMethod("publickey");
        conn->sshSettings()->setPrivateKeyFile("/nonexistent/id_ed25519");
        conn->sshSettings()->setPassphrase("top-secret-passphrase");
        return conn;
    }

    template <typename Pred>
    bool waitUntil(Pred &&pred, int timeoutMs = 4000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return pred();
    }

    struct FakeSetupRecord
    {
        int preferredLocalPort = -1;
        bool interactiveHostKey = false;
        int assignedLocalPort = 0;
    };

    class FakeTunnelController
    {
    public:
        struct ScriptedSetup
        {
            bool ok = true;
            SshTunnelErrorKind errorKind = SshTunnelErrorKind::None;
            std::string errorMessage;
            int forcePort = 0;
        };

        void enqueueSetup(ScriptedSetup step)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _scriptedSetups.push_back(std::move(step));
        }

        void setDefaultSetup(ScriptedSetup fallback)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _defaultSetup = std::move(fallback);
        }

        void triggerNetworkDrop(const std::string &reason = "SSH keepalive timeout")
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _dropCurrentGeneration = _runGeneration;
            _dropReason = reason;
            _cv.notify_all();
        }

        int setupCallCount() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return static_cast<int>(_records.size());
        }

        std::vector<FakeSetupRecord> records() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _records;
        }

        int peakConcurrentSessions() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _peakAliveSessions;
        }

        int aliveSessions() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _aliveSessions;
        }

        int peakConcurrentForwarders() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _peakForwarders;
        }

        SshTunnelSessionFactory factory()
        {
            return [this](const ConnectionSettings &, std::function<void(const std::string &, int)>) {
                return std::make_unique<SessionImpl>(*this);
            };
        }

    private:
        class SessionImpl final : public ISshTunnelSession
        {
        public:
            explicit SessionImpl(FakeTunnelController &ctrl) : _ctrl(ctrl)
            {
                std::lock_guard<std::mutex> lock(_ctrl._mutex);
                ++_ctrl._aliveSessions;
                if (_ctrl._aliveSessions > _ctrl._peakAliveSessions)
                    _ctrl._peakAliveSessions = _ctrl._aliveSessions;
            }

            ~SessionImpl() override
            {
                std::lock_guard<std::mutex> lock(_ctrl._mutex);
                --_ctrl._aliveSessions;
            }

            SshTunnelSetupResult setup(int preferredLocalPort, bool interactiveHostKey) override
            {
                std::lock_guard<std::mutex> lock(_ctrl._mutex);
                if (_stopRequested)
                    return {false, 0, SshTunnelErrorKind::Stopped, "stopped"};

                ScriptedSetup step = _ctrl._defaultSetup;
                if (!_ctrl._scriptedSetups.empty()) {
                    step = _ctrl._scriptedSetups.front();
                    _ctrl._scriptedSetups.pop_front();
                }

                int boundPort = 0;
                if (step.ok) {
                    if (step.forcePort > 0)
                        boundPort = step.forcePort;
                    else if (preferredLocalPort > 0)
                        boundPort = preferredLocalPort;
                    else
                        boundPort = _ctrl._nextEphemeralPort++;
                }

                _ctrl._records.push_back({preferredLocalPort, interactiveHostKey, boundPort});
                if (!step.ok)
                    return {false, 0, step.errorKind, step.errorMessage};

                return {true, boundPort, SshTunnelErrorKind::None, {}};
            }

            SshTunnelRunResult runForwarding() override
            {
                std::unique_lock<std::mutex> lock(_ctrl._mutex);
                const int myGen = ++_ctrl._runGeneration;
                ++_ctrl._activeForwarders;
                if (_ctrl._activeForwarders > _ctrl._peakForwarders)
                    _ctrl._peakForwarders = _ctrl._activeForwarders;

                _ctrl._cv.wait(lock, [&]() {
                    return _stopRequested.load() || _ctrl._dropCurrentGeneration >= myGen;
                });

                --_ctrl._activeForwarders;

                if (_stopRequested.load())
                    return {SshTunnelErrorKind::Stopped, {}};

                return {SshTunnelErrorKind::Network, _ctrl._dropReason};
            }

            void requestStop() override
            {
                _stopRequested.store(true);
                _ctrl._cv.notify_all();
            }

        private:
            FakeTunnelController &_ctrl;
            std::atomic<bool> _stopRequested{false};
        };

        mutable std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<ScriptedSetup> _scriptedSetups;
        ScriptedSetup _defaultSetup{};
        std::vector<FakeSetupRecord> _records;
        int _nextEphemeralPort = 43100;
        int _aliveSessions = 0;
        int _peakAliveSessions = 0;
        int _activeForwarders = 0;
        int _peakForwarders = 0;
        int _runGeneration = 0;
        int _dropCurrentGeneration = 0;
        std::string _dropReason;
    };

    bool canTcpConnectLoopback(int port)
    {
#if defined(_WIN32) || defined(WIN32)
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return false;
#else
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0)
            return false;
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        const int rc = connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#if defined(_WIN32) || defined(WIN32)
        closesocket(s);
#else
        close(s);
#endif
        return rc == 0;
    }

    /**
     * Real OS socket session backed by rbm_ssh_session_create + rbm_ssh_session_listen_local +
     * rbm_ssh_session_stop + rbm_ssh_session_close to prove actual 127.0.0.1:<port> rebinding.
     */
    class RealLoopbackListenerController
    {
    public:
        void setPreBindHook(std::function<void(int)> hook)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _preBindHook = std::move(hook);
        }

        void triggerDrop()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _dropGen = _gen;
            _cv.notify_all();
        }

        int setupCount() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _setupCount;
        }

        SshTunnelSessionFactory factory()
        {
            return [this](const ConnectionSettings &settings,
                          std::function<void(const std::string &, int)>) {
                return std::make_unique<LoopbackSession>(*this, settings);
            };
        }

    private:
        class LoopbackSession final : public ISshTunnelSession
        {
        public:
            LoopbackSession(RealLoopbackListenerController &ctrl, const ConnectionSettings &settings)
                : _ctrl(ctrl),
                  _settings(settings)
            {
            }

            ~LoopbackSession() override
            {
                rbm_ssh_session *raw = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    raw = _session;
                    _session = nullptr;
                }
                if (raw)
                    rbm_ssh_session_close(raw);
            }

            SshTunnelSetupResult setup(int preferredLocalPort, bool) override
            {
                std::function<void(int)> hook;
                {
                    std::lock_guard<std::mutex> lock(_ctrl._mutex);
                    hook = _ctrl._preBindHook;
                    _ctrl._preBindHook = nullptr;
                }
                if (hook)
                    hook(preferredLocalPort);

                _cfgCreator = std::make_unique<SshTunnelConfigCreator>(&_settings, preferredLocalPort);
                rbm_ssh_session *raw = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    if (_stopped)
                        return {false, 0, SshTunnelErrorKind::Stopped, "stopped"};
                    _session = rbm_ssh_session_create(_cfgCreator->config());
                    raw = _session;
                }
                if (!raw)
                    return {false, 0, SshTunnelErrorKind::Network, "session alloc failed"};

                if (rbm_ssh_session_listen_local(raw) != 0) {
                    const SshTunnelErrorKind kind = (raw->errorcode == RBM_SSH_ERR_LOCAL_BIND)
                        ? SshTunnelErrorKind::LocalBind
                        : SshTunnelErrorKind::Network;
                    const std::string err = raw->lasterror ? raw->lasterror : "bind failed";
                    {
                        std::lock_guard<std::mutex> lock(_sessionMutex);
                        _session = nullptr;
                    }
                    rbm_ssh_session_close(raw);
                    return {false, 0, kind, err};
                }

                {
                    std::lock_guard<std::mutex> lock(_ctrl._mutex);
                    ++_ctrl._setupCount;
                }
                return {true, static_cast<int>(_cfgCreator->config()->localport), SshTunnelErrorKind::None, {}};
            }

            SshTunnelRunResult runForwarding() override
            {
                std::unique_lock<std::mutex> lock(_ctrl._mutex);
                const int myGen = ++_ctrl._gen;
                _ctrl._cv.wait(lock, [&]() {
                    return _stopped.load() || _ctrl._dropGen >= myGen;
                });
                if (_stopped.load())
                    return {SshTunnelErrorKind::Stopped, {}};
                return {SshTunnelErrorKind::Network, "simulated SSH transport EOF"};
            }

            void requestStop() override
            {
                _stopped.store(true);
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    if (_session)
                        rbm_ssh_session_stop(_session);
                }
                _ctrl._cv.notify_all();
            }

        private:
            RealLoopbackListenerController &_ctrl;
            const ConnectionSettings &_settings;
            std::unique_ptr<SshTunnelConfigCreator> _cfgCreator;
            std::mutex _sessionMutex;
            rbm_ssh_session *_session = nullptr;
            std::atomic<bool> _stopped{false};
        };

        mutable std::mutex _mutex;
        std::condition_variable _cv;
        std::function<void(int)> _preBindHook;
        int _setupCount = 0;
        int _gen = 0;
        int _dropGen = 0;
    };

    class ScopedExternalLoopbackBlocker
    {
    public:
        bool bindPort(int port)
        {
#if defined(_WIN32) || defined(WIN32)
            _sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_sock == INVALID_SOCKET)
                return false;
#else
            _sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_sock < 0)
                return false;
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<unsigned short>(port));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                closeNow();
                return false;
            }
            if (listen(_sock, 4) != 0) {
                closeNow();
                return false;
            }
            return true;
        }

        ~ScopedExternalLoopbackBlocker()
        {
            closeNow();
        }

        void closeNow()
        {
#if defined(_WIN32) || defined(WIN32)
            if (_sock != INVALID_SOCKET) {
                closesocket(_sock);
                _sock = INVALID_SOCKET;
            }
#else
            if (_sock >= 0) {
                close(_sock);
                _sock = -1;
            }
#endif
        }

    private:
#if defined(_WIN32) || defined(WIN32)
        SOCKET _sock = INVALID_SOCKET;
#else
        int _sock = -1;
#endif
    };

    class SshTunnelWorkerTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            ensureSshSubmoduleInitialized();
        }
    };
} // namespace

TEST_F(SshTunnelWorkerTest, unexpected_drop_closes_old_session_and_rebinds_same_local_port)
{
    FakeTunnelController ctrl;
    SshReconnectPolicy policy;
    policy.initialDelayMs = 5;
    policy.maxDelayMs = 40;
    policy.maxAttempts = 4;

    std::atomic<int> reconnectedPort{0};
    std::atomic<bool> reconnectedPortChanged{true};
    std::atomic<int> reconnectedSignals{0};

    SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
    QObject::connect(&worker, &SshTunnelWorker::tunnelReconnected,
                     [&](int port, bool portChanged) {
                         reconnectedPort.store(port);
                         reconnectedPortChanged.store(portChanged);
                         reconnectedSignals.fetch_add(1);
                     });

    ASSERT_TRUE(worker.startStandalone(true));
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));

    const int initialPort = worker.localPort();
    ASSERT_GT(initialPort, 0);

    // Simulate sudden SSH tunnel disconnect.
    ctrl.triggerNetworkDrop("SSH connection reset by peer");

    ASSERT_TRUE(waitUntil([&]() { return reconnectedSignals.load() == 1 && worker.isRunning(); }));
    EXPECT_EQ(worker.localPort(), initialPort);
    EXPECT_EQ(reconnectedPort.load(), initialPort);
    EXPECT_FALSE(reconnectedPortChanged.load());
    EXPECT_EQ(ctrl.peakConcurrentSessions(), 1);
    EXPECT_EQ(ctrl.peakConcurrentForwarders(), 1);

    const auto records = ctrl.records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].preferredLocalPort, 0);
    EXPECT_TRUE(records[0].interactiveHostKey);
    EXPECT_EQ(records[1].preferredLocalPort, initialPort);
    EXPECT_FALSE(records[1].interactiveHostKey);
    EXPECT_EQ(records[1].assignedLocalPort, initialPort);

    worker.stop();
    EXPECT_EQ(ctrl.aliveSessions(), 0);
}

TEST_F(SshTunnelWorkerTest, real_loopback_listener_rebinds_exact_same_port_across_drop_and_restart)
{
    RealLoopbackListenerController ctrl;
    SshReconnectPolicy policy;
    policy.initialDelayMs = 5;
    policy.maxDelayMs = 50;
    policy.maxAttempts = 4;

    SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
    ASSERT_TRUE(worker.startStandalone(false));
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));

    const int originalPort = worker.localPort();
    ASSERT_GT(originalPort, 0);
    EXPECT_TRUE(canTcpConnectLoopback(originalPort));

    // 1. Explicit restart() stops and joins the old thread, closes the old listener socket,
    //    and binds a brand-new listener on the exact same 127.0.0.1:<originalPort>.
    ASSERT_TRUE(worker.restart());
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning() && ctrl.setupCount() == 2; }));
    EXPECT_EQ(worker.localPort(), originalPort);
    EXPECT_TRUE(canTcpConnectLoopback(originalPort));

    // 2. Unexpected transport drop inside runForwarding() also rebinds 127.0.0.1:<originalPort>.
    ctrl.triggerDrop();
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning() && ctrl.setupCount() == 3; }));
    EXPECT_EQ(worker.localPort(), originalPort);
    EXPECT_TRUE(canTcpConnectLoopback(originalPort));

    worker.stop();
    EXPECT_FALSE(canTcpConnectLoopback(originalPort));
}

TEST_F(SshTunnelWorkerTest, c_listener_enforces_loopback_only_rejects_port_hijack_and_falls_back_when_busy)
{
    std::unique_ptr<ConnectionSettings> settings(makeTestSshSettings());

    // 1. Non-loopback bind addresses ("0.0.0.0" and LAN IP) must be refused by socket_listen.
    {
        SshTunnelConfigCreator cfgCreator(settings.get(), 0);
        char wildcardIp[] = "0.0.0.0";
        cfgCreator.config()->localip = wildcardIp;
        rbm_ssh_session *sess = rbm_ssh_session_create(cfgCreator.config());
        ASSERT_NE(sess, nullptr);
        EXPECT_EQ(rbm_ssh_session_listen_local(sess), -1);
        EXPECT_EQ(sess->errorcode, RBM_SSH_ERR_LOCAL_BIND);
        rbm_ssh_session_close(sess);
    }
    {
        SshTunnelConfigCreator cfgCreator(settings.get(), 0);
        char lanIp[] = "192.168.1.25";
        cfgCreator.config()->localip = lanIp;
        rbm_ssh_session *sess = rbm_ssh_session_create(cfgCreator.config());
        ASSERT_NE(sess, nullptr);
        EXPECT_EQ(rbm_ssh_session_listen_local(sess), -1);
        EXPECT_EQ(sess->errorcode, RBM_SSH_ERR_LOCAL_BIND);
        rbm_ssh_session_close(sess);
    }

    // 2. An active listener on 127.0.0.1:<port> cannot be hijacked by a second session
    //    (proves SO_REUSEPORT is never enabled and Windows SO_REUSEADDR is not set).
    {
        SshTunnelConfigCreator cfgA(settings.get(), 0);
        rbm_ssh_session *sessA = rbm_ssh_session_create(cfgA.config());
        ASSERT_NE(sessA, nullptr);
        ASSERT_EQ(rbm_ssh_session_listen_local(sessA), 0);
        const int activePort = static_cast<int>(cfgA.config()->localport);
        ASSERT_GT(activePort, 0);

        SshTunnelConfigCreator cfgB(settings.get(), activePort);
        rbm_ssh_session *sessB = rbm_ssh_session_create(cfgB.config());
        ASSERT_NE(sessB, nullptr);
        EXPECT_EQ(rbm_ssh_session_listen_local(sessB), -1);
        EXPECT_EQ(sessB->errorcode, RBM_SSH_ERR_LOCAL_BIND);

        rbm_ssh_session_close(sessB);
        rbm_ssh_session_close(sessA);
    }

    // 3. If an external OS process occupies the previous loopback port while the tunnel is down,
    //    SshTunnelWorker falls back cleanly to a fresh ephemeral 127.0.0.1 port and flags portChanged.
    RealLoopbackListenerController ctrl;
    SshReconnectPolicy policy;
    policy.initialDelayMs = 5;
    policy.maxDelayMs = 40;
    policy.maxAttempts = 3;

    std::atomic<int> notifiedPort{0};
    std::atomic<bool> notifiedPortChanged{false};
    ScopedExternalLoopbackBlocker externalHolder;

    SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
    QObject::connect(&worker, &SshTunnelWorker::tunnelReconnected,
                     [&](int newPort, bool portChanged) {
                         notifiedPort.store(newPort);
                         notifiedPortChanged.store(portChanged);
                     });

    ASSERT_TRUE(worker.startStandalone(false));
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));
    const int firstPort = worker.localPort();
    ASSERT_GT(firstPort, 0);

    ctrl.setPreBindHook([&](int preferredPort) {
        if (preferredPort == firstPort)
            externalHolder.bindPort(firstPort);
    });

    ASSERT_TRUE(worker.restart());
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning() && notifiedPort.load() > 0; }));
    EXPECT_NE(worker.localPort(), firstPort);
    EXPECT_EQ(notifiedPort.load(), worker.localPort());
    EXPECT_TRUE(notifiedPortChanged.load());
    EXPECT_TRUE(canTcpConnectLoopback(worker.localPort()));

    worker.stop();
}

TEST_F(SshTunnelWorkerTest, concurrent_restarts_and_network_resume_bursts_never_double_start)
{
    FakeTunnelController ctrl;
    SshReconnectPolicy policy;
    policy.initialDelayMs = 5;
    policy.maxDelayMs = 25;
    policy.maxAttempts = 5;
    policy.resumeCooldownMs = 2000;

    SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
    ASSERT_TRUE(worker.startStandalone(false));
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));
    const int initialPort = worker.localPort();

    // Coalescing check: immediate burst of onNetworkResumed() within resumeCooldownMs
    // on a freshly established tunnel does not tear down and restart the tunnel.
    const int countBeforeBurst = ctrl.setupCallCount();
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(worker.onNetworkResumed());
    EXPECT_EQ(ctrl.setupCallCount(), countBeforeBurst);

    // Hammer restart() and onNetworkResumed() concurrently across multiple threads.
    std::vector<std::thread> callers;
    for (int t = 0; t < 6; ++t) {
        callers.emplace_back([&, t]() {
            for (int iter = 0; iter < 5; ++iter) {
                if ((iter + t) % 2 == 0)
                    worker.restart();
                else
                    worker.onNetworkResumed();
            }
        });
    }
    for (auto &th : callers)
        th.join();

    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));
    EXPECT_EQ(worker.localPort(), initialPort);
    EXPECT_EQ(ctrl.peakConcurrentSessions(), 1);
    EXPECT_EQ(ctrl.peakConcurrentForwarders(), 1);

    worker.stop();
    EXPECT_EQ(ctrl.aliveSessions(), 0);
}

TEST_F(SshTunnelWorkerTest, explicit_stop_interrupts_backoff_and_blocks_subsequent_restarts)
{
    FakeTunnelController ctrl;
    ctrl.enqueueSetup({true, SshTunnelErrorKind::None, {}, 0});
    ctrl.setDefaultSetup({false, SshTunnelErrorKind::Network, "network unreachable", 0});

    SshReconnectPolicy policy;
    policy.initialDelayMs = 15000; // 15 s backoff to prove stop() interrupts sleep promptly
    policy.maxDelayMs = 30000;
    policy.maxAttempts = 5;

    SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
    ASSERT_TRUE(worker.startStandalone(false));
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));

    ctrl.triggerNetworkDrop("Wi-Fi turned off");
    ASSERT_TRUE(waitUntil([&]() {
        return worker.state() == SshTunnelWorker::State::BackoffWaiting;
    }));

    const auto stopStart = std::chrono::steady_clock::now();
    worker.stop();
    const auto stopElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - stopStart)
                                   .count();

    EXPECT_LT(stopElapsedMs, 1000);
    EXPECT_TRUE(worker.isStopped());
    EXPECT_EQ(worker.state(), SshTunnelWorker::State::Stopped);

    const int attemptsAtStop = ctrl.setupCallCount();
    EXPECT_FALSE(worker.restart());
    EXPECT_FALSE(worker.onNetworkResumed());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(ctrl.setupCallCount(), attemptsAtStop);
}

TEST_F(SshTunnelWorkerTest, exponential_backoff_attempt_limit_and_recovery_after_exhaustion)
{
    SshReconnectPolicy mathCheck;
    mathCheck.initialDelayMs = 500;
    mathCheck.maxDelayMs = 30000;
    EXPECT_EQ(mathCheck.delayForAttempt(1), 500);
    EXPECT_EQ(mathCheck.delayForAttempt(2), 1000);
    EXPECT_EQ(mathCheck.delayForAttempt(3), 2000);
    EXPECT_EQ(mathCheck.delayForAttempt(4), 4000);
    EXPECT_EQ(mathCheck.delayForAttempt(5), 8000);
    EXPECT_EQ(mathCheck.delayForAttempt(6), 16000);
    EXPECT_EQ(mathCheck.delayForAttempt(7), 30000);
    EXPECT_EQ(mathCheck.delayForAttempt(8), 30000);
    EXPECT_EQ(mathCheck.delayForAttempt(20), 30000);

    FakeTunnelController ctrl;
    ctrl.enqueueSetup({true, SshTunnelErrorKind::None, {}, 0});
    ctrl.setDefaultSetup({false, SshTunnelErrorKind::Network, "SSH host unreachable", 0});

    SshReconnectPolicy policy;
    policy.initialDelayMs = 5;
    policy.maxDelayMs = 20;
    policy.maxAttempts = 4;
    policy.resumeCooldownMs = 0;

    std::atomic<int> permFailedCount{0};
    SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
    QObject::connect(&worker, &SshTunnelWorker::tunnelPermanentlyFailed,
                     [&](int, const QString &) { permFailedCount.fetch_add(1); });

    ASSERT_TRUE(worker.startStandalone(false));
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));
    const int initialPort = worker.localPort();

    ctrl.triggerNetworkDrop("link down");
    ASSERT_TRUE(waitUntil([&]() {
        return worker.state() == SshTunnelWorker::State::Exhausted;
    }));

    // 1 initial connect + 4 reconnect attempts = 5 total setups, with 3 sleeps between the 4 attempts.
    EXPECT_EQ(ctrl.setupCallCount(), 5);
    const std::vector<int> delays = worker.recordedBackoffDelaysMs();
    const std::vector<int> expectedDelays{5, 10, 20};
    EXPECT_EQ(delays, expectedDelays);
    EXPECT_EQ(permFailedCount.load(), 1);

    // Network comes back later: onNetworkResumed() revives the Exhausted worker on the same port.
    ctrl.setDefaultSetup({true, SshTunnelErrorKind::None, {}, 0});
    ASSERT_TRUE(worker.onNetworkResumed());
    ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));
    EXPECT_EQ(worker.localPort(), initialPort);

    worker.stop();
}

TEST_F(SshTunnelWorkerTest, hostkey_mismatch_and_auth_failure_halt_immediately_without_retries)
{
    for (SshTunnelErrorKind fatalKind : {SshTunnelErrorKind::HostKey, SshTunnelErrorKind::Auth}) {
        FakeTunnelController ctrl;
        ctrl.enqueueSetup({true, SshTunnelErrorKind::None, {}, 0});
        ctrl.setDefaultSetup({false,
                              fatalKind,
                              fatalKind == SshTunnelErrorKind::HostKey
                                  ? "SSH server host key has CHANGED"
                                  : "Authentication by key failed",
                              0});

        SshReconnectPolicy policy;
        policy.initialDelayMs = 5;
        policy.maxDelayMs = 50;
        policy.maxAttempts = 6;

        SshTunnelWorker worker(makeTestSshSettings(), policy, ctrl.factory());
        ASSERT_TRUE(worker.startStandalone(true));
        ASSERT_TRUE(waitUntil([&]() { return worker.isRunning(); }));

        ctrl.triggerNetworkDrop("server restarted with new identity/auth");
        ASSERT_TRUE(waitUntil([&]() { return worker.isHaltedByFatalError(); }));

        EXPECT_EQ(worker.state(), SshTunnelWorker::State::HaltedFatal);
        EXPECT_EQ(worker.lastErrorKind(), fatalKind);
        // Exactly 1 initial setup + 1 failed reconnect setup; zero retries and zero backoff waits!
        EXPECT_EQ(ctrl.setupCallCount(), 2);
        EXPECT_TRUE(worker.recordedBackoffDelaysMs().empty());

        // Unattended restart / network resume must refuse to reconnect until user reconnects interactively.
        EXPECT_FALSE(worker.restart());
        EXPECT_FALSE(worker.onNetworkResumed());
        EXPECT_EQ(ctrl.setupCallCount(), 2);

        worker.stop();
    }

    // Verify in-place secret wiping on SshSettings and SshTunnelConfigCreator.
    std::unique_ptr<ConnectionSettings> secretConn(makeTestSshSettings());
    secretConn->sshSettings()->setUserPassword("my-secret-password");
    secretConn->sshSettings()->setAskedPassword("my-asked-secret");
    SshTunnelConfigCreator cfgCreator(secretConn.get(), 0);
    EXPECT_STREQ(cfgCreator.config()->password, "my-secret-password");
    EXPECT_STREQ(cfgCreator.config()->passphrase, "top-secret-passphrase");
    cfgCreator.wipeSecrets();
    EXPECT_STREQ(cfgCreator.config()->password, "");
    EXPECT_STREQ(cfgCreator.config()->passphrase, "");
    secretConn->sshSettings()->wipeSecrets();
    EXPECT_TRUE(secretConn->sshSettings()->userPassword().empty());
    EXPECT_TRUE(secretConn->sshSettings()->passphrase().empty());
    EXPECT_TRUE(secretConn->sshSettings()->askedPassword().empty());
}
