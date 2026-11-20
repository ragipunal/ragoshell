#include "ragoshell/core/EventBusSubscriber.h"
#include "ragoshell/core/EventBusDispatcher.h"

namespace RagoShell
{
    EventBusSubscriber::EventBusSubscriber(EventBusDispatcher *dispatcher, QObject *receiver, QObject *sender) :
        receiver(receiver),
        dispatcher(dispatcher),
        sender(sender) {}
}
