#pragma once
#include <QString>
#include <QModelIndex>

QT_BEGIN_NAMESPACE
class QThread;
class QAbstractItemModel;
QT_END_NAMESPACE

#ifdef QT_NO_DEBUG
#define VERIFY(x) (x)
#else //QT_NO_DEBUG
#define VERIFY(x) Q_ASSERT(x)
#endif //QT_NO_DEBUG

namespace RagoShell
{
    namespace QtUtils
    {
        template<typename T>
        QString toQString(const T &value);

        std::string toStdString(const QString &value);

        std::string toStdStringSafe(const QString &value);

        void cleanUpThread(QThread *const thread);

        /// True if text contains typographic curly single/double quotes (U+2018..U+201F).
        bool containsSmartQuotes(const QString &value) noexcept;

        /// Replaces typographic curly quotes (‘ ’ ‚ ‛ -> ' and “ ” „ ‟ -> ") with ASCII quotes.
        QString normalizeSmartQuotes(const QString &value);

        template<typename Type>
        inline Type item(const QModelIndex &index)
        {
            return static_cast<Type>(index.internalPointer());
        }
    }
}
