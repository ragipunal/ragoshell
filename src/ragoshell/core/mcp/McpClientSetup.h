#pragma once

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /// How an AI client starts the MCP server: the program and its arguments.
    struct McpLaunch
    {
        QString command;
        QStringList args;
        bool found = false;   ///< the program exists on disk
    };

    /**
     * @brief Ready-to-paste MCP client configuration for ragoshell-mcp
     * (Preferences › AI tools). The text names the server and its path only;
     * it never contains connection details or secrets.
     */
    namespace McpClientSetup
    {
        enum class Client { ClaudeCode, ClaudeDesktop, Cursor, Antigravity, VsCode };

        /// Server name used in every client configuration (the product id).
        QString serverName();

        /// The server binary next to the app (appDir), or, inside a Linux
        /// AppImage (appImage = $APPIMAGE), the AppImage with --mcp, because the
        /// files inside an AppImage are not reachable from outside.
        McpLaunch locate(const QString &appDir, const QString &appImage, bool windows);

        /// Whether the command line is safe to paste into a shell. Inside double
        /// quotes PowerShell still expands $ and backticks and cmd expands %,
        /// all legal in Windows folder names; POSIX single quotes keep everything.
        bool commandLineSafe(const McpLaunch &launch, bool windows);

        /// Configuration text for one client: the `claude mcp add` command line
        /// for Claude Code, a JSON block for the others. windows picks the
        /// path style and the shell quoting.
        QString snippet(Client client, const McpLaunch &launch, bool windows);
    }
}
