#include "ragoshell-app/QmlUiServices.h"

#include <QGuiApplication>
#include <QCursor>

namespace RagoShell
{
    QmlUiServices::QmlUiServices(QObject *parent) : QObject(parent) {}

    QString QmlUiServices::getOpenFileName(const QString &caption,
                                         const QString &dir,
                                         const QString &filter)
    {
        QEventLoop loop;
        _loop = &loop;
        _accepted = false;
        emit openFileRequested(caption, dir, filter);
        loop.exec();
        _loop = nullptr;
        return _accepted ? _result : QString();
    }

    QString QmlUiServices::getSaveFileName(const QString &caption,
                                         const QString &dir,
                                         const QString &filter)
    {
        QEventLoop loop;
        _loop = &loop;
        _accepted = false;
        emit saveFileRequested(caption, dir, filter);
        loop.exec();
        _loop = nullptr;
        return _accepted ? _result : QString();
    }

    void QmlUiServices::criticalError(const QString &title, const QString &message)
    {
        showMessage(title, message, 2);
    }

    void QmlUiServices::showMessage(const QString &title, const QString &message,
                                  int level)
    {
        QEventLoop loop;
        _loop = &loop;
        emit messageRequested(title, message, level);
        loop.exec();
        _loop = nullptr;
    }

    QString QmlUiServices::getText(const QString &title, const QString &label,
                                   bool passwordMode, bool *ok)
    {
        QEventLoop loop;
        _loop = &loop;
        _accepted = false;
        emit textRequested(title, label, passwordMode);
        loop.exec();
        _loop = nullptr;
        if (ok)
            *ok = _accepted;
        return _accepted ? _result : QString();
    }

    void QmlUiServices::beginWaitCursor()
    {
        ++_waitDepth;
        QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    }

    void QmlUiServices::endWaitCursor()
    {
        if (--_waitDepth <= 0) {
            _waitDepth = 0;
            QGuiApplication::restoreOverrideCursor();
        }
    }

    void QmlUiServices::resolveFile(const QString &path)
    {
        _result = path;
        _accepted = !path.isEmpty();
        if (_loop)
            _loop->quit();
    }

    void QmlUiServices::resolveText(const QString &text)
    {
        _result = text;
        _accepted = true;
        if (_loop)
            _loop->quit();
    }

    void QmlUiServices::cancelPending()
    {
        _accepted = false;
        if (_loop)
            _loop->quit();
    }

    void QmlUiServices::dismissMessage()
    {
        if (_loop)
            _loop->quit();
    }
}
