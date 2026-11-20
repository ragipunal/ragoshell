#pragma once

#include <QObject>
#include <QString>
#include <string>

#include "ragoshell/core/Enums.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/utils/SingletonPattern.hpp"

namespace RagoShell
{  
    class Logger : public QObject, public Patterns::LazySingleton<Logger>
    {
        Q_OBJECT
        friend class Patterns::LazySingleton<Logger>;

    public:
        void print(const char *msg, LogSeverity level, bool notify);
        void print(const std::string &msg, LogSeverity level, bool notify);
        void print(const QString &msg, LogSeverity level, bool notify);

    Q_SIGNALS:
        void printed(const QString &msg, RagoShell::LogSeverity level);

    private:
        Logger();
        ~Logger();
    };

    // Use in main thread
    template<typename T>
    inline void LOG_MSG(const T &msg, LogSeverity level, bool notify = true)
    {
        return Logger::instance().print(msg, level, notify);
    }
    
    // Use in worker threads (e.g. MongoWorker) to log anything 
    // Sends LogEvent to main thread (App class) 
    void sendLog(
        QObject *sender, LogEvent::LogLevel const& severity,
        std::string const& msg, bool const informUser = false
    );

    void debugLog(std::string_view msg);
}

Q_DECLARE_METATYPE(RagoShell::LogSeverity)
