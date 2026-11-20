#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "ragoshell/core/mcp/McpClientSetup.h"

using namespace RagoShell;
using Client = McpClientSetup::Client;

TEST(McpClientSetupTest, locates_the_binary_next_to_the_app)
{
    QTemporaryDir dir;
    const QString name = McpClientSetup::serverName() + "-mcp";

    McpLaunch missing = McpClientSetup::locate(dir.path(), {}, /*windows=*/false);
    EXPECT_FALSE(missing.found);
    EXPECT_TRUE(missing.command.endsWith("/" + name));
    EXPECT_TRUE(missing.args.isEmpty());

    QFile bin(dir.filePath(name));
    ASSERT_TRUE(bin.open(QIODevice::WriteOnly));
    bin.close();
    EXPECT_TRUE(McpClientSetup::locate(dir.path(), {}, false).found);

    const McpLaunch win = McpClientSetup::locate("C:/Program Files/RagoShell", {}, /*windows=*/true);
    EXPECT_EQ(win.command, R"(C:\Program Files\RagoShell\)" + name + ".exe");
}

TEST(McpClientSetupTest, an_appimage_is_started_with_mcp)
{
    QTemporaryDir dir;
    const QString appImage = dir.filePath("RagoShell.AppImage");
    QFile file(appImage);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    const McpLaunch launch = McpClientSetup::locate("/tmp/.mount_x/usr/bin", appImage, /*windows=*/false);
    EXPECT_TRUE(launch.found);
    EXPECT_EQ(launch.command, appImage);
    EXPECT_EQ(launch.args, QStringList({"--mcp"}));
    EXPECT_EQ(McpClientSetup::snippet(Client::ClaudeCode, launch, false),
              QString("claude mcp add --scope user %1 -- '%2' '--mcp'").arg(McpClientSetup::serverName(), appImage));
}

TEST(McpClientSetupTest, claude_code_command_quotes_the_path)
{
    const McpLaunch win{R"(C:\Program Files\RagoShell\ragoshell-mcp.exe)", {}, true};
    EXPECT_EQ(McpClientSetup::snippet(Client::ClaudeCode, win, /*windows=*/true),
              QString(R"(claude mcp add --scope user %1 -- "C:\Program Files\RagoShell\ragoshell-mcp.exe")")
                  .arg(McpClientSetup::serverName()));

    // POSIX: single quotes keep spaces and $ literal; a quote in the path is escaped.
    const McpLaunch odd{"/home/it's me/$HOME/ragoshell-mcp", {}, true};
    EXPECT_EQ(McpClientSetup::snippet(Client::ClaudeCode, odd, /*windows=*/false),
              QString(R"(claude mcp add --scope user %1 -- '/home/it'\''s me/$HOME/ragoshell-mcp')")
                  .arg(McpClientSetup::serverName()));
}

TEST(McpClientSetupTest, no_command_line_for_a_path_a_windows_shell_would_expand)
{
    EXPECT_TRUE(McpClientSetup::commandLineSafe({R"(C:\Program Files\RagoShell\ragoshell-mcp.exe)", {}, true}, true));
    for (const char *path : {R"(C:\Tools\$(calc)\ragoshell-mcp.exe)", R"(C:\Tools\%TEMP%\ragoshell-mcp.exe)",
                             R"(C:\Tools\a`b\ragoshell-mcp.exe)"})
        EXPECT_FALSE(McpClientSetup::commandLineSafe({path, {}, true}, true)) << path;
    // POSIX single quotes keep $ literal.
    EXPECT_TRUE(McpClientSetup::commandLineSafe({"/opt/$(x)/ragoshell-mcp", {}, true}, false));
}

TEST(McpClientSetupTest, json_blocks_per_client)
{
    const McpLaunch win{R"(C:\Program Files\RagoShell\ragoshell-mcp.exe)", {}, true};
    const QString name = McpClientSetup::serverName();

    for (Client client : {Client::ClaudeDesktop, Client::Cursor, Client::Antigravity}) {
        const QString text = McpClientSetup::snippet(client, win, true);
        // Backslashes are escaped, so the block is valid JSON with the exact path.
        const QJsonObject server = QJsonDocument::fromJson(text.toUtf8()).object()
                                       .value("mcpServers").toObject().value(name).toObject();
        EXPECT_EQ(server.value("command").toString(), win.command) << text.toStdString();
        EXPECT_EQ(server.value("args").toArray().size(), 0);
        EXPECT_FALSE(server.contains("type"));
    }

    const QString vscode = McpClientSetup::snippet(Client::VsCode, win, true);
    const QJsonObject server = QJsonDocument::fromJson(vscode.toUtf8()).object()
                                   .value("servers").toObject().value(name).toObject();
    EXPECT_EQ(server.value("type").toString(), QStringLiteral("stdio"));
    EXPECT_EQ(server.value("command").toString(), win.command);
}
