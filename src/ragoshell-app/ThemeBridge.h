#pragma once

#include <QObject>
#include <QColor>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include "ragoshell/core/utils/UiDensity.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    class SettingsManager;

    /**
     * @brief QML-facing theme provider. Exposes the persisted uiTheme
     *        (System/Light/Dark) and the resolved brand palette as
     *        Q_PROPERTY colors so QML can bind directly.
     */
    class ThemeBridge : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(Theme)
        QML_SINGLETON

        Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged FINAL)
        Q_PROPERTY(QStringList availableThemes READ availableThemes CONSTANT FINAL)

        // Interface density: "Automatic", "Compact" or "Touch" (docs/DesignSystem.md)
        Q_PROPERTY(QString density READ density WRITE setDensity NOTIFY densityChanged FINAL)
        Q_PROPERTY(QStringList availableDensities READ availableDensities CONSTANT FINAL)
        Q_PROPERTY(bool touch READ touch NOTIFY densityChanged FINAL)
        Q_PROPERTY(int controlHeight READ controlHeight NOTIFY densityChanged FINAL)
        Q_PROPERTY(int fieldHeight READ fieldHeight NOTIFY densityChanged FINAL)
        Q_PROPERTY(int iconButtonSize READ iconButtonSize NOTIFY densityChanged FINAL)
        Q_PROPERTY(int menuItemHeight READ menuItemHeight NOTIFY densityChanged FINAL)
        Q_PROPERTY(int splitHandle READ splitHandle NOTIFY densityChanged FINAL)

        // Resolved palette (changes with theme / OS scheme)
        Q_PROPERTY(QColor windowColor READ windowColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor baseColor READ baseColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor alternateColor READ alternateColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor textColor READ textColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor disabledTextColor READ disabledTextColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor buttonColor READ buttonColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor borderColor READ borderColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor hoverColor READ hoverColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor selectedColor READ selectedColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor accentColor READ accentColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor onAccentColor READ onAccentColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor surfaceColor READ surfaceColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor glassBackground READ glassBackground NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor glassBorder READ glassBorder NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor glassBorderHover READ glassBorderHover NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor accentGlow READ accentGlow NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor cardBackground READ cardBackground NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor cardBackgroundHover READ cardBackgroundHover NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor statusOnline READ statusOnline CONSTANT FINAL)
        Q_PROPERTY(QColor statusOffline READ statusOffline CONSTANT FINAL)
        Q_PROPERTY(QColor dangerColor READ dangerColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(QColor warningColor READ warningColor NOTIFY paletteChanged FINAL)
        Q_PROPERTY(bool dark READ dark NOTIFY paletteChanged FINAL)

    public:
        explicit ThemeBridge(QObject *parent = nullptr);
        static ThemeBridge *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<ThemeBridge>::get();
        }

        QString theme() const;
        void setTheme(const QString &theme);

        QStringList availableThemes() const { return {"System", "Light", "Dark"}; }

        QString density() const;
        void setDensity(const QString &density);
        /// For dev checks (--density): changes the density for this run only, nothing is saved.
        void overrideDensity(const QString &density);
        QStringList availableDensities() const { return {"Automatic", "Compact", "Touch"}; }
        bool touch() const;
        int controlHeight() const { return sizes().control; }
        int fieldHeight() const { return sizes().field; }
        int iconButtonSize() const { return sizes().iconButton; }
        int menuItemHeight() const { return sizes().menuItem; }
        int splitHandle() const { return sizes().splitHandle; }

        QColor windowColor() const;
        QColor baseColor() const;
        QColor alternateColor() const;
        QColor textColor() const;
        QColor disabledTextColor() const;
        QColor buttonColor() const;
        QColor borderColor() const;
        QColor hoverColor() const;
        QColor selectedColor() const;
        QColor accentColor() const;
        QColor onAccentColor() const;
        QColor surfaceColor() const;
        QColor glassBackground() const;
        QColor glassBorder() const;
        QColor glassBorderHover() const;
        QColor accentGlow() const;
        QColor cardBackground() const;
        QColor cardBackgroundHover() const;
        QColor statusOnline() const { return QColor("#00e668"); }
        QColor statusOffline() const { return QColor("#ff5c5c"); }
        QColor dangerColor() const { return _dark ? QColor("#ff5c5c") : QColor("#dc2626"); }
        QColor warningColor() const { return _dark ? QColor("#fbbf24") : QColor("#b45309"); }
        bool dark() const;

        /// Resolve "System" against the OS color scheme (dark OS -> Dark
        /// palette, light OS -> Light).
        Q_INVOKABLE QString effectiveTheme() const;

    Q_SIGNALS:
        void themeChanged();
        void densityChanged();
        void paletteChanged();

    private:
        UiDensity::Sizes sizes() const;
        void refresh();

        SettingsManager *_settings;
        QString _theme;
        QString _density;
        bool _dark = false;
        QString _window, _base, _alternate, _text, _disabledText, _button,
            _border, _hover, _selected, _accent, _onAccent, _surface,
            _glassBackground, _glassBorder, _glassBorderHover, _accentGlow,
            _cardBackground, _cardBackgroundHover;
    };
}
