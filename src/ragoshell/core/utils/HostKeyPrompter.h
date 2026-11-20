#pragma once

#include <functional>

#include <QHash>
#include <QMutex>
#include <QString>
#include <QWaitCondition>

namespace RagoShell
{
    /// What the user is asked about an SSH server's host key.
    struct HostKeyQuestion
    {
        int id = 0;
        QString host;
        int port = 22;
        QString keyType;       ///< "ssh-ed25519", "ssh-rsa", ...
        QString fingerprint;   ///< "SHA256:..." (as OpenSSH shows it)
        bool changed = false;  ///< The host is known with a different key.
    };

    /// "SHA256:" and the unpadded base64 of the 32-byte hash, as ssh-keygen -l prints it.
    QString hostKeyFingerprint(const unsigned char *sha256);

    /**
     * @brief Hands the question "trust this SSH host key?" from the tunnel's
     *        worker thread to the UI and blocks the worker until it is answered.
     *
     * The UI installs a handler (it is called on the worker thread and must
     * only queue the question to the UI thread) and later calls answer().
     * Without a handler, and when the user does not answer in time, the key is
     * not trusted.
     */
    class HostKeyPrompter
    {
    public:
        static HostKeyPrompter &instance();

        void setHandler(std::function<void(const HostKeyQuestion &)> handler);

        /// Blocks until answered; false = refuse. Called by the SSH worker thread.
        bool ask(HostKeyQuestion question, int timeoutMs = kDefaultTimeoutMs);
        /// Called by the UI. Unknown or already answered ids are ignored.
        void answer(int id, bool trust);

        static constexpr int kDefaultTimeoutMs = 120 * 1000;

    private:
        struct Pending
        {
            bool answered = false;
            bool trust = false;
        };

        QMutex _mutex;
        QWaitCondition _answered;
        std::function<void(const HostKeyQuestion &)> _handler;
        QHash<int, Pending> _pending;
        int _nextId = 1;
    };
}
