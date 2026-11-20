#pragma once

#include <QString>

#include "ragoshell/core/Enums.h"

namespace RagoShell
{
    /**
     * Log file of the application: <appLogDir>/ragoshell.log plus one rotated
     * file, ragoshell.log.1. Everything goes there: LOG_MSG lines, worker
     * thread log events and every Qt message (qDebug, qWarning, QML errors),
     * which is then still passed on to Qt's own handler, so stderr, logcat
     * and the unified log keep working.
     *
     * Not installed until install() is called, so tools that link rago-core
     * (the MCP server, unit tests) never create a log file. write() is
     * thread safe.
     */
    namespace AppLog
    {
        /// Size at which the log is rotated.
        constexpr qint64 kMaxBytes = 5 * 1024 * 1024;

        /// Opens <logDir>/ragoshell.log (rotating an oversized one) and
        /// installs the Qt message handler. Returns false when the file
        /// cannot be opened; the Qt handler is then not installed.
        bool install(const QString &logDir);

        /// Removes the Qt message handler and closes the file.
        void uninstall();

        bool isInstalled();

        /// Current log file, empty while not installed.
        QString filePath();

        /// Appends "<time> [<level>] <message>" to the log; no-op while not installed.
        void write(LogSeverity severity, const QString &message);
    }
}
