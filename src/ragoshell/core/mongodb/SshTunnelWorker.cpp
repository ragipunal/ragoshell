#include "ragoshell/core/mongodb/SshTunnelWorker.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

#include <QCoreApplication>
#include <QDir>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/HostKeyPrompter.h"
#include "ragoshell/ssh/ssh.h"

namespace RagoShell
{
    namespace
    {
        constexpr qint64 kHealthySessionResetMs = 2000;

        void wipeSecretString(std::string &secret)
        {
            if (secret.empty())
                return;
            volatile char *ptr = &secret[0];
            for (size_t i = 0; i < secret.size(); ++i)
                ptr[i] = '\0';
            secret.clear();
        }

        rbm_ssh_log_type sanitizeSshLogLevel(int rawLevel)
        {
            switch (rawLevel) {
            case RBM_SSH_LOG_TYPE_ERROR: return RBM_SSH_LOG_TYPE_ERROR;
            case RBM_SSH_LOG_TYPE_WARN:  return RBM_SSH_LOG_TYPE_WARN;
            case RBM_SSH_LOG_TYPE_INFO:  return RBM_SSH_LOG_TYPE_INFO;
            case RBM_SSH_LOG_TYPE_DEBUG: return RBM_SSH_LOG_TYPE_DEBUG;
            default:                     return RBM_SSH_LOG_TYPE_ERROR;
            }
        }

        SshTunnelErrorKind mapSshErrorCode(rbm_ssh_error_code code, SshTunnelErrorKind fallback)
        {
            switch (code) {
            case RBM_SSH_ERR_NONE:       return fallback;
            case RBM_SSH_ERR_NETWORK:    return SshTunnelErrorKind::Network;
            case RBM_SSH_ERR_HOSTKEY:    return SshTunnelErrorKind::HostKey;
            case RBM_SSH_ERR_AUTH:       return SshTunnelErrorKind::Auth;
            case RBM_SSH_ERR_LOCAL_BIND: return SshTunnelErrorKind::LocalBind;
            case RBM_SSH_ERR_STOPPED:    return SshTunnelErrorKind::Stopped;
            }
            return fallback;
        }

        int rejectUnknownOrChangedHostKeyCallback(void *, const rbm_hostkey_info *)
        {
            // Unattended background reconnect: known_hosts exact matches pass inside
            // verify_hostkey() without invoking hostkeycallback. Any new or changed host key
            // is refused before credentials are sent.
            return 0;
        }

        class Libssh2TunnelSession final : public ISshTunnelSession
        {
        public:
            Libssh2TunnelSession(const ConnectionSettings &settings,
                                 std::function<void(const std::string &, int)> logFn)
                : _settings(settings),
                  _logFn(std::move(logFn))
            {
            }

            ~Libssh2TunnelSession() override
            {
                closeSession();
            }

            SshTunnelSetupResult setup(int preferredLocalPort, bool interactiveHostKey) override
            {
                _configCreator = std::make_unique<SshTunnelConfigCreator>(&_settings, preferredLocalPort);
                rbm_ssh_tunnel_config *cfg = _configCreator->config();
                cfg->logcontext = this;
                cfg->logcallback = &Libssh2TunnelSession::sessionLogCallback;
                cfg->loglevel = sanitizeSshLogLevel(_settings.sshSettings()->logLevel());
                cfg->hostkeycontext = nullptr;
                cfg->hostkeycallback = interactiveHostKey
                    ? &SshTunnelWorker::interactiveHostKeyCallback
                    : &rejectUnknownOrChangedHostKeyCallback;

                rbm_ssh_session *rawSession = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    if (_stopRequested)
                        return {false, 0, SshTunnelErrorKind::Stopped, "SSH tunnel stopped"};

                    _sshSession = rbm_ssh_session_create(cfg);
                    if (!_sshSession)
                        return {false, 0, SshTunnelErrorKind::Network, "Failed to create SSH session"};

                    rawSession = _sshSession;
                }

                if (rbm_ssh_session_setup(rawSession) == -1) {
                    const std::string err = rawSession->lasterror ? rawSession->lasterror : "";
                    const SshTunnelErrorKind kind = mapSshErrorCode(rawSession->errorcode, SshTunnelErrorKind::Network);
                    _configCreator->wipeSecrets();
                    closeSession();
                    return {false, 0, kind, err.empty() ? "SSH tunnel setup failed" : err};
                }

