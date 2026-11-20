#include "ragoshell/core/utils/HostKeyPrompter.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutexLocker>

namespace RagoShell
{
    QString hostKeyFingerprint(const unsigned char *sha256)
    {
        if (!sha256)
            return QString();
        const QByteArray hash(reinterpret_cast<const char *>(sha256), 32);
        return QStringLiteral("SHA256:")
               + QString::fromLatin1(hash.toBase64(QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals));
    }

    HostKeyPrompter &HostKeyPrompter::instance()
    {
        static HostKeyPrompter prompter;
        return prompter;
    }

    void HostKeyPrompter::setHandler(std::function<void(const HostKeyQuestion &)> handler)
    {
        QMutexLocker lock(&_mutex);
        _handler = std::move(handler);
        if (!_handler && !_pending.isEmpty()) {
            for (auto it = _pending.begin(); it != _pending.end(); ++it) {
                it->answered = true;
                it->trust = false;
            }
            _answered.wakeAll();
        }
    }

    bool HostKeyPrompter::ask(HostKeyQuestion question, int timeoutMs)
    {
        std::function<void(const HostKeyQuestion &)> handler;
        {
            QMutexLocker lock(&_mutex);
            handler = _handler;
            if (!handler)
                return false;
            question.id = _nextId++;
            _pending.insert(question.id, Pending{});
        }

        handler(question);   // outside the lock: it may answer at once

        QMutexLocker lock(&_mutex);
        bool trust = false;
        const int id = question.id;
        QElapsedTimer timer;
        timer.start();
        while (!_pending.value(id).answered) {
            const qint64 left = timeoutMs - timer.elapsed();
            if (left <= 0 || !_answered.wait(&_mutex, static_cast<unsigned long>(left)))
                break;
        }
        trust = _pending.value(id).answered && _pending.value(id).trust;
        _pending.remove(id);
        return trust;
    }

    void HostKeyPrompter::answer(int id, bool trust)
    {
        QMutexLocker lock(&_mutex);
        auto it = _pending.find(id);
        if (it == _pending.end() || it->answered)
            return;
        it->answered = true;
        it->trust = trust;
        _answered.wakeAll();
    }
}
