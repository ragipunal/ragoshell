#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include <ragoshell/ssh/ssh.h>
#include "ragoshell/core/events/MongoEvents.h"

struct rbm_ssh_tunnel_config;
struct rbm_ssh_session;

namespace RagoShell
{
    class ConnectionSettings;

    enum class SshTunnelErrorKind
    {
        None = 0,
        Network,
        HostKey,
        Auth,
        LocalBind,
        Stopped
    };

    struct SshReconnectPolicy
    {
        int initialDelayMs = 500;
        int maxDelayMs = 30000;
        int maxAttempts = 8;
        int resumeCooldownMs = 2000;

        /// Deterministic exponential backoff delay (ms) for 1-based attempt index.
        int delayForAttempt(int attempt) const;
    };

    struct SshTunnelSetupResult
    {
        bool ok = false;
        int boundLocalPort = 0;
        SshTunnelErrorKind errorKind = SshTunnelErrorKind::None;
        std::string errorMessage;
    };

    struct SshTunnelRunResult
    {
        SshTunnelErrorKind errorKind = SshTunnelErrorKind::None;
        std::string errorMessage;
    };

    /**
     * @brief Abstract single-attempt SSH tunnel transport session.
     *        Production uses Libssh2TunnelSession; unit tests may inject a fake or
     *        loopback TCP implementation to deterministically exercise supervision.
     */
    class ISshTunnelSession
    {
    public:
        virtual ~ISshTunnelSession() = default;

        virtual SshTunnelSetupResult setup(int preferredLocalPort, bool interactiveHostKey) = 0;
        virtual SshTunnelRunResult runForwarding() = 0;
        virtual void requestStop() = 0;
    };

    using SshTunnelSessionFactory =
        std::function<std::unique_ptr<ISshTunnelSession>(const ConnectionSettings &settings,
                                                         std::function<void(const std::string &, int)> logFn)>;

    /*
     * Helper class for creating rbm_ssh_tunnel_config from ConnectionSettings.
     * Automatically wipes in-memory secret copies and deletes the allocated config.
     */
    class SshTunnelConfigCreator
    {
    public:
        explicit SshTunnelConfigCreator(const ConnectionSettings *settings, int preferredLocalPort = 0);
        ~SshTunnelConfigCreator();
        rbm_ssh_tunnel_config *config() { return _sshConfig; }
        void wipeSecrets();

    private:
        std::string _sshhost;
        int _sshport;
        std::string _remotehost;
        int _remoteport;
        std::string _localip;
        int _localport;

        std::string _userName;
        std::string _userPassword;
        std::string _privateKeyFile;
        std::string _publicKeyFile;
        std::string _passphrase;
        std::string _authMethod; // "password" or "publickey"
        std::string _knownHostsFile;

        rbm_ssh_tunnel_config *_sshConfig;
    };

    /**
     * @brief Owns and supervises the SSH port-forwarding worker thread for a MongoServer.
     *
     * Threading & safety invariants (docs/SshTunnelReconnect.md):
     *  - External lifecycle operations (start*, restart, onNetworkResumed, stop, ~SshTunnelWorker)
     *    serialize on _lifecycleMutex. The worker thread NEVER acquires _lifecycleMutex.
     *  - Old sessions close their 127.0.0.1:<localPort> listener socket and join their thread
     *    before any new listener socket is opened.
     *  - Unattended reconnects enforce strict known_hosts matching and halt immediately on
     *    HostKey mismatch or Auth rejection without further retries.
     */
    class SshTunnelWorker : public QObject
    {
        Q_OBJECT

    public:
        enum class State
        {
            Idle,
            InitialConnecting,
            Running,
            Reconnecting,
            BackoffWaiting,
            Exhausted,
            HaltedFatal,
            Stopped
        };

        explicit SshTunnelWorker(ConnectionSettings *settings,
                                 SshReconnectPolicy policy = SshReconnectPolicy{},
                                 SshTunnelSessionFactory sessionFactory = SshTunnelSessionFactory{});
        ~SshTunnelWorker() override;

        /// Starts initial interactive SSH setup + forwarding asynchronously, replying via EventBus.
        void startInitialConnect(QObject *replyReceiver, int serverHandle, ConnectionType connectionType);

        /// Starts initial setup + forwarding without EventBus (for standalone supervision / unit tests).
        bool startStandalone(bool interactiveHostKey = true);

        /// Stops any active session, joins the old worker thread, and reconnects on 127.0.0.1:<localPort>.
        /// Returns false if stop() was called or the tunnel halted on a fatal HostKey/Auth error.
        bool restart();

        /// Triggered when the network returns or the device wakes from sleep/suspension.
        /// Coalesces duplicate resume bursts and wakes backoff waits immediately.
        bool onNetworkResumed();

        /// Permanently stops the tunnel and joins the worker thread. Never reconnects afterwards.
        void stop();

        int localPort() const;
        State state() const;
        bool isRunning() const;
        bool isStopped() const;
        bool isHaltedByFatalError() const;
        SshTunnelErrorKind lastErrorKind() const;
        std::string lastErrorMessage() const;
        std::vector<int> recordedBackoffDelaysMs() const;

        ConnectionSettings *settings() const { return _settings.get(); }

        static void logCallbackHandler(void *context, char *message, int level);
        /// Asks the user (HostKeyPrompter) whether to trust a new or changed SSH host key.
        static int interactiveHostKeyCallback(void *context, const rbm_hostkey_info *info);

    Q_SIGNALS:
        void initialConnectFinished(bool ok, int localPort, int errorKind, const QString &errorMessage);
        void tunnelReconnected(int localPort, bool portChanged);
        void tunnelPermanentlyFailed(int errorKind, const QString &errorMessage);

    private:
        bool restartLocked();
        void supervisorThreadMain(bool initialConnect, bool interactiveInitialHostKey);
        SshTunnelSetupResult establishSession(std::unique_ptr<ISshTunnelSession> *outSession,
                                              int preferredPort,
                                              bool interactiveHostKey);
        bool waitBackoffInterruptible(int delayMs);
        void reply(QObject *receiver, Event *event);
        void log(const std::string &message, int level = RBM_SSH_LOG_TYPE_INFO);
        static qint64 nowSteadyMs();

        mutable std::mutex _lifecycleMutex;
        mutable std::mutex _stateMutex;
        std::condition_variable _stateCv;
        std::thread _workerThread;

        std::unique_ptr<ConnectionSettings> _settings;
        SshReconnectPolicy _policy;
        SshTunnelSessionFactory _sessionFactory;

        QPointer<QObject> _replyReceiver;
        int _serverHandle = 0;
        ConnectionType _connectionType = ConnectionPrimary;

        State _state = State::Idle;
        bool _userStopped = false;
        bool _stopCurrentRun = false;
        bool _nudgeImmediateRetry = false;
        ISshTunnelSession *_activeSession = nullptr;
        int _boundLocalPort = 0;
        int _attemptCount = 0;
        SshTunnelErrorKind _lastErrorKind = SshTunnelErrorKind::None;
        std::string _lastErrorMessage;
        std::vector<int> _recordedBackoffDelaysMs;
        qint64 _lastEstablishedSteadyMs = 0;
        qint64 _lastRestartSteadyMs = 0;
    };
}
