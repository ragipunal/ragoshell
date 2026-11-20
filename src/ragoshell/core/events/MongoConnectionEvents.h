#pragma once

#include <cstdint>

#include <QString>
#include <QStringList>
#include <QEvent>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>

#include "ragoshell/core/domain/MongoShellResult.h"
#include "ragoshell/core/domain/CursorPosition.h"
#include "ragoshell/core/domain/MongoUser.h"
#include "ragoshell/core/domain/MongoFunction.h"
#include "ragoshell/core/events/MongoEventsInfo.h"
#include "ragoshell/core/domain/MongoAggregateInfo.h"
#include "ragoshell/core/Event.h"
#include "ragoshell/core/Enums.h"
#include "ragoshell/core/mongodb/ReplicaSet.h"

namespace RagoShell
{
    class MongoServer;
    class MongoShell;
    class MongoDatabase;
    class MongoWorker;
    class ConnectionSettings;
    class SshTunnelWorker;

    /**
     * @brief EstablishConnection
     */

    struct EstablishConnectionRequest : public Event
    {
        R_EVENT

            EstablishConnectionRequest(QObject *sender, ConnectionType connectionType, std::string const& uuid) :
            Event(sender),
            connectionType(connectionType),
            uuid(uuid) {}

        ConnectionType const connectionType;
        std::string const uuid;
    };

    struct EstablishConnectionResponse : public Event
    {
        R_EVENT

        enum ErrorReason {
            NoError             = 0,
            MongoConnection     = 1,
            MongoAuth           = 2,
            MongoSslConnection  = 3
        };

        EstablishConnectionResponse(QObject *sender, const ConnectionInfo &info, ConnectionType connectionType, 
                                    const ReplicaSet& replicaSet) :
            Event(sender),
            info(info),
            connectionType(connectionType),
            replicaSet(replicaSet)
            {}

        EstablishConnectionResponse(QObject *sender, const EventError &error, ConnectionType connectionType, 
                                    ConnectionInfo const& info, const ReplicaSet& replicaSet, ErrorReason errorReason) :
            Event(sender, error),
            info(info),
            connectionType(connectionType),
            errorReason(errorReason),
            replicaSet(replicaSet)
        {}

        ConnectionInfo const info;
        ConnectionType const connectionType;
        ErrorReason const errorReason = NoError;
        ReplicaSet const replicaSet;
    };

    struct ReplicaSetRefreshed : public Event
    {
        R_EVENT

        ReplicaSetRefreshed(QObject *sender) :
            Event(sender) {}

        ReplicaSetRefreshed(QObject *sender, const EventError &error, ReplicaSet const& replicaSet) :
            Event(sender, error), replicaSet(replicaSet) {}

        ReplicaSet const replicaSet;
    };

    struct RefreshReplicaSetFolderRequest : public Event
    {
        R_EVENT

        RefreshReplicaSetFolderRequest(QObject *sender, bool expanded) :
            Event(sender), expanded(expanded) {}

        bool const expanded = false;
    };

    struct RefreshReplicaSetFolderResponse : public Event
    {
        R_EVENT

        // Primary is reachable
        RefreshReplicaSetFolderResponse(QObject *sender, ReplicaSet const& replicaSet, bool expanded) :
            Event(sender), replicaSet(replicaSet), expanded(expanded) {}
        
        // Primary is unreachable, secondary(ies) might be reachable
        RefreshReplicaSetFolderResponse(QObject *sender, ReplicaSet const&  replicaSet, bool expanded,
                                        const EventError &error) :
            Event(sender, error), replicaSet(replicaSet), expanded(expanded) {}

        ReplicaSet const replicaSet;
        bool const expanded = false;
    };

    struct ReplicaSetFolderLoading : public Event
    {
        R_EVENT

            ReplicaSetFolderLoading(QObject *sender) :
            Event(sender) {}
    };

    struct ReplicaSetFolderRefreshed : public Event
    {
        R_EVENT

        ReplicaSetFolderRefreshed(QObject *sender, bool expanded) :
            Event(sender), expanded(expanded) {}

        ReplicaSetFolderRefreshed(QObject *sender, const EventError &error, bool expanded) :
            Event(sender, error), expanded(expanded) {}

        ReplicaSetFolderRefreshed(QObject *sender, const EventError &error, ReplicaSet const& replicaSet,
                                  bool expanded) :
            Event(sender, error), replicaSet(replicaSet), expanded(expanded) {}

        ReplicaSet const replicaSet;
        bool const expanded = false;
    };


    class ConnectingEvent : public Event
    {
        R_EVENT

        ConnectingEvent(QObject *sender) :
            Event(sender) { }
    };

    class OpeningShellEvent : public Event
    {
        R_EVENT

        OpeningShellEvent(QObject *sender, MongoShell *shell) :
            Event(sender),
            shell(shell) {}

        MongoShell *shell;
    };

    class ConnectionFailedEvent : public Event
    {
        R_EVENT

        enum Reason {
            MongoConnection    = 1,
            MongoAuth          = 2,
            SshConnection      = 3,
            SshChannel         = 4,
            SslConnection      = 5
        };

