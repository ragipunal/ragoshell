#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    /**
     * @brief What the QML needs to know about the platform and its input devices (docs/DesignSystem.md,
     *        section 5). The one place that decides: which OS, desktop or touch device, phone or tablet,
     *        whether a hardware keyboard is attached, and how a key sequence is written here.
     *
     * Shortcut text is shown only where it works: always on a desktop, and on a phone or tablet only while
     * a real physical keyboard is attached (Android queries Configuration.keyboard / hardKeyboardHidden,
     * iOS/iPadOS observes GameController GCKeyboard connect/disconnect; Simulator and Emulator host keyboard
     * bridges are ignored unless RAGOSHELL_SIM_HW_KEYBOARD=1 is set).
     */
    class PlatformInfo : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(Platform)
        QML_SINGLETON

        /// "windows", "macos", "linux", "android", "ios" or "wasm"
        Q_PROPERTY(QString os READ os CONSTANT FINAL)
        Q_PROPERTY(bool isMobile READ isMobile CONSTANT FINAL)
        Q_PROPERTY(bool isWasm READ isWasm CONSTANT FINAL)
        Q_PROPERTY(bool isWasmPlayground READ isWasmPlayground CONSTANT FINAL)
        /// "desktop", "tablet" (a phone or tablet OS whose screen is at least 600 dp wide) or "phone"
        Q_PROPERTY(QString formFactor READ formFactor CONSTANT FINAL)
        Q_PROPERTY(bool isTablet READ isTablet CONSTANT FINAL)
        /// The command key is the ⌘ key (macOS and iPadOS), not Ctrl.
        Q_PROPERTY(bool usesCommandKey READ usesCommandKey CONSTANT FINAL)
        Q_PROPERTY(bool hardwareKeyboard READ hardwareKeyboard NOTIFY hardwareKeyboardChanged FINAL)
        /// Desktop: always. Phone or tablet: while a hardware keyboard is attached.
        Q_PROPERTY(bool shortcutsVisible READ shortcutsVisible NOTIFY hardwareKeyboardChanged FINAL)
        /// "⌘" or "Ctrl+"
        Q_PROPERTY(QString modLabel READ modLabel CONSTANT FINAL)

    public:
        explicit PlatformInfo(QObject *parent = nullptr);

        /// "desktop", "tablet" or "phone" (see formFactor); computed once, usable before the object exists.
        static QString detectFormFactor();

        static PlatformInfo *create(QQmlEngine *engine, QJSEngine *)
    {
        PlatformInfo *info = QmlSingletonInstance<PlatformInfo>::get();
        info->attach(engine);
        return info;
    }

        QString os() const;
        bool isMobile() const;
        bool isWasm() const;
        bool isWasmPlayground() const;
        /// Dev/CI headless layout verification only (--wasm-playground-preview); must be set before QML loads.
        void enableHeadlessPlaygroundPreview();
        QString formFactor() const { return _formFactor; }
        bool isTablet() const { return _formFactor == QLatin1String("tablet"); }
        bool usesCommandKey() const;
        bool hardwareKeyboard() const { return _detected; }
        bool shortcutsVisible() const;
        QString modLabel() const;

        /// A portable sequence ("Ctrl+Shift+S") as this platform writes it; "" when shortcuts are not shown.
        Q_INVOKABLE QString shortcut(const QString &sequence) const;
        /// The same without the visibility rule (for the shortcut reference, which is a keyboard topic).
        Q_INVOKABLE QString keyLabel(const QString &sequence) const;
        /// A translated sentence that names keys in its text ("press Ctrl+N"): on a Mac or iPad the Control key
        /// there is the Command key, so "Ctrl+" is written "⌘". Elsewhere the text is returned as it is.
        Q_INVOKABLE QString macKeys(const QString &text) const;
        /// A translated text that holds the key as its first argument ("Close Log (%1)"): the key is put in
        /// where shortcuts are shown; elsewhere the placeholder goes away with its brackets ("Close Log"), so
        /// the existing translations serve both.
        Q_INVOKABLE QString withShortcut(const QString &text, const QString &sequence) const;

    signals:
        void hardwareKeyboardChanged();
        void memoryWarningReceived();

    public slots:
        void onMemoryWarningReceived();

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override;

    private slots:
        void refreshHardwareKeyboard();

    private:
        void setDetected(bool present);
    /// A QML binding does not see the keyboard state through the invokables above; when it changes, the
    /// engine evaluates the bindings that hold a qsTr() again, which is where withShortcut() is used.
    void attach(QQmlEngine *engine);

        QString _formFactor;
        bool _detected = false;                  ///< the platform says a physical hardware keyboard is attached
        bool _rewritingInput = false;            ///< re-entrancy guard while normalizing smart quotes
        bool _wasmPlayground = false;            ///< true in WASM Mod A Playground (docs/WasmPlaygroundUx.md)
        bool _headlessPlaygroundPreview = false; ///< dev/CI headless --check-geometry / --open-view preview only
        QTimer _poll;
    };
}
