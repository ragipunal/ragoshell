#include "ragoshell/core/utils/Logger.h"

#include <QDebug>
#include <QMetaType>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppLog.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    Logger::Logger()
    {
        qRegisterMetaType<LogSeverity>();
    }

    Logger::~Logger()
    {
    }

    void Logger::print(const char *mess, LogSeverity level, bool notify)
    {
        print(std::string(mess), level, notify);
    }

    void Logger::print(const std::string &mess, LogSeverity level, bool notify)
    {
        print(QtUtils::toQString(mess), level, notify);
    }

    void Logger::print(const QString &msg, LogSeverity level, bool notify)
    {
        // The log file gets every line, also the ones that are not for the user.
        AppLog::write(level, msg.simplified());

        if (!notify)
            return;

        // Make uniform log level strings e.g "Error: ", "Info: " etc...
        QString logLevelStr;
        switch (level) {
        case LogSeverity::Log:     logLevelStr = "Log"; break;
        case LogSeverity::Info:    logLevelStr = "Info"; break;
        case LogSeverity::Warning: logLevelStr = "Warning"; break;
        case LogSeverity::Error:   logLevelStr = "Error"; break;
        }
        if (!logLevelStr.isEmpty()) {
            logLevelStr = logLevelStr.toLower();
            logLevelStr[0] = logLevelStr[0].toUpper();
            logLevelStr += ": ";
        }
        emit printed(logLevelStr + msg.simplified(), level);
    }

    void sendLog(
        QObject *sender, LogEvent::LogLevel const& severity,
        std::string const& msg, bool const informUser /*= false*/)
    {
        AppRegistry::instance().bus()->send(
            AppRegistry::instance().app(),
            new LogEvent(sender, msg, severity, informUser)
        );
    }

    void debugLog(std::string_view msg) {
        if (!AppRegistry::instance().settingsManager()->debugMode())
            return;

        // Qt logging: stderr on desktop, logcat / os_log on mobile, and the log file.
        qDebug().noquote() << QString::fromUtf8(msg.data(), static_cast<qsizetype>(msg.size()));
    }
}
