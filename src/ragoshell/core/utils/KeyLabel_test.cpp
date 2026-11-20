#include <gtest/gtest.h>

#include "ragoshell/core/utils/KeyLabel.h"

namespace RagoShell
{
    TEST(KeyLabelTest, windows_linux_and_android_spell_the_keys_out)
    {
        EXPECT_EQ(keyLabel("Ctrl+N", false), "Ctrl+N");
        EXPECT_EQ(keyLabel("Ctrl+Shift+S", false), "Ctrl+Shift+S");
        EXPECT_EQ(keyLabel("F5", false), "F5");
        EXPECT_EQ(keyLabel("Ctrl+,", false), "Ctrl+,");
        EXPECT_EQ(keyLabel("Ctrl+Shift+Return", false), "Ctrl+Shift+Return");
        EXPECT_EQ(keyLabel("Alt+Shift+F", false), "Alt+Shift+F");
        EXPECT_EQ(keyLabel("F5 / Ctrl+R", false), "F5 / Ctrl+R");
        EXPECT_EQ(keyLabel("Ctrl+/", false), "Ctrl+/");
    }

    TEST(KeyLabelTest, mac_and_ipad_use_apple_symbols_in_apples_order)
    {
        EXPECT_EQ(keyLabel("Ctrl+N", true), "⌘N");
        EXPECT_EQ(keyLabel("Ctrl+Shift+S", true), "⇧⌘S");
        EXPECT_EQ(keyLabel("Ctrl+Alt+Shift+K", true), "⌥⇧⌘K");
        EXPECT_EQ(keyLabel("Meta+Ctrl+F", true), "⌃⌘F");
        EXPECT_EQ(keyLabel("Ctrl+,", true), "⌘,");
        EXPECT_EQ(keyLabel("Ctrl+Shift+Return", true), "⇧⌘↩");
        EXPECT_EQ(keyLabel("Ctrl+Backspace", true), "⌘⌫");
        EXPECT_EQ(keyLabel("Ctrl+Tab", true), "⌘⇥");
        EXPECT_EQ(keyLabel("F5", true), "F5");
        EXPECT_EQ(keyLabel("Alt+Right", true), "⌥→");
        EXPECT_EQ(keyLabel("F5 / Ctrl+R", true), "F5 / ⌘R");
        EXPECT_EQ(keyLabel("Ctrl+/", true), "⌘/");
    }

    TEST(KeyLabelTest, a_plus_sign_as_the_key_and_empty_input_do_not_break)
    {
        EXPECT_EQ(keyLabel("Ctrl++", false), "Ctrl++");
        EXPECT_EQ(keyLabel("Ctrl++", true), "⌘+");
        EXPECT_EQ(keyLabel("", false), "");
        EXPECT_EQ(keyLabel("", true), "");
    }

    TEST(KeyLabelTest, mobile_hides_shortcuts_without_physical_keyboard_or_on_simulator)
    {
        // Desktop always displays shortcuts
        EXPECT_TRUE(areShortcutsVisible(false, false, false, false));

        // Mobile hides shortcuts when no hardware keyboard is attached
        EXPECT_FALSE(areShortcutsVisible(true, false, false, false));

        // Mobile shows shortcuts when a real physical keyboard is attached on hardware
        EXPECT_TRUE(areShortcutsVisible(true, true, false, false));

        // iOS Simulator / Android Emulator bridges host Mac/PC keyboard: must stay hidden by default
        EXPECT_FALSE(areShortcutsVisible(true, true, true, false));
        EXPECT_TRUE(areShortcutsVisible(true, true, true, true));

        // formatWithShortcut strips (%1) / （%1） / [%1] cleanly when shortcuts are hidden
        EXPECT_EQ(formatWithShortcut("Çalıştır (%1)", "F5", false, true), "Çalıştır");
        EXPECT_EQ(formatWithShortcut("Ağaç (%1)", "F2", false, true), "Ağaç");
        EXPECT_EQ(formatWithShortcut("Günlüğü Kapat（%1）", "Ctrl+L", false, true), "Günlüğü Kapat");
        EXPECT_EQ(formatWithShortcut("Execute (%1)", "", true, true), "Execute");
        EXPECT_EQ(formatWithShortcut("New Connection (%1)", "Ctrl+N", true, true), "New Connection (⌘N)");
    }
}