                // Credentials are only needed during SSH authentication; scrub them from
                // the session's config buffer before entering the long-lived forwarding loop.
                _configCreator->wipeSecrets();

                return {true, static_cast<int>(cfg->localport), SshTunnelErrorKind::None, {}};
            }

            SshTunnelRunResult runForwarding() override
            {
                rbm_ssh_session *rawSession = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    if (_stopRequested || !_sshSession)
                        return {SshTunnelErrorKind::Stopped, {}};
                    rawSession = _sshSession;
                }

                const int rc = rbm_ssh_open_tunnel(rawSession);
                if (rawSession->errorcode == RBM_SSH_ERR_STOPPED)
                    return {SshTunnelErrorKind::Stopped, {}};

                if (rc != 0) {
                    const std::string err = rawSession->lasterror ? rawSession->lasterror : "";
                    const SshTunnelErrorKind kind = mapSshErrorCode(rawSession->errorcode, SshTunnelErrorKind::Network);
                    return {kind, err.empty() ? "SSH tunnel disconnected" : err};
                }

                return {SshTunnelErrorKind::Stopped, {}};
            }

            void requestStop() override
            {
                std::lock_guard<std::mutex> lock(_sessionMutex);
                _stopRequested = true;
                if (_sshSession)
                    rbm_ssh_session_stop(_sshSession);
            }

        private:
            static void sessionLogCallback(void *context, char *message, int level)
            {
                auto *self = static_cast<Libssh2TunnelSession *>(context);
                if (self && self->_logFn && message)
                    self->_logFn(message, level);
            }

            void closeSession()
            {
                rbm_ssh_session *rawSession = nullptr;
                {
                    std::lock_guard<std::mutex> lock(_sessionMutex);
                    rawSession = _sshSession;
                    _sshSession = nullptr;
                }
                if (rawSession)
                    rbm_ssh_session_close(rawSession);
            }

