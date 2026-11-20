#include "ragoshell-app/ThemeBridge.h"

#include <QGuiApplication>
#include <QStyleHints>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/BackgroundPolicy.h"
#include "ragoshell/core/utils/UiDensity.h"
#include "ragoshell-app/PlatformInfo.h"

namespace RagoShell
{
    ThemeBridge::ThemeBridge(QObject *parent)
        : QObject(parent),
          _settings(AppRegistry::instance().settingsManager())
    {
        _theme = _settings->uiTheme();
        _density = _settings->uiDensity();
        refresh();

        // React to OS light/dark switches while "System" is selected.
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this](Qt::ColorScheme) {
                    if (_theme == "System")
                        refresh(), emit paletteChanged();
                });
    }

    QString ThemeBridge::theme() const { return _theme; }

    void ThemeBridge::setTheme(const QString &theme)
    {
        if (_theme == theme)
            return;
        _theme = theme;
        _settings->setUiTheme(theme);
        _settings->save();
        emit themeChanged();
        refresh();
        emit paletteChanged();
    }

    QString ThemeBridge::density() const { return _density; }

    void ThemeBridge::setDensity(const QString &density)
    {
        if (_density == density)
            return;
        _density = density;
        _settings->setUiDensity(density);
        _settings->save();
        emit densityChanged();
    }

    void ThemeBridge::overrideDensity(const QString &density)
    {
        if (_density == density)
            return;
        _density = density;
        emit densityChanged();
    }

    bool ThemeBridge::touch() const
    {
        return UiDensity::isTouch(_density, BackgroundPolicy::isMobilePlatform());
    }

    UiDensity::Sizes ThemeBridge::sizes() const
    {
        return UiDensity::sizes(touch(), PlatformInfo::detectFormFactor() == QLatin1String("tablet"));
    }

    QString ThemeBridge::effectiveTheme() const
    {
        if (_theme != "System")
            return _theme;
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                   ? "Dark"
                   : "Light";
    }

    void ThemeBridge::refresh()
    {
        _dark = effectiveTheme() == "Dark";
        if (_dark) {
            // macOS Liquid Obsidian & Dark Emerald palette
            _window = "#090d0b"; _base = "#0f1612"; _alternate = "#0b120e";
            _text = "#eef4f0"; _disabledText = "#677b71";
            _button = "#17231c"; _border = "#1f2f25"; _hover = "#1c2b22";
            _selected = "#1c3c2f"; _accent = "#00be54"; _onAccent = "#05140e";
            _surface = "#0c130f";
            _glassBackground = "#12ffffff";
            _glassBorder = "#24ffffff";
            _glassBorderHover = "#5534f28b";
            _accentGlow = "#00e668";
            _cardBackground = "#aa0f1712";
            _cardBackgroundHover = "#dd15221b";
        }
        else {
            // macOS Liquid Light palette
            _window = "#f3f6f4"; _base = "#ffffff"; _alternate = "#ebf1ed";
            _text = "#14201a"; _disabledText = "#566b60";
            _button = "#e5eee8"; _border = "#d2ded7"; _hover = "#dce7e1";
            _selected = "#c5ebd8"; _accent = "#00a344"; _onAccent = "#ffffff";
            _surface = "#f8faf9";
            _glassBackground = "#0a000000";
            _glassBorder = "#18000000";
            _glassBorderHover = "#5000a344";
            _accentGlow = "#00be54";
            _cardBackground = "#d5ffffff";
            _cardBackgroundHover = "#f2ffffff";
        }
    }

    QColor ThemeBridge::windowColor() const { return _window; }
    QColor ThemeBridge::baseColor() const { return _base; }
    QColor ThemeBridge::alternateColor() const { return _alternate; }
    QColor ThemeBridge::textColor() const { return _text; }
    QColor ThemeBridge::disabledTextColor() const { return _disabledText; }
    QColor ThemeBridge::buttonColor() const { return _button; }
    QColor ThemeBridge::borderColor() const { return _border; }
    QColor ThemeBridge::hoverColor() const { return _hover; }
    QColor ThemeBridge::selectedColor() const { return _selected; }
    QColor ThemeBridge::accentColor() const { return _accent; }
    QColor ThemeBridge::onAccentColor() const { return _onAccent; }
    QColor ThemeBridge::surfaceColor() const { return _surface; }
    QColor ThemeBridge::glassBackground() const { return _glassBackground; }
    QColor ThemeBridge::glassBorder() const { return _glassBorder; }
    QColor ThemeBridge::glassBorderHover() const { return _glassBorderHover; }
    QColor ThemeBridge::accentGlow() const { return _accentGlow; }
    QColor ThemeBridge::cardBackground() const { return _cardBackground; }
    QColor ThemeBridge::cardBackgroundHover() const { return _cardBackgroundHover; }
    bool ThemeBridge::dark() const { return _dark; }
}
