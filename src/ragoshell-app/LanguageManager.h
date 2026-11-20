#pragma once

#include <QFont>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <vector>

#include "ragoshell-app/QmlSingleton.h"

class QQmlEngine;
class QTranslator;

namespace RagoShell
{
    class SettingsManager;

    /**
     * @brief UI language of the app: loads the Qt and application
     *        translations, switches them live, and sets the layout
     *        direction and the fallback fonts. See docs/Localization.md.
     *
     *        The stored setting is "system" or a code from LanguageCatalog;
     *        effective is the code actually in use.
     */
    class LanguageManager : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(Language)
        QML_SINGLETON

        /// [{code, endonym, englishName, rtl}] in picker order.
        Q_PROPERTY(QVariantList languages READ languages CONSTANT FINAL)
        /// Stored choice: "system" or a language code.
        Q_PROPERTY(QString current READ current WRITE setCurrent NOTIFY currentChanged FINAL)
        /// Language code in use after resolving "system".
        Q_PROPERTY(QString effective READ effective NOTIFY languageChanged FINAL)
        Q_PROPERTY(QString effectiveEndonym READ effectiveEndonym NOTIFY languageChanged FINAL)
        Q_PROPERTY(bool rtl READ rtl NOTIFY languageChanged FINAL)

    public:
        explicit LanguageManager(SettingsManager *settings, QObject *parent = nullptr);
        ~LanguageManager() override;

        static LanguageManager *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<LanguageManager>::get();
        }

        /// The engine whose bindings are re-evaluated on a language switch.
        void setEngine(QQmlEngine *engine);

        /// Installs the translators for the stored setting (or for
        /// overrideCode, a --lang value, without storing it). Call before the
        /// QML engine loads the UI.
        void initialize(const QString &overrideCode = QString());

        /// Installs one more .qm file on top of the current translators
        /// (dev/CI: the pseudo-locale). Lost on the next language switch.
        bool installExtraCatalog(const QString &qmFile);

        QVariantList languages() const;
        QString current() const;
        void setCurrent(const QString &code);
        QString effective() const { return _effective; }
        QString effectiveEndonym() const;
        bool rtl() const { return _rtl; }

        /// Endonym of the language "system default" resolves to right now.
        Q_INVOKABLE QString systemEndonym() const;
        /// Endonym of a language code (the code itself when unknown).
        Q_INVOKABLE QString endonym(const QString &code) const;
        /// Wraps a database name, host, path ... in Unicode isolates for use
        /// inside translated text (docs/Localization.md).
        Q_INVOKABLE QString isolate(const QString &text) const;

    Q_SIGNALS:
        void currentChanged();
        /// Just before the QML bindings are re-evaluated for the new language.
        /// A ComboBox whose model holds translated text resets its currentIndex
        /// to 0 when that happens; controls that care save their state here
        /// and restore it on languageChanged().
        void languageAboutToChange();
        void languageChanged();

    private:
        void apply(const QString &setting);
        void removeTranslators();
        void installTranslator(const QString &resourcePath);
        void applyFonts(const QString &code);

        SettingsManager *_settings;
        QQmlEngine *_engine = nullptr;
        QString _effective = QStringLiteral("en");
        bool _rtl = false;
        QFont _baseFont;
        std::vector<std::unique_ptr<QTranslator>> _translators;
    };
}
