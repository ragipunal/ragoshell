#include "ragoshell/core/domain/ScriptInfo.h"

#include <QTextStream>
#include <QFile>
#include <QFileInfo>

#include "ragoshell/core/services/UiServices.h"

namespace
{
    const QString filterForScripts = QObject::tr("JavaScript (*.js);; All Files (*.*)");

    bool loadFromFileText(const QString &filePath, QString &text)
    {
        bool result = false;
        QFile file(filePath);
        if (file.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream in(&file);
            if (auto *ui = RagoShell::UiServices::instance())
                ui->beginWaitCursor();
            text = in.readAll();
            if (auto *ui = RagoShell::UiServices::instance())
                ui->endWaitCursor();
            result = true;
        }
        else if (auto *ui = RagoShell::UiServices::instance()) {
            ui->criticalError(QString("Error"),
                QObject::tr(PROJECT_NAME" can't read from %1:\n%2.")
                    .arg(filePath)
                    .arg(file.errorString()));
        }

        return result;
    }

    bool saveToFileText(QString filePath, const QString &text)
    {
        if (filePath.isEmpty())
            return false;

#ifdef Q_OS_LINUX
        if (QFileInfo(filePath).suffix().isEmpty()) {
            filePath += ".js";
        }
#endif
        bool result = false;
        QFile file(filePath);
        if (file.open(QFile::WriteOnly | QFile::Text)) {
            QTextStream out(&file);
            if (auto *ui = RagoShell::UiServices::instance())
                ui->beginWaitCursor();
            out << text;
            if (auto *ui = RagoShell::UiServices::instance())
                ui->endWaitCursor();
            result = true;
        }
        else if (auto *ui = RagoShell::UiServices::instance()) {
            ui->criticalError(QString("Error"),
                QObject::tr(PROJECT_NAME" can't save to %1:\n%2.")
                    .arg(filePath)
                    .arg(file.errorString()));
        }

        return result;
    }
}

namespace RagoShell
{
    ScriptInfo::ScriptInfo(const QString &script, bool execute, const std::string &dbname, 
                           const CursorPosition &position, const QString &title, const QString &filePath,
                           const QString &draftId) :
        _script(script),
        _execute(execute),
        _dbname(dbname),
        _title(title),
        _cursor(position),
        _filePath(filePath),
        _draftId(draftId) {}

    bool ScriptInfo::loadFromFile(const QString &filePath)
    {
        bool result = false;
        auto *ui = UiServices::instance();
        QString filepath = ui ? ui->getOpenFileName(QString(), filePath, filterForScripts)
                              : QString();
        if (!filepath.isEmpty()) {
            QString out;
            if (loadFromFileText(filepath, out)) {
                _script = out;
                _filePath = filepath;
                result = true;
            }
        }
        return result;
    }

    bool ScriptInfo::loadFromFile()
    {
        return loadFromFile(_filePath);
    }

    bool ScriptInfo::saveToFileAs()
    {
        auto *ui = UiServices::instance();
        QString filepath = ui ? ui->getSaveFileName(QObject::tr("Save As"), _filePath,
                                                  filterForScripts)
                              : QString();

        if (saveToFileText(filepath, _script)) {
            _filePath = filepath;
            return true;
        }
        return false;
    }

    bool ScriptInfo::saveToFile()
    {
        return _filePath.isEmpty() ? saveToFileAs() : saveToFileText(_filePath, _script);
    }
}
