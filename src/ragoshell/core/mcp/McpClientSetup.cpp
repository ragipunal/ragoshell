#include "ragoshell/core/mcp/McpClientSetup.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ragoshell/core/BrandInfo.h"

namespace RagoShell
{
    namespace
    {
        // cmd.exe and PowerShell both take a double-quoted path; a Windows path
        // cannot contain a double quote. POSIX shells get single quotes, which
        // keep $, backslashes and spaces literal.
        QString shellQuote(const QString &arg, bool windows)
        {
            if (windows)
                return QLatin1Char('"') + arg + QLatin1Char('"');
            QString quoted = arg;
            quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            return QLatin1Char('\'') + quoted + QLatin1Char('\'');
        }

        QString nativePath(const QString &path, bool windows)
        {
            if (!windows)
                return QDir::fromNativeSeparators(path);
            QString p = path;
            p.replace(QLatin1Char('/'), QLatin1Char('\\'));
            return p;
        }
    }

    QString McpClientSetup::serverName()
    {
        return QString::fromLatin1(BrandInfo::productId);
    }

    McpLaunch McpClientSetup::locate(const QString &appDir, const QString &appImage, bool windows)
    {
        McpLaunch launch;
        QString onDisk;
        if (!windows && !appImage.isEmpty()) {
            onDisk = appImage;
            launch.args = {QStringLiteral("--mcp")};
        } else {
            const QString binary = serverName() + QStringLiteral("-mcp")
                                   + (windows ? QStringLiteral(".exe") : QString());
            onDisk = QDir(appDir).filePath(binary);
        }
        launch.found = QFileInfo(onDisk).isFile();
        launch.command = nativePath(onDisk, windows);
        return launch;
    }

    bool McpClientSetup::commandLineSafe(const McpLaunch &launch, bool windows)
    {
        if (!windows)
            return true;
        static const QString expanded = QStringLiteral("$`%\"");
        return std::none_of(launch.command.begin(), launch.command.end(),
                            [](QChar ch) { return expanded.contains(ch); });
    }

    QString McpClientSetup::snippet(Client client, const McpLaunch &launch, bool windows)
    {
        const QString command = nativePath(launch.command, windows);
        if (client == Client::ClaudeCode) {
            QString line = QStringLiteral("claude mcp add --scope user %1 -- %2")
                               .arg(serverName(), shellQuote(command, windows));
            for (const QString &arg : launch.args)
                line += QLatin1Char(' ') + shellQuote(arg, windows);
            return line;
        }

        QJsonObject server;
        if (client == Client::VsCode)
            server["type"] = QStringLiteral("stdio");
        server["command"] = command;
        server["args"] = QJsonArray::fromStringList(launch.args);
        // VS Code names the top-level key "servers"; the others "mcpServers".
        const QString rootKey = client == Client::VsCode ? QStringLiteral("servers")
                                                         : QStringLiteral("mcpServers");
        const QJsonObject root{{rootKey, QJsonObject{{serverName(), server}}}};
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)).trimmed();
    }
}
