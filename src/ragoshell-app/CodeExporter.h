#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace RagoShell
{
    /**
     * @brief QML "CodeExporter": shell find()/aggregate() → Python, Node.js,
     * Java or C# driver code (see QueryCodeExport).
     */
    class CodeExporter : public QObject
    {
        Q_OBJECT
        QML_ELEMENT

        Q_PROPERTY(QStringList languages READ languages CONSTANT FINAL)

    public:
        explicit CodeExporter(QObject *parent = nullptr);

        QStringList languages() const;

        /// { ok, error, code } for 'script' in the given language index. The
        /// URI comes from the connection 'serverUuid' with the password
        /// replaced by "<password>".
        Q_INVOKABLE QVariantMap exportScript(const QString &script, const QString &database,
                                             const QString &serverUuid, int language) const;

        /// Connection string for generated code; mongodb://localhost:27017 when
        /// the server is not connected.
        Q_INVOKABLE QString connectionUriFor(const QString &serverUuid) const;
    };
}
