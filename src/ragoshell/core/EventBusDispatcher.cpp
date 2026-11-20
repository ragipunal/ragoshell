#include "ragoshell/core/EventBusDispatcher.h"
#include "ragoshell/core/EventWrapper.h"

namespace RagoShell
{

    EventBusDispatcher::EventBusDispatcher(QObject *parent) :
        QObject(parent)
    {

    }

    bool EventBusDispatcher::event(QEvent *qevent)
    {
        EventWrapper *wrapper = dynamic_cast<EventWrapper *>(qevent);

        if (!wrapper)
            return false;

        Event *event = wrapper->event();

        const char *typeName = event->typeString();
        const QList<QPointer<QObject>> &recivers = wrapper->receivers();
        for (const QPointer<QObject> &target : recivers) {
            if (target)
                QMetaObject::invokeMethod(target.data(), "handle", QGenericArgument(typeName, &event));
        }

        return true;
    }
}
