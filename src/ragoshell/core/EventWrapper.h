#pragma once
#include <memory>
#include <QPointer>
#include "ragoshell/core/Event.h"

namespace RagoShell
{
    class EventWrapper : public QEvent
    {
    public:
        EventWrapper(Event *event, QList<QObject *> receivers);
        EventWrapper(Event *event, QObject * receiver);
        Event *event() const;
        const QList<QPointer<QObject>> &receivers() const;

    private:
        const std::unique_ptr<Event> _event;
        QList<QPointer<QObject>> _receivers;
    };
}
