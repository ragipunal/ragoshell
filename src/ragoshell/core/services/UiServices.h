#pragma once

#include <QString>

namespace RagoShell
{
    /**
     * @brief Frontend-provided UI services consumed by the GUI-independent
     *        core library (rago-core).
     *
     * The QML app registers its QML-dialog-based implementation
     * (QmlUiServices) at startup.
     *
     * Core code must tolerate a null instance (unit tests register none).
     */
    class IUiServices
    {
    public:
        virtual ~IUiServices() = default;

        virtual QString getOpenFileName(const QString &caption,
                                        const QString &dir,
                                        const QString &filter) = 0;
        virtual QString getSaveFileName(const QString &caption,
                                        const QString &dir,
                                        const QString &filter) = 0;
        virtual void criticalError(const QString &title,
                                   const QString &message) = 0;
        /// Severity level: 0 = information, 1 = warning, 2 = critical.
        virtual void showMessage(const QString &title,
                                 const QString &message,
                                 int level) = 0;
        virtual QString getText(const QString &title, const QString &label,
                                bool passwordMode, bool *ok) = 0;
        virtual void beginWaitCursor() = 0;
        virtual void endWaitCursor() = 0;
    };

    namespace UiServices
    {
        IUiServices *instance();
        void setInstance(IUiServices *services);
    }
}
