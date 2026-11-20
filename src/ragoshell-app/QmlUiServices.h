#pragma once

#include <QObject>
#include <QEventLoop>
#include <QString>

#include "ragoshell/core/services/UiServices.h"

namespace RagoShell
{
    /**
     * @brief IUiServices implementation for the QML frontend.
     *
     * Dialogs are driven by QML: each call emits a request signal and blocks
     * on a nested QEventLoop until the QML side calls the matching
     * resolve/cancel method. Works because the calls originate on the GUI
     * thread and the previous dialog semantics were modal anyway.
     */
    class QmlUiServices : public QObject, public IUiServices
    {
        Q_OBJECT

    public:
        explicit QmlUiServices(QObject *parent = nullptr);

        // IUiServices
        QString getOpenFileName(const QString &caption, const QString &dir,
                                const QString &filter) override;
        QString getSaveFileName(const QString &caption, const QString &dir,
                                const QString &filter) override;
        void criticalError(const QString &title, const QString &message) override;
        void showMessage(const QString &title, const QString &message,
                         int level) override;
        QString getText(const QString &title, const QString &label,
                        bool passwordMode, bool *ok) override;
        void beginWaitCursor() override;
        void endWaitCursor() override;

        // Called from QML to resolve pending requests.
        Q_INVOKABLE void resolveFile(const QString &path);
        Q_INVOKABLE void resolveText(const QString &text);
        Q_INVOKABLE void cancelPending();
        Q_INVOKABLE void dismissMessage();

    Q_SIGNALS:
        void openFileRequested(const QString &caption, const QString &dir,
                               const QString &filter);
        void saveFileRequested(const QString &caption, const QString &dir,
                               const QString &filter);
        void messageRequested(const QString &title, const QString &message,
                              int level);
        void textRequested(const QString &title, const QString &label,
                           bool passwordMode);

    private:
        QEventLoop *_loop = nullptr;
        QString _result;
        bool _accepted = false;
        int _waitDepth = 0;
    };
}
