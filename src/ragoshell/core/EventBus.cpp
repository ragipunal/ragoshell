#include "ragoshell/core/EventBus.h"

#include <QObject>
#include <QCoreApplication>
#include <QThread>
#include <QMutexLocker>

#include "ragoshell/core/EventBusDispatcher.h"
#include "ragoshell/core/EventBusSubscriber.h"
#include "ragoshell/core/Event.h"
#include "ragoshell/core/EventWrapper.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace
{
    struct RemoveIfReciver
    {
        RemoveIfReciver(QObject *receiver) : _receiver(receiver) {}

        bool operator()(const RagoShell::EventBus::EventTypeAndSubscriber& item) const {
            if (item.second->receiver == _receiver) {
                delete item.second;
                return true;
            }
            return false;
        }

        QObject *_receiver;
    };

    struct FindIfReciver
    {
        FindIfReciver(QThread *thread) : _thread(thread) {}
        bool operator()(const RagoShell::EventBus::ThreadAndDispatcher &item) const {
            return item.first == _thread && item.second && item.second->thread() == _thread;
        }
        QThread *_thread;
    };
}

namespace RagoShell
{
    EventBus::EventBus() : QObject(),
        _lock()
    {
    }

    EventBus::~EventBus()
    {
        QMutexLocker lock(&_lock);
        for (auto const& dispatchersByThread : _dispatchersByThread) {
            if (dispatchersByThread.first)
                QObject::disconnect(dispatchersByThread.first, nullptr, this, nullptr);
            EventBusDispatcher *dis = dispatchersByThread.second.data();
            if (!dis)
                continue;
            QObject::disconnect(dis, nullptr, this, nullptr);
            if (dis->thread() == QThread::currentThread() || !dis->thread())
                delete dis;
            else
                dis->deleteLater();
        }
        _dispatchersByThread.clear();
    }

    void EventBus::publish(Event *event)
    {
        QMutexLocker lock(&_lock);
        QList<QObject*> theReceivers;
        EventBusDispatcher *dis = nullptr;
        for (auto const& item : _subscribersByEventType) {            
            if (event->type() == item.first) {
                EventBusSubscriber *subscriber = item.second;
                if (!subscriber->sender || subscriber->sender == event->sender()) {
                    theReceivers.append(subscriber->receiver);

                    if (dis && dis != subscriber->dispatcher)
                        throw "You cannot publish events to subscribers from more than one thread.";

                    dis = subscriber->dispatcher;
                }
            }
        }

        if (dis)
            sendEvent(dis, new EventWrapper(event, theReceivers));
    }

    void EventBus::send(QObject *receiver, Event *event)
    {
        QMutexLocker lock(&_lock);

        if (!receiver)
            return;

        QThread *thread = receiver->thread();
        EventBusDispatcher *dis = dispatcher(thread);

        sendEvent(dis, new EventWrapper(event, receiver));
    }

    void EventBus::send(QList<QObject *> receivers, Event *event)
    {
        QMutexLocker lock(&_lock);

        if (receivers.count() == 0)
            return;

        QThread *thread = receivers.last()->thread();
        EventBusDispatcher *dis = dispatcher(thread);

        sendEvent(dis, new EventWrapper(event, receivers));
    }

    void EventBus::subscribe(QObject *receiver, QEvent::Type type, QObject *sender /* = NULL */)
    {
        QMutexLocker lock(&_lock);

        QThread *currentThread = QThread::currentThread();
        EventBusDispatcher *dis = dispatcher(currentThread);

        // subscribe to destroyed signal in order to remove
        // listener (receiver) from list of subscribers
        VERIFY(connect(receiver, SIGNAL(destroyed(QObject*)), this, SLOT(unsubscibe(QObject*))));

        // add subscriber
        _subscribersByEventType.push_back(EventTypeAndSubscriber(type, new EventBusSubscriber(dis, receiver, sender)));
    }

    void EventBus::unsubscibe(QObject *receiver)
    {
        QMutexLocker lock(&_lock);
        _subscribersByEventType.erase(std::remove_if(_subscribersByEventType.begin(), _subscribersByEventType.end(),
            RemoveIfReciver(receiver)), _subscribersByEventType.end());
    }

    /**
     * @brief Returns dispatcher for specified thread. If there is no dispatcher
     * for this thread registered, it will be created and moved to 'thread' thread.
     */
    EventBusDispatcher *EventBus::dispatcher(QThread *thread)
    {
        _dispatchersByThread.erase(
            std::remove_if(_dispatchersByThread.begin(), _dispatchersByThread.end(),
                           [](const ThreadAndDispatcher &item) {
                               return !item.first || !item.second || item.second->thread() != item.first;
                           }),
            _dispatchersByThread.end());

        auto const& disIt = std::find_if(
            _dispatchersByThread.begin(), _dispatchersByThread.end(), FindIfReciver(thread)
        );
       
        if (disIt != _dispatchersByThread.end()) {
            return disIt->second.data();
        }
        else {
            EventBusDispatcher *dis = new EventBusDispatcher();
            dis->moveToThread(thread);
            _dispatchersByThread.push_back(ThreadAndDispatcher(thread, dis));

            if (thread && (!QCoreApplication::instance() || thread != QCoreApplication::instance()->thread())) {
                QObject::connect(thread, &QThread::finished, dis, &QObject::deleteLater);
            }
            if (thread) {
                QObject::connect(thread, &QObject::destroyed, this, [this, thread](QObject *) {
                    QMutexLocker lock(&_lock);
                    _dispatchersByThread.erase(
                        std::remove_if(_dispatchersByThread.begin(), _dispatchersByThread.end(),
                                       [thread](const ThreadAndDispatcher &item) {
                                           return item.first == thread || !item.second;
                                       }),
                        _dispatchersByThread.end());
                }, Qt::DirectConnection);
            }
            QObject::connect(dis, &QObject::destroyed, this, [this, dis](QObject *) {
                QMutexLocker lock(&_lock);
                _dispatchersByThread.erase(
                    std::remove_if(_dispatchersByThread.begin(), _dispatchersByThread.end(),
                                   [dis](const ThreadAndDispatcher &item) {
                                       return item.second.data() == dis || !item.second;
                                   }),
                    _dispatchersByThread.end());
            }, Qt::DirectConnection);

            return dis;
        }        
    }

    /**
     * @brief Sends event synchronousely, if current thread and dispatcher thread are
     * the same. Sends asynchronousely, if this is cross-thread communication;
     */
    void EventBus::sendEvent(EventBusDispatcher *dispatcher, EventWrapper *wrapper)
    {
        if (dispatcher->thread() == QThread::currentThread()) {
            QCoreApplication::sendEvent(dispatcher, wrapper);
            delete wrapper;
        }
        else {
            QCoreApplication::postEvent(dispatcher, wrapper);
        }
    }

}
