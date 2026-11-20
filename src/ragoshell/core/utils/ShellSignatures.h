#pragma once

#include <QString>
#include <QStringList>

namespace RagoShell
{
    /**
     * @brief Call tips for the mongo shell editor: which shell method call
     * the caret is in, its parameters and the parameter being typed.
     */
    namespace ShellSignatures
    {
        struct CallTip
        {
            bool found = false;
            QString name;          ///< method / function name, e.g. "find"
            QStringList params;    ///< e.g. {"filter", "projection", "options"}
            int activeParam = 0;   ///< index into params (may exceed for varargs)
            int parenOffset = 0;   ///< characters from the '(' to the end of the text
        };

        /**
         * @param textBeforeCaret editor text up to the caret (the last few
         *        thousand characters are enough)
         * Strings, template literals and // or block comments are skipped;
         * the innermost open call with a known signature wins.
         */
        CallTip lookup(const QString &textBeforeCaret);

        /// "find(filter, projection, options)"
        QString label(const CallTip &tip);
    }
}