        ConnectionFailedEvent(QObject *sender, int serverHandle, ConnectionType connectionType,
                              const std::string& message, Reason reason) :
            Event(sender),
            message(message),
            reason(reason),
            serverHandle(serverHandle),
            connectionType(connectionType) {}

        std::string message;
        Reason reason;
        ConnectionType connectionType;
        int serverHandle;
    };

//    class ScriptExecute

    struct ConnectionEstablishedEvent : public Event
    {
        R_EVENT

        ConnectionEstablishedEvent(MongoServer *server, ConnectionType type, ConnectionInfo connInfo) :
            Event((QObject *)server),
            server(server),
            connectionType(type),
            connInfo(connInfo) { }       

        MongoServer *server;
        ConnectionType const connectionType;
        ConnectionInfo const connInfo;
    };


    class OperationFailedEvent : public Event
    {
        R_EVENT

    public:
        OperationFailedEvent(QObject *sender, const std::string &technicalErrorMessage, const std::string &userFriendlyErrorMessage) :
            technicalErrorMessage(technicalErrorMessage),
            userFriendlyErrorMessage(userFriendlyErrorMessage),
            Event(sender) { }

        std::string technicalErrorMessage;
        std::string userFriendlyErrorMessage;
    };

    /**
     * @brief Establish SSH Connection
     */

    class EstablishSshConnectionRequest : public Event
    {
    R_EVENT

        EstablishSshConnectionRequest(QObject *sender, int serverHandle, SshTunnelWorker* worker, ConnectionSettings* settings, ConnectionType connectionType) :
            Event(sender),
            worker(worker),
            settings(settings),
            serverHandle(serverHandle),
            connectionType(connectionType) {}

        ConnectionSettings* settings;
        ConnectionType connectionType;
        SshTunnelWorker* worker;
        int serverHandle;
    };

    class EstablishSshConnectionResponse : public Event
    {
    R_EVENT

        EstablishSshConnectionResponse(QObject *sender, int serverHandle, SshTunnelWorker* worker, ConnectionSettings* settings, ConnectionType connectionType, int localport) :
            Event(sender),
            worker(worker),
            settings(settings),
            connectionType(connectionType),
            serverHandle(serverHandle),
            localport(localport) {}

        EstablishSshConnectionResponse(QObject *sender, int serverHandle, const EventError &error, SshTunnelWorker* worker, ConnectionSettings* settings, ConnectionType connectionType) :
            Event(sender, error),
            worker(worker),
            settings(settings),
            serverHandle(serverHandle),
            connectionType(connectionType) {}

        ConnectionSettings* settings;
        ConnectionType connectionType;
        SshTunnelWorker* worker;
        int localport;
        int serverHandle;
    };

    /**
     * @brief Start listening on all opened sockets (SSH tunnel)
     */

    class ListenSshConnectionRequest : public Event
    {
    R_EVENT

        ListenSshConnectionRequest(QObject *sender, int serverHandle, ConnectionType connectionType) :
            Event(sender),
            serverHandle(serverHandle),
            connectionType(connectionType) {}

        ConnectionType connectionType;
        int serverHandle;
    };

    class ListenSshConnectionResponse : public Event
    {
    R_EVENT

        ListenSshConnectionResponse(QObject *sender, int serverHandle, ConnectionSettings* settings, ConnectionType connectionType) :
            Event(sender),
            settings(settings),
            serverHandle(serverHandle),
            connectionType(connectionType) {}

        ListenSshConnectionResponse(QObject *sender, const EventError &error, int serverHandle, ConnectionSettings* settings, ConnectionType connectionType) :
            Event(sender, error),
            settings(settings),
            serverHandle(serverHandle),
            connectionType(connectionType) {}

        ConnectionSettings* settings;
        ConnectionType connectionType;
        int serverHandle;
    };

    class LogEvent : public Event
    {
    R_EVENT

        enum LogLevel {
            RBM_ERROR  = 1,
            RBM_WARN   = 2,
            RBM_INFO   = 3,
            RBM_DEBUG  = 100 // log as much as possible
        };

        LogEvent(QObject *sender, const std::string& message, LogLevel level, 
                 bool const informUser = false) 
            : Event(sender), message(message), level(level), informUser(informUser)
        {}

        std::string severity() const {
            switch (level) {
                case RBM_ERROR : return "Error";
                case RBM_WARN  : return "Warning";
                case RBM_INFO  : return "Info";
                case RBM_DEBUG : return "Debug";
                default: return "Undefined";
            }
        }

        /// Severity mapped to a UI level: 0 = info, 1 = warning, 2 = critical.
        int uiLevel() const {
            switch (level) {
                case RBM_ERROR: return 2;
                case RBM_WARN:  return 1;
                default:        return 0;
            }
        }

        RagoShell::LogSeverity mongoLogSeverity() const {
            switch (level) {
                case RBM_ERROR: return RagoShell::LogSeverity::Error;
                case RBM_WARN: return RagoShell::LogSeverity::Warning;
                case RBM_INFO: return RagoShell::LogSeverity::Info;
                case RBM_DEBUG: return RagoShell::LogSeverity::Log;
                default: return RagoShell::LogSeverity::Info;
            }
        }

        std::string message;
        LogLevel level;
        bool const informUser = false;
    };

}
