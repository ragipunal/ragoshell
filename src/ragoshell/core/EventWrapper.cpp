#include "ragoshell/core/EventWrapper.h"
namespace RagoShell
{
    EventWrapper::EventWrapper(Event *event, QList<QObject *> receivers) 
        : QEvent(event->type()), _event(event)
    {
        _receivers.reserve(receivers.size());
        for (QObject *r : receivers)
            _receivers.append(QPointer<QObject>(r));
    }

    EventWrapper::EventWrapper(Event *event, QObject * receiver)
        : QEvent(event->type()), _event(event)
    {
        _receivers.append(QPointer<QObject>(receiver));
    }

    Event *EventWrapper::event() const 
    {
        return _event.get(); 
    }

    const QList<QPointer<QObject>> &EventWrapper::receivers() const 
    {
        return _receivers;
    }
}
