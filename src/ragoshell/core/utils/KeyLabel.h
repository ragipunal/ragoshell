#pragma once

#include <QString>

namespace RagoShell
{
    /**
     * @brief Writes a key sequence the way a platform writes it (docs/DesignSystem.md).
     *
     * The sequence is portable: modifiers `Ctrl` (the primary command key), `Shift`, `Alt` and `Meta`, then
     * the key (`Ctrl+Shift+S`, `Ctrl+,`, `F5`, `Ctrl+Shift+Return`). Where the command key is the ⌘ key
     * (macOS, iPadOS) the result is `⇧⌘S`: symbols in Apple's order (⌃ ⌥ ⇧ ⌘) and the key as a symbol where
     * there is one (↩ ⇥ ⌫ ⌦ ⎋ arrows). Elsewhere it stays `Ctrl+Shift+S` with the key names spelt out.
     * Several alternatives separated by `/` (`F5 / Ctrl+R`) are written one by one.
     */
    QString keyLabel(const QString &sequence, bool commandKey);

    /**
     * @brief Decides whether keyboard shortcut hints should be displayed in the UI.
     *
     * Desktop platforms always display shortcuts. Mobile platforms (phones/tablets) only display
     * shortcuts while a real physical keyboard is attached — excluding Simulator / Emulator host
     * keyboard bridges unless explicitly enabled via `simKeyboardOverride`.
     */
    constexpr bool areShortcutsVisible(bool isMobile, bool hardwareKeyboard,
                                       bool simulatedDevice = false, bool simKeyboardOverride = false)
    {
        if (!isMobile)
            return true;
        if (simulatedDevice && !simKeyboardOverride)
            return false;
        return hardwareKeyboard;
    }

    /**
     * @brief Formats a translated label carrying a `%1` shortcut placeholder (e.g. `"Execute (%1)"`),
     *        or strips the placeholder and any surrounding brackets when shortcuts are hidden or
     *        `sequence` is empty.
     */
    QString formatWithShortcut(const QString &text, const QString &sequence,
                               bool shortcutsVisible, bool commandKey);
}
