#pragma once

#include <QtQml/qqmlregistration.h>

#include "ragoshell-app/ConnectionListModel.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlUiServices.h"
#include "ragoshell-app/ServerMonitorController.h"
#include "ragoshell-app/ShellController.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    /**
     * @brief Registers QmlBackend as the QML singleton "Backend".
     *
     * Kept out of QmlBackend itself: QML_ELEMENT makes moc require complete
     * types for every slot argument, and QmlBackend's EventBus handle()
     * slots take forward-declared event types.
     */
    struct QmlBackendForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::QmlBackend)
        QML_NAMED_ELEMENT(Backend)
        QML_SINGLETON

    public:
        static QmlBackend *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<QmlBackend>::get();
        }
    };

    /// Saved connections as the QML singleton "ConnectionModel" (foreign:
    /// the model already has an invokable create(QVariantMap)).
    struct ConnectionListModelForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::ConnectionListModel)
        QML_NAMED_ELEMENT(ConnectionModel)
        QML_SINGLETON

    public:
        static ConnectionListModel *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<ConnectionListModel>::get();
        }
    };

    /// File/text/message requests from core code as the QML singleton "UiServices".
    struct QmlUiServicesForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::QmlUiServices)
        QML_NAMED_ELEMENT(UiServices)
        QML_SINGLETON

    public:
        static QmlUiServices *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<QmlUiServices>::get();
        }
    };

    // Types QML only receives from Backend: registered (uncreatable) so
    // properties can be typed and qmlsc can compile bindings that use them.
    struct ServerTreeModelForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::ServerTreeModel)
        QML_NAMED_ELEMENT(ServerTreeModel)
        QML_UNCREATABLE("Provided by Backend.serverTree")
    };

    struct LogModelForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::LogModel)
        QML_NAMED_ELEMENT(LogModel)
        QML_UNCREATABLE("Provided by Backend.logModel")
    };

    struct QmlObjectListModelForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::QmlObjectListModel)
        QML_NAMED_ELEMENT(QmlObjectListModel)
        QML_UNCREATABLE("Provided by Backend.shellModel / monitorModel")
    };

    struct TabModelForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::TabModel)
        QML_NAMED_ELEMENT(TabModel)
        QML_UNCREATABLE("Provided by Backend.tabs")
    };

    struct ShellControllerForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::ShellController)
        QML_NAMED_ELEMENT(ShellController)
        QML_UNCREATABLE("Provided by Backend shell tabs")
    };

    struct ServerMonitorControllerForeign
    {
        Q_GADGET
        QML_FOREIGN(RagoShell::ServerMonitorController)
        QML_NAMED_ELEMENT(ServerMonitorController)
        QML_UNCREATABLE("Provided by Backend monitor tabs")
    };
}
