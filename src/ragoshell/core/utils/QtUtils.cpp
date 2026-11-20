#include "ragoshell/core/utils/QtUtils.h"

#include <QThread>

namespace RagoShell
{
    namespace QtUtils
    {
        template<>
        QString toQString<std::string>(const std::string &value)
        {
            //static QTextCodec *LOCALECODEC = QTextCodec::codecForLocale();
            return QString::fromUtf8(value.c_str(), value.size());
        }

        template<>
        QString toQString<std::wstring>(const std::wstring &value)
        {
            return  QString((const QChar*)value.c_str(), value.length());
        }

        std::string toStdString(const QString &value)
        {
            QByteArray sUtf8 = value.toUtf8();
            return std::string(sUtf8.constData(), sUtf8.length());
        }

        std::string toStdStringSafe(const QString &value)
        {
#ifdef Q_OS_WIN
            QByteArray sUtf8 = value.toLocal8Bit();            
#else
            QByteArray sUtf8 = value.toUtf8();
#endif    
            return std::string(sUtf8.constData(), sUtf8.length());
        }

        void cleanUpThread(QThread *const thread)
        {
            if (thread && thread->isRunning()) {
                //thread->stop();
                thread->wait();
            }
        }

        bool containsSmartQuotes(const QString &value) noexcept
        {
            for (QChar ch : value) {
                const ushort u = ch.unicode();
                if (u >= 0x2018 && u <= 0x201F)
                    return true;
            }
            return false;
        }

        QString normalizeSmartQuotes(const QString &value)
        {
            if (!containsSmartQuotes(value))
                return value;

            QString out = value;
            for (qsizetype i = 0; i < out.size(); ++i) {
                const ushort u = out.at(i).unicode();
                if (u >= 0x2018 && u <= 0x201B)
                    out[i] = QLatin1Char('\'');
                else if (u >= 0x201C && u <= 0x201F)
                    out[i] = QLatin1Char('"');
            }
            return out;
        }

    }
}