            const ConnectionSettings &_settings;
            std::function<void(const std::string &, int)> _logFn;
            std::unique_ptr<SshTunnelConfigCreator> _configCreator;
            std::mutex _sessionMutex;
            rbm_ssh_session *_sshSession = nullptr;
            bool _stopRequested = false;
        };
    } // namespace

    int SshReconnectPolicy::delayForAttempt(int attempt) const
    {
        if (initialDelayMs <= 0 || maxDelayMs <= 0)
            return 0;

        const int clampedAttempt = std::max(1, attempt);
        long long delay = initialDelayMs;
        for (int i = 1; i < clampedAttempt; ++i) {
            if (delay >= maxDelayMs) {
                delay = maxDelayMs;
                break;
            }
            delay *= 2;
        }
        if (delay > maxDelayMs)
            delay = maxDelayMs;
        return static_cast<int>(delay);
    }

    SshTunnelWorker::SshTunnelWorker(ConnectionSettings *settings,
                                     SshReconnectPolicy policy,
                                     SshTunnelSessionFactory sessionFactory)
        : QObject(),
          _settings(settings),
          _policy(policy),
          _sessionFactory(std::move(sessionFactory))
    {
        if (!_sessionFactory) {
            _sessionFactory = [](const ConnectionSettings &connSettings,
                                 std::function<void(const std::string &, int)> logFn) {
                return std::make_unique<Libssh2TunnelSession>(connSettings, std::move(logFn));
            };
        }
    }

    SshTunnelWorker::~SshTunnelWorker()
    {
        stop();

        if (_settings && _settings->sshSettings())
            _settings->sshSettings()->wipeSecrets();
    }

    void SshTunnelWorker::startInitialConnect(QObject *replyReceiver, int serverHandle, ConnectionType connectionType)
    {
        std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            if (_userStopped || _workerThread.joinable())
                return;

            _replyReceiver = replyReceiver;
            _serverHandle = serverHandle;
            _connectionType = connectionType;
            _stopCurrentRun = false;
            _nudgeImmediateRetry = false;
            _attemptCount = 0;
            _state = State::InitialConnecting;
        }

        _workerThread = std::thread(&SshTunnelWorker::supervisorThreadMain, this, true, true);
    }

    bool SshTunnelWorker::startStandalone(bool interactiveHostKey)
    {
        std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            if (_userStopped || _workerThread.joinable())
                return false;

            _replyReceiver = nullptr;
            _stopCurrentRun = false;
            _nudgeImmediateRetry = false;
            _attemptCount = 0;
            _state = State::InitialConnecting;
        }

        _workerThread = std::thread(&SshTunnelWorker::supervisorThreadMain, this, true, interactiveHostKey);
        return true;
    }

    bool SshTunnelWorker::restart()
    {
        std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
        return restartLocked();
    }

    bool SshTunnelWorker::restartLocked()
    {
        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            if (_userStopped || _state == State::HaltedFatal)
                return false;

            _stopCurrentRun = true;
            if (_activeSession)
                _activeSession->requestStop();
            _stateCv.notify_all();
        }

        if (_workerThread.joinable())
            _workerThread.join();

        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            if (_userStopped || _state == State::HaltedFatal)
                return false;

            _stopCurrentRun = false;
            _nudgeImmediateRetry = false;
            _attemptCount = 0;
            _lastErrorKind = SshTunnelErrorKind::None;
            _lastErrorMessage.clear();
            _lastRestartSteadyMs = nowSteadyMs();
            _state = State::Reconnecting;
        }

        _workerThread = std::thread(&SshTunnelWorker::supervisorThreadMain, this, false, false);
        return true;
    }

    bool SshTunnelWorker::onNetworkResumed()
    {
        std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
        {
            std::unique_lock<std::mutex> stateLock(_stateMutex);
            if (_userStopped || _state == State::HaltedFatal || _state == State::Idle)
                return false;

            if (_state == State::InitialConnecting)
                return true;

            if (_state == State::BackoffWaiting) {
                _attemptCount = 0;
                _nudgeImmediateRetry = true;
                _lastRestartSteadyMs = nowSteadyMs();
                _stateCv.notify_all();
                return true;
            }

            if (_state == State::Reconnecting) {
                _attemptCount = 0;
                return true;
            }

            if (_state == State::Running && _policy.resumeCooldownMs > 0) {
                const qint64 now = nowSteadyMs();
                const bool freshEstablished =
                    _lastEstablishedSteadyMs > 0 && (now - _lastEstablishedSteadyMs) < _policy.resumeCooldownMs;
                const bool freshRestarted =
                    _lastRestartSteadyMs > 0 && (now - _lastRestartSteadyMs) < _policy.resumeCooldownMs;
                if (freshEstablished || freshRestarted)
                    return true;
            }
        }

        return restartLocked();
    }

    void SshTunnelWorker::stop()
    {
        std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            _userStopped = true;
            _stopCurrentRun = true;
            if (_activeSession)
                _activeSession->requestStop();
            _stateCv.notify_all();
        }

        if (_workerThread.joinable())
            _workerThread.join();

        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            _state = State::Stopped;
        }
    }

    int SshTunnelWorker::localPort() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _boundLocalPort;
    }

    SshTunnelWorker::State SshTunnelWorker::state() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _state;
    }

    bool SshTunnelWorker::isRunning() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _state == State::Running;
    }

    bool SshTunnelWorker::isStopped() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _userStopped;
    }

    bool SshTunnelWorker::isHaltedByFatalError() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _state == State::HaltedFatal;
    }

    SshTunnelErrorKind SshTunnelWorker::lastErrorKind() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _lastErrorKind;
    }

    std::string SshTunnelWorker::lastErrorMessage() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _lastErrorMessage;
    }

    std::vector<int> SshTunnelWorker::recordedBackoffDelaysMs() const
    {
        std::lock_guard<std::mutex> stateLock(_stateMutex);
        return _recordedBackoffDelaysMs;
    }

    void SshTunnelWorker::supervisorThreadMain(bool initialConnect, bool interactiveInitialHostKey)
    {
        std::unique_ptr<ISshTunnelSession> session;

        if (initialConnect) {
            int preferredPort = 0;
            {
                std::lock_guard<std::mutex> stateLock(_stateMutex);
                if (_userStopped || _stopCurrentRun)
                    return;
                preferredPort = _boundLocalPort;
            }

            const SshTunnelSetupResult setupRes = establishSession(&session, preferredPort, interactiveInitialHostKey);
            if (!setupRes.ok) {
                QPointer<QObject> receiver;
                int handle = 0;
                ConnectionType connType = ConnectionPrimary;
                bool wasUserStopped = false;
                {
                    std::lock_guard<std::mutex> stateLock(_stateMutex);
                    _activeSession = nullptr;
                    _lastErrorKind = setupRes.errorKind;
                    _lastErrorMessage = setupRes.errorMessage;
                    wasUserStopped = _userStopped || _stopCurrentRun ||
                                     setupRes.errorKind == SshTunnelErrorKind::Stopped;
                    if (wasUserStopped) {
                        _state = _userStopped ? State::Stopped : State::Idle;
                    } else if (setupRes.errorKind == SshTunnelErrorKind::HostKey ||
                               setupRes.errorKind == SshTunnelErrorKind::Auth) {
                        _state = State::HaltedFatal;
                    } else {
                        _state = State::Exhausted;
                    }
                    receiver = _replyReceiver;
                    handle = _serverHandle;
                    connType = _connectionType;
                }
                session.reset();

                if (!wasUserStopped) {
                    std::stringstream ss;
                    ss << "Failed to create SSH tunnel to "
                       << _settings->sshSettings()->host() << ":"
                       << _settings->sshSettings()->port() << ".\n\nError:\n"
                       << setupRes.errorMessage;
                    const std::string fullMessage = ss.str();

                    if (receiver) {
                        reply(receiver.data(),
                              new EstablishSshConnectionResponse(this, handle, EventError(fullMessage),
                                                                 this, _settings.get(), connType));
                    }
                    Q_EMIT initialConnectFinished(false, 0, static_cast<int>(setupRes.errorKind),
                                                  QString::fromStdString(fullMessage));
                }
                return;
            }

            QPointer<QObject> receiver;
            int handle = 0;
            ConnectionType connType = ConnectionPrimary;
            bool stopNow = false;
            {
                std::lock_guard<std::mutex> stateLock(_stateMutex);
                stopNow = _userStopped || _stopCurrentRun;
                if (stopNow) {
                    _activeSession = nullptr;
                } else {
                    _boundLocalPort = setupRes.boundLocalPort;
                    _attemptCount = 0;
                    _lastErrorKind = SshTunnelErrorKind::None;
                    _lastErrorMessage.clear();
                    _lastEstablishedSteadyMs = nowSteadyMs();
                    _state = State::Running;
                    receiver = _replyReceiver;
                    handle = _serverHandle;
                    connType = _connectionType;
                }
            }

            if (stopNow) {
                session.reset();
                return;
            }

            if (receiver) {
                reply(receiver.data(),
                      new EstablishSshConnectionResponse(this, handle, this, _settings.get(),
                                                         connType, setupRes.boundLocalPort));
            }
            Q_EMIT initialConnectFinished(true, setupRes.boundLocalPort,
                                          static_cast<int>(SshTunnelErrorKind::None), QString());
        }

        while (true) {
            if (!session) {
                int preferredPort = 0;
                {
                    std::lock_guard<std::mutex> stateLock(_stateMutex);
                    if (_userStopped || _stopCurrentRun)
                        break;
                    _state = State::Reconnecting;
                    preferredPort = _boundLocalPort;
                }

                const SshTunnelSetupResult setupRes = establishSession(&session, preferredPort, false);
                if (!setupRes.ok) {
                    int delayMs = 0;
                    {
                        std::unique_lock<std::mutex> stateLock(_stateMutex);
                        _activeSession = nullptr;
                        if (_userStopped || _stopCurrentRun || setupRes.errorKind == SshTunnelErrorKind::Stopped)
                            break;

                        _lastErrorKind = setupRes.errorKind;
                        _lastErrorMessage = setupRes.errorMessage;

                        if (setupRes.errorKind == SshTunnelErrorKind::HostKey ||
                            setupRes.errorKind == SshTunnelErrorKind::Auth) {
                            _state = State::HaltedFatal;
                            const std::string errCopy = setupRes.errorMessage;
                            const SshTunnelErrorKind kindCopy = setupRes.errorKind;
                            stateLock.unlock();
                            session.reset();
                            log("SSH tunnel reconnect halted due to host-key or authentication rejection (" +
                                    _settings->sshSettings()->host() + ":" +
                                    std::to_string(_settings->sshSettings()->port()) + ")",
                                RBM_SSH_LOG_TYPE_ERROR);
                            Q_EMIT tunnelPermanentlyFailed(static_cast<int>(kindCopy),
                                                           QString::fromStdString(errCopy));
                            return;
                        }

                        ++_attemptCount;
                        if (_attemptCount >= _policy.maxAttempts) {
                            _state = State::Exhausted;
                            const std::string errCopy = setupRes.errorMessage;
                            const SshTunnelErrorKind kindCopy = setupRes.errorKind;
                            stateLock.unlock();
                            session.reset();
                            log("SSH tunnel reconnect exhausted retry budget (" +
                                    _settings->sshSettings()->host() + ":" +
                                    std::to_string(_settings->sshSettings()->port()) + ")",
                                RBM_SSH_LOG_TYPE_WARN);
                            Q_EMIT tunnelPermanentlyFailed(static_cast<int>(kindCopy),
                                                           QString::fromStdString(errCopy));
                            return;
                        }

                        delayMs = _policy.delayForAttempt(_attemptCount);
                    }
                    session.reset();

                    log("SSH tunnel reconnect attempt failed for " +
                            _settings->sshSettings()->host() + ":" +
                            std::to_string(_settings->sshSettings()->port()) + "; scheduling retry",
                        RBM_SSH_LOG_TYPE_WARN);

                    if (!waitBackoffInterruptible(delayMs))
                        break;
                    continue;
                }

                bool portChanged = false;
                bool stopNow = false;
                {
                    std::lock_guard<std::mutex> stateLock(_stateMutex);
                    stopNow = _userStopped || _stopCurrentRun;
                    if (stopNow) {
                        _activeSession = nullptr;
                    } else {
                        portChanged = (_boundLocalPort != 0 && setupRes.boundLocalPort != _boundLocalPort);
                        _boundLocalPort = setupRes.boundLocalPort;
                        ++_attemptCount;
                        _lastErrorKind = SshTunnelErrorKind::None;
                        _lastErrorMessage.clear();
                        _lastEstablishedSteadyMs = nowSteadyMs();
                        _state = State::Running;
                    }
                }

                if (stopNow) {
                    session.reset();
                    break;
                }

                log("SSH tunnel re-established on 127.0.0.1:" + std::to_string(setupRes.boundLocalPort),
                    RBM_SSH_LOG_TYPE_INFO);
                Q_EMIT tunnelReconnected(setupRes.boundLocalPort, portChanged);
            }

            const qint64 runStartedMs = nowSteadyMs();
            const SshTunnelRunResult runRes = session->runForwarding();
            const qint64 runDurationMs = nowSteadyMs() - runStartedMs;

            {
                std::lock_guard<std::mutex> stateLock(_stateMutex);
                _activeSession = nullptr;
            }
            // Close the old SSH session and its 127.0.0.1:<localPort> listener socket BEFORE
            // any backoff wait or reconnect attempt.
            session.reset();

            int flapDelayMs = 0;
            {
                std::unique_lock<std::mutex> stateLock(_stateMutex);
                if (_userStopped || _stopCurrentRun || runRes.errorKind == SshTunnelErrorKind::Stopped)
                    break;

                _lastErrorKind = (runRes.errorKind == SshTunnelErrorKind::None)
                    ? SshTunnelErrorKind::Network
                    : runRes.errorKind;
                _lastErrorMessage = runRes.errorMessage;
                _state = State::Reconnecting;

                if (runDurationMs >= kHealthySessionResetMs) {
                    _attemptCount = 0;
                } else if (_attemptCount > 0) {
                    if (_attemptCount >= _policy.maxAttempts) {
                        _state = State::Exhausted;
                        const std::string errCopy = _lastErrorMessage;
                        const SshTunnelErrorKind kindCopy = _lastErrorKind;
                        stateLock.unlock();
                        log("SSH tunnel halted after repeated rapid disconnects (" +
                                _settings->sshSettings()->host() + ":" +
                                std::to_string(_settings->sshSettings()->port()) + ")",
                            RBM_SSH_LOG_TYPE_WARN);
                        Q_EMIT tunnelPermanentlyFailed(static_cast<int>(kindCopy),
                                                       QString::fromStdString(errCopy));
                        return;
                    }
                    flapDelayMs = _policy.delayForAttempt(_attemptCount);
                }
            }

            log("SSH tunnel connection dropped (" +
                    _settings->sshSettings()->host() + ":" +
                    std::to_string(_settings->sshSettings()->port()) + "); reconnecting",
                RBM_SSH_LOG_TYPE_WARN);

            if (flapDelayMs > 0 && !waitBackoffInterruptible(flapDelayMs))
                break;
        }

        std::lock_guard<std::mutex> stateLock(_stateMutex);
        _activeSession = nullptr;
        if (_userStopped)
            _state = State::Stopped;
    }

    SshTunnelSetupResult SshTunnelWorker::establishSession(std::unique_ptr<ISshTunnelSession> *outSession,
                                                           int preferredPort,
                                                           bool interactiveHostKey)
    {
        auto logBridge = [this](const std::string &msg, int lvl) { log(msg, lvl); };
        std::unique_ptr<ISshTunnelSession> candidate = _sessionFactory(*_settings, logBridge);
        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            if (_userStopped || _stopCurrentRun)
                return {false, 0, SshTunnelErrorKind::Stopped, "SSH tunnel stopped"};
            _activeSession = candidate.get();
        }

        SshTunnelSetupResult res = candidate->setup(preferredPort, interactiveHostKey);
        if (res.ok) {
            *outSession = std::move(candidate);
            return res;
        }

        {
            std::lock_guard<std::mutex> stateLock(_stateMutex);
            _activeSession = nullptr;
            if (_userStopped || _stopCurrentRun)
                return {false, 0, SshTunnelErrorKind::Stopped, "SSH tunnel stopped"};
        }
        candidate.reset();

        // Fallback: if another OS process claimed our previous ephemeral 127.0.0.1:<preferredPort>
        // while the tunnel was down, retry once with port 0 to allocate a fresh loopback port.
        if (res.errorKind == SshTunnelErrorKind::LocalBind && preferredPort > 0) {
            log("Previous SSH local port 127.0.0.1:" + std::to_string(preferredPort) +
                    " is occupied; binding a new loopback port",
                RBM_SSH_LOG_TYPE_WARN);

            std::unique_ptr<ISshTunnelSession> fallbackCandidate = _sessionFactory(*_settings, logBridge);
            {
                std::lock_guard<std::mutex> stateLock(_stateMutex);
                if (_userStopped || _stopCurrentRun)
                    return {false, 0, SshTunnelErrorKind::Stopped, "SSH tunnel stopped"};
                _activeSession = fallbackCandidate.get();
            }

            SshTunnelSetupResult fallbackRes = fallbackCandidate->setup(0, interactiveHostKey);
            if (fallbackRes.ok) {
                *outSession = std::move(fallbackCandidate);
                return fallbackRes;
            }

            {
                std::lock_guard<std::mutex> stateLock(_stateMutex);
                _activeSession = nullptr;
            }
            fallbackCandidate.reset();
            return fallbackRes;
        }

        return res;
    }

    bool SshTunnelWorker::waitBackoffInterruptible(int delayMs)
    {
        std::unique_lock<std::mutex> stateLock(_stateMutex);
        if (_userStopped || _stopCurrentRun)
            return false;

        _recordedBackoffDelaysMs.push_back(delayMs);
        if (delayMs <= 0)
            return true;

        _state = State::BackoffWaiting;
        const bool interrupted = _stateCv.wait_for(
            stateLock,
            std::chrono::milliseconds(delayMs),
            [this]() { return _userStopped || _stopCurrentRun || _nudgeImmediateRetry; });

        if (_userStopped || _stopCurrentRun)
            return false;

        if (interrupted && _nudgeImmediateRetry)
            _nudgeImmediateRetry = false;

        return true;
    }

    void SshTunnelWorker::reply(QObject *receiver, Event *event)
    {
        if (!receiver || !QCoreApplication::instance() || isStopped()) {
            delete event;
            return;
        }

        EventBus *bus = AppRegistry::instance().bus();
        if (!bus) {
            delete event;
            return;
        }
        bus->send(receiver, event);
    }

    void SshTunnelWorker::log(const std::string &message, int level)
    {
        if (!QCoreApplication::instance() || isStopped())
            return;

        // Only forward logs when an EventBus receiver exists in a live App instance.
        if (!_replyReceiver)
            return;

        EventBus *bus = AppRegistry::instance().bus();
        App *app = AppRegistry::instance().app();
        if (!bus || !app)
            return;

        bus->send(app, new LogEvent(this, message, static_cast<LogEvent::LogLevel>(level)));
    }

    void SshTunnelWorker::logCallbackHandler(void *context, char *message, int level)
    {
        auto *worker = static_cast<SshTunnelWorker *>(context);
        if (worker && message)
            worker->log(message, level);
    }

    int SshTunnelWorker::interactiveHostKeyCallback(void *, const rbm_hostkey_info *info)
    {
        HostKeyQuestion question;
        question.host = QString::fromUtf8(info->host);
        question.port = static_cast<int>(info->port);
        question.keyType = QString::fromUtf8(info->keytype);
        question.fingerprint = hostKeyFingerprint(info->sha256);
        question.changed = info->status == RBM_HOSTKEY_CHANGED;
        // Blocks this worker thread until the user answers; no handler or no answer: not trusted.
        return HostKeyPrompter::instance().ask(question) ? 1 : 0;
    }

    qint64 SshTunnelWorker::nowSteadyMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /*
     * SshTunnelConfigCreator
     */
    SshTunnelConfigCreator::SshTunnelConfigCreator(const ConnectionSettings *settings, int preferredLocalPort)
    {
        const SshSettings *ssh = settings->sshSettings();

        _sshhost = ssh->host();
        _sshport = ssh->port();
        _remotehost = settings->serverHost();
        _remoteport = settings->serverPort();
        _localip = "127.0.0.1";
        _localport = (preferredLocalPort > 0 && preferredLocalPort <= 65535) ? preferredLocalPort : 0;

        _userName = ssh->userName();
        _userPassword = ssh->userPassword();
        _privateKeyFile = ssh->privateKeyFile();
        _publicKeyFile = ssh->publicKeyFile();
        _passphrase = ssh->passphrase();
        _authMethod = ssh->authMethod(); // "password" or "publickey"

        // Use "askedPassword" for both passphrase and password if required
        if (ssh->askPassword()) {
            _passphrase = ssh->askedPassword();
            _userPassword = ssh->askedPassword();
        }

        _sshConfig = new rbm_ssh_tunnel_config;
        _sshConfig->sshserverhost = const_cast<char *>(_sshhost.c_str());
        _sshConfig->sshserverport = static_cast<unsigned int>(_sshport);
        _sshConfig->remotehost = const_cast<char *>(_remotehost.c_str());
        _sshConfig->remoteport = static_cast<unsigned int>(_remoteport);
        _sshConfig->localip = const_cast<char *>(_localip.c_str());
        _sshConfig->localport = static_cast<unsigned int>(_localport);

        _sshConfig->username = const_cast<char *>(_userName.c_str());
        _sshConfig->password = const_cast<char *>(_userPassword.c_str());
        _sshConfig->privatekeyfile = const_cast<char *>(_privateKeyFile.c_str());
        _sshConfig->publickeyfile = _publicKeyFile.empty() ? nullptr : const_cast<char *>(_publicKeyFile.c_str());
        _sshConfig->passphrase = const_cast<char *>(_passphrase.c_str());
        _sshConfig->authtype = (_authMethod == "publickey")
            ? RBM_SSH_AUTH_TYPE_PUBLICKEY
            : RBM_SSH_AUTH_TYPE_PASSWORD;

        // The server's host key is checked against ssh/known_hosts in the settings folder;
        // the worker sets the callback that asks the user about a new or changed key.
        QDir().mkpath(appDataDir() + QStringLiteral("ssh"));
        _knownHostsFile = (appDataDir() + QStringLiteral("ssh/known_hosts")).toStdString();
        _sshConfig->knownhostsfile = const_cast<char *>(_knownHostsFile.c_str());
        _sshConfig->hostkeycontext = nullptr;
        _sshConfig->hostkeycallback = nullptr;

        _sshConfig->logcontext = nullptr;
        _sshConfig->logcallback = nullptr;
        _sshConfig->loglevel = RBM_SSH_LOG_TYPE_ERROR;
    }

    SshTunnelConfigCreator::~SshTunnelConfigCreator()
    {
        wipeSecrets();
        delete _sshConfig;
    }

    void SshTunnelConfigCreator::wipeSecrets()
    {
        wipeSecretString(_userPassword);
        wipeSecretString(_passphrase);
        if (_sshConfig) {
            _sshConfig->password = const_cast<char *>(_userPassword.c_str());
            _sshConfig->passphrase = const_cast<char *>(_passphrase.c_str());
        }
    }
} // namespace RagoShell
