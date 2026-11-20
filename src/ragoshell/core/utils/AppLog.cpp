#include "ragoshell/core/utils/AppLog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
    namespace
    {
        QMutex g_mutex;
        QFile g_file;
        QString g_path;
        qint64 g_size = 0;
        QtMessageHandler g_previousHandler = nullptr;
        bool g_handlerInstalled = false;

        const char *levelName(LogSeverity severity)
        {
            switch (severity) {
            case LogSeverity::Log:     return "Log";
            case LogSeverity::Info:    return "Info";
            case LogSeverity::Warning: return "Warning";
            case LogSeverity::Error:   return "Error";
            }
            return "Log";
        }

        // Unbuffered: a line is on disk when write() returns, so a crash or a
        // forced exit (_exit, a killed process) loses nothing.
        bool openFile()
        {
            g_file.setFileName(g_path);
            if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered))
                return false;
            g_size = g_file.size();
            return true;
        }

        // One rotated file: ragoshell.log -> ragoshell.log.1 (replaced).
        void rotateLocked()
        {
            g_file.close();
            const QString rotated = g_path + QStringLiteral(".1");
            QFile::remove(rotated);
            QFile::rename(g_path, rotated);
            openFile();
        }

        void writeLocked(LogSeverity severity, const QString &message)
        {
            if (!g_file.isOpen())
                return;
            if (g_size >= AppLog::kMaxBytes)
                rotateLocked();
            if (!g_file.isOpen())
                return;

            QByteArray line = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8();
            line += " [";
            line += levelName(severity);
            line += "] ";
            line += message.toUtf8();
            line += '\n';
            g_size += g_file.write(line);
        }

        void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
        {
            LogSeverity severity = LogSeverity::Log;
            switch (type) {
            case QtDebugMsg:    severity = LogSeverity::Log; break;
            case QtInfoMsg:     severity = LogSeverity::Info; break;
            case QtWarningMsg:  severity = LogSeverity::Warning; break;
            case QtCriticalMsg:
            case QtFatalMsg:    severity = LogSeverity::Error; break;
            }
            // A message raised while writing the log (from this thread) is not logged again.
            thread_local bool inHandler = false;
            QtMessageHandler previous = nullptr;
            if (!inHandler) {
                inHandler = true;
                {
                    QMutexLocker lock(&g_mutex);
                    previous = g_previousHandler;
                    // Qt categories (qt.qml.binding.removal, ...) say where a message came from.
                    if (context.category && qstrcmp(context.category, "default") != 0)
                        writeLocked(severity, QString::fromLatin1(context.category) + QStringLiteral(": ") + message);
                    else
                        writeLocked(severity, message);
                }
                inHandler = false;
            } else {
                // Raised from inside writeLocked(): this thread already holds g_mutex.
                previous = g_previousHandler;
            }
            if (previous)
                previous(type, context, message);
        }
    }

    namespace AppLog
    {
        bool install(const QString &logDir)
        {
            QMutexLocker lock(&g_mutex);
            if (g_file.isOpen())
                return true;

            if (!QDir().mkpath(logDir))
                return false;
            g_path = QDir::cleanPath(logDir) + QLatin1Char('/') + appFileName("log");
            if (QFileInfo(g_path).size() >= kMaxBytes) {
                const QString rotated = g_path + QStringLiteral(".1");
                QFile::remove(rotated);
                QFile::rename(g_path, rotated);
            }
            if (!openFile())
                return false;

            if (!g_handlerInstalled) {
                g_previousHandler = qInstallMessageHandler(messageHandler);
                g_handlerInstalled = true;
            }
            return true;
        }

        void uninstall()
        {
            QMutexLocker lock(&g_mutex);
            if (g_handlerInstalled) {
                qInstallMessageHandler(g_previousHandler);
                g_previousHandler = nullptr;
                g_handlerInstalled = false;
            }
            g_file.close();
            g_path.clear();
            g_size = 0;
        }

        bool isInstalled()
        {
            QMutexLocker lock(&g_mutex);
            return g_file.isOpen();
        }

        QString filePath()
        {
            QMutexLocker lock(&g_mutex);
            return g_file.isOpen() ? g_path : QString();
        }

        void write(LogSeverity severity, const QString &message)
        {
            QMutexLocker lock(&g_mutex);
            writeLocked(severity, message);
        }
    }
}
