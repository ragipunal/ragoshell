#pragma once

#include <QObject>
#include <QString>

namespace RagoShell
{
    /**
     * @brief Lightweight signal hub decoupling the GUI-independent core from
     *        concrete UI implementations.
     *
     * Core components emit these signals; the QML app connects them to its
     * own dialogs. This keeps MainWindow/QMessageBox usage out of core/.
     */
    class UiSignals : public QObject
    {
        Q_OBJECT

    public:
        static UiSignals& instance()
        {
            static UiSignals inst;
            return inst;
        }

    Q_SIGNALS:
        /// Request to hide the main-window query progress bar.
        void hideProgressBarRequested();

        /// Request to show a warning dialog (title, message).
        void warningRequested(const QString &title, const QString &message);
    };
}
