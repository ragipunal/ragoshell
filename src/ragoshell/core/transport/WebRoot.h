#pragma once

#include <QFileInfo>
#include <QString>

namespace RagoShell
{
    /// Static-file containment for ragoshell-web-gateway (docs/WebGateway.md
    /// §3, T7). Header-only so the gateway binary and the unit tests share
    /// the exact check.
    namespace WebRoot
    {
        /// Canonical path of `path` when it exists and lies strictly inside
        /// `canonicalRoot` (itself already canonical); empty otherwise.
        /// Canonicalizing first resolves symlinks, so a link inside the root
        /// that points outside is refused. Every file the gateway opens —
        /// the requested one and its `.br` sibling (2026-11-04 review R9) —
        /// must go through this.
        inline QString containedPath(const QString &canonicalRoot, const QString &path)
        {
            if (canonicalRoot.isEmpty())
                return {};
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty()
                || !canonical.startsWith(canonicalRoot + QLatin1Char('/')))
                return {};
            return canonical;
        }
    }
}
