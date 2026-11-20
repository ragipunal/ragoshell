#pragma once

#include <QString>

namespace RagoShell
{
    namespace UiDensity
    {
        /// True when the interface uses touch sizes: "Touch" always, "Compact" never,
        /// anything else ("Automatic") on phones and tablets only.
        inline bool isTouch(const QString &setting, bool mobilePlatform)
        {
            if (setting == QLatin1String("Touch"))
                return true;
            if (setting == QLatin1String("Compact"))
                return false;
            return mobilePlatform;
        }

        /// Recommended finger touch pitch (44 pt on iOS, 48 dp on Android), met by pairing
        /// minimalist control bounds (kMinTouchBox) with inter-control / row spacing.
        constexpr int kTouchTarget = 44;
        constexpr int kMinTouchBox = 36;

        /// A phone or tablet screen is a tablet from this shortest side on (dp), the usual 600 dp breakpoint.
        constexpr int kTabletShortestSide = 600;

        /// The control sizes of one column of the table (docs/DesignSystem.md).
        struct Sizes
        {
            int control;     ///< push button outer touch height, combo popup row
            int field;       ///< text field, combo box, spin box
            int iconButton;  ///< glyph button touch hit-box minimum
            int menuItem;
            int splitHandle;
        };

        /// Desktop (compact): 32 / 34 / 28 / 30 / 4.
        /// Tablet: 34 / 35 / 32 / 36 / 6.
        /// Phone (and "Touch" on desktop): 36 / 36 / 34 / 38 / 8 — minimalist visual chrome paired
        /// with full-box touch hit areas and spacing so finger targets stay comfortable without bloating UI.
        inline Sizes sizes(bool touch, bool tablet)
        {
            if (!touch)
                return { 32, 34, 28, 30, 4 };
            if (tablet)
                return { 34, 35, 32, 36, 6 };
            return { 36, 36, 34, 38, 8 };
        }
    }
}
