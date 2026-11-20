#include "ragoshell/core/utils/KeyLabel.h"

#include <QRegularExpression>
#include <QStringList>

namespace RagoShell
{
    namespace
    {
        bool isModifier(const QString &part)
        {
            return part == QLatin1String("Ctrl") || part == QLatin1String("Shift")
                || part == QLatin1String("Alt") || part == QLatin1String("Meta");
        }

        // The key as a symbol where Apple's keyboards print one.
        QString macKey(const QString &key)
        {
            struct Entry { const char *name; const char *symbol; };
            static const Entry table[] = {
                { "Return", "↩" }, { "Enter", "↩" }, { "Tab", "⇥" }, { "Backspace", "⌫" }, { "Delete", "⌦" },
                { "Del", "⌦" }, { "Esc", "⎋" }, { "Escape", "⎋" }, { "PgUp", "⇞" }, { "PgDown", "⇟" },
                { "Home", "↖" }, { "End", "↘" }, { "Up", "↑" }, { "Down", "↓" }, { "Left", "←" }, { "Right", "→" },
            };
            for (const Entry &e : table) {
                if (key == QLatin1String(e.name))
                    return QString::fromUtf8(e.symbol);
            }
            return key.size() == 1 ? key.toUpper() : key;
        }

        QString oneSequence(const QString &sequence, bool commandKey)
        {
            QStringList parts = sequence.trimmed().split(QLatin1Char('+'));
            // "Ctrl++" and "Ctrl+," style: an empty last part means the key is the plus sign itself.
            if (parts.size() >= 2 && parts.last().isEmpty()) {
                parts.removeLast();
                parts.last() = QStringLiteral("+");
            }

            bool ctrl = false, shift = false, alt = false, meta = false;
            QString key;
            for (const QString &p : parts) {
                if (p == QLatin1String("Ctrl")) ctrl = true;
                else if (p == QLatin1String("Shift")) shift = true;
                else if (p == QLatin1String("Alt")) alt = true;
                else if (p == QLatin1String("Meta")) meta = true;
                else if (!p.isEmpty()) key = p;
            }

            if (!commandKey) {
                // Windows, Linux, Android: keep the written order of the modifiers.
                QStringList out;
                for (const QString &p : parts) {
                    if (isModifier(p) || p == key)
                        out << p;
                }
                return out.join(QLatin1Char('+'));
            }

            QString out;
            if (meta) out += QStringLiteral("⌃");
            if (alt) out += QStringLiteral("⌥");
            if (shift) out += QStringLiteral("⇧");
            if (ctrl) out += QStringLiteral("⌘");
            return out + macKey(key);
        }
    }

    QString keyLabel(const QString &sequence, bool commandKey)
    {
        // "F5 / Ctrl+R": alternatives; a "/" that is the key itself ("Ctrl+/") has no spaces around it.
        const QStringList alternatives = sequence.split(QStringLiteral(" / "));
        QStringList out;
        for (const QString &one : alternatives)
            out << oneSequence(one, commandKey);
        return out.join(QStringLiteral(" / "));
    }

    QString formatWithShortcut(const QString &text, const QString &sequence,
                               bool shortcutsVisible, bool commandKey)
    {
        if (shortcutsVisible && !sequence.isEmpty())
            return text.arg(keyLabel(sequence, commandKey));

        static const QRegularExpression bracketed(QStringLiteral("\\s*[\\(（\\[]\\s*%1\\s*[\\)）\\]]"));
        QString out = text;
        out.remove(bracketed);
        out.remove(QStringLiteral("%1"));
        return out.trimmed();
    }
}
