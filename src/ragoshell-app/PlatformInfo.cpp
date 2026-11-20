#include "ragoshell-app/PlatformInfo.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QScreen>
#include <QScopedValueRollback>

#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/KeyLabel.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/UiDensity.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#ifdef Q_OS_IOS
namespace RagoShell
{
    // PlatformInfo_ios.mm (GameController, UIKit)
    void iosDisableSmartPunctuation();
    bool iosHardwareKeyboardPresent();
    void iosObserveHardwareKeyboard(QObject *target, const char *slot);
    void iosObserveMemoryWarning(QObject *target, const char *slot);
}
#endif

namespace RagoShell
{
    namespace
    {
        bool isSensitiveInputTarget(QObject *watched)
        {
            QObject *target = QGuiApplication::focusObject();
            if (!target)
                target = watched;
            if (!target)
                return false;
            QInputMethodQueryEvent query(Qt::ImHints);
            QCoreApplication::sendEvent(target, &query);
            const auto hints = Qt::InputMethodHints(query.value(Qt::ImHints).toInt());
            return (hints & (Qt::ImhHiddenText | Qt::ImhSensitiveData)) != 0;
        }

#ifdef Q_OS_ANDROID
        bool isAndroidEmulator()
        {
            const QString fingerprint = QJniObject::getStaticObjectField<jstring>(
                "android/os/Build", "FINGERPRINT").toString();
            const QString hardware = QJniObject::getStaticObjectField<jstring>(
                "android/os/Build", "HARDWARE").toString();
            const QString model = QJniObject::getStaticObjectField<jstring>(
                "android/os/Build", "MODEL").toString();
            return fingerprint.startsWith(QLatin1String("generic"))
                || fingerprint.contains(QLatin1String("emu64"))
                || fingerprint.contains(QLatin1String("vbox"))
                || hardware.contains(QLatin1String("goldfish"))
                || hardware.contains(QLatin1String("ranchu"))
                || model.contains(QLatin1String("sdk_gphone"))
                || model.contains(QLatin1String("Android SDK built for"))
                || model.contains(QLatin1String("Emulator"));
        }

        // Configuration.keyboard: 1 = no keys, 2 = qwerty, 3 = 12 keys.
        // Configuration.hardKeyboardHidden: 1 = HARDKEYBOARDHIDDEN_NO, 2 = HARDKEYBOARDHIDDEN_YES.
        bool androidHardwareKeyboard()
        {
            if (isAndroidEmulator() && RagoShell::productEnvInt("SIM_HW_KEYBOARD") <= 0)
                return false;

            QJniObject context = QNativeInterface::QAndroidApplication::context();
            if (!context.isValid())
                return false;
            QJniObject resources = context.callObjectMethod("getResources", "()Landroid/content/res/Resources;");
            if (!resources.isValid())
                return false;
            QJniObject config = resources.callObjectMethod("getConfiguration", "()Landroid/content/res/Configuration;");
            if (!config.isValid())
                return false;
            const jint keyboard = config.getField<jint>("keyboard");
            const jint hardHidden = config.getField<jint>("hardKeyboardHidden");
            return (keyboard == 2 || keyboard == 3) && hardHidden != 2;
        }
#endif
    }

    PlatformInfo::PlatformInfo(QObject *parent) :
        QObject(parent)
    {
        _formFactor = detectFormFactor();
        if (qApp)
            qApp->installEventFilter(this);

        if (isMobile()) {
#if defined(Q_OS_ANDROID)
            connect(qApp, &QGuiApplication::applicationStateChanged, this, &PlatformInfo::refreshHardwareKeyboard);
            _poll.setInterval(3000); // a keyboard can be connected while the app runs
            connect(&_poll, &QTimer::timeout, this, &PlatformInfo::refreshHardwareKeyboard);
            _poll.start();
#elif defined(Q_OS_IOS)
            iosDisableSmartPunctuation();
            iosObserveHardwareKeyboard(this, "refreshHardwareKeyboard");
            iosObserveMemoryWarning(this, "onMemoryWarningReceived");
#endif
            refreshHardwareKeyboard();
        }
    }

    QString PlatformInfo::detectFormFactor()
    {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        // The screen, not the window: a split window of a tablet is still a tablet. Qt's sizes are device
        // independent, so the limit is the usual 600 dp.
        static const QString factor = [] {
            int shortest = 0;
            if (const QScreen *screen = QGuiApplication::primaryScreen()) {
                const QSize size = screen->size();
                shortest = qMin(size.width(), size.height());
            }
            return shortest >= UiDensity::kTabletShortestSide ? QStringLiteral("tablet") : QStringLiteral("phone");
        }();
        return factor;
#else
        return QStringLiteral("desktop");
#endif
    }

    QString PlatformInfo::os() const
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
        return QStringLiteral("macos");
#elif defined(Q_OS_IOS)
        return QStringLiteral("ios");
#elif defined(Q_OS_ANDROID)
        return QStringLiteral("android");
#else
        return QStringLiteral("linux");
#endif
    }

    bool PlatformInfo::isMobile() const
    {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        return true;
#else
        return false;
#endif
    }

    bool PlatformInfo::usesCommandKey() const
    {
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
        return true;
#else
        return false;
#endif
    }

    bool PlatformInfo::shortcutsVisible() const
    {
        return RagoShell::areShortcutsVisible(isMobile(), hardwareKeyboard());
    }

    QString PlatformInfo::modLabel() const
    {
        return usesCommandKey() ? QStringLiteral("⌘") : QStringLiteral("Ctrl+");
    }

    QString PlatformInfo::keyLabel(const QString &sequence) const
    {
        return RagoShell::keyLabel(sequence, usesCommandKey());
    }

    QString PlatformInfo::shortcut(const QString &sequence) const
    {
        return shortcutsVisible() ? keyLabel(sequence) : QString();
    }

    QString PlatformInfo::macKeys(const QString &text) const
    {
        if (!usesCommandKey())
            return text;
        QString out = text;
        return out.replace(QStringLiteral("Ctrl+"), QStringLiteral("⌘"));
    }

    QString PlatformInfo::withShortcut(const QString &text, const QString &sequence) const
    {
        return RagoShell::formatWithShortcut(text, sequence, shortcutsVisible(), usesCommandKey());
    }

    void PlatformInfo::attach(QQmlEngine *engine)
    {
        static QPointer<QQmlEngine> attached;
        if (!engine || attached)
            return;
        attached = engine;
        connect(this, &PlatformInfo::hardwareKeyboardChanged, engine, &QQmlEngine::retranslate);
    }

    void PlatformInfo::setDetected(bool present)
    {
        if (_detected == present)
            return;
        _detected = present;
        emit hardwareKeyboardChanged();
    }

    void PlatformInfo::refreshHardwareKeyboard()
    {
#if defined(Q_OS_ANDROID)
        setDetected(androidHardwareKeyboard());
#elif defined(Q_OS_IOS)
        setDetected(iosHardwareKeyboardPresent());
#endif
    }

    bool PlatformInfo::eventFilter(QObject *watched, QEvent *event)
    {
        if (!_rewritingInput) {
            if (event->type() == QEvent::KeyPress) {
                auto *ke = static_cast<QKeyEvent *>(event);
                if (QtUtils::containsSmartQuotes(ke->text()) && !isSensitiveInputTarget(watched)) {
                    const QString normText = QtUtils::normalizeSmartQuotes(ke->text());
                    int normKey = ke->key();
                    if (normText == QLatin1String("'"))
                        normKey = Qt::Key_Apostrophe;
                    else if (normText == QLatin1String("\""))
                        normKey = Qt::Key_QuoteDbl;

                    QKeyEvent normalized(ke->type(), normKey, ke->modifiers(),
                                         ke->nativeScanCode(), ke->nativeVirtualKey(),
                                         ke->nativeModifiers(), normText,
                                         ke->isAutoRepeat(), static_cast<quint16>(ke->count()),
                                         ke->device());
                    QScopedValueRollback<bool> guard(_rewritingInput, true);
                    QCoreApplication::sendEvent(watched, &normalized);
                    ke->setAccepted(normalized.isAccepted());
                    return true;
                }
            } else if (event->type() == QEvent::InputMethod) {
                auto *ime = static_cast<QInputMethodEvent *>(event);
                if ((QtUtils::containsSmartQuotes(ime->commitString())
                     || QtUtils::containsSmartQuotes(ime->preeditString()))
                    && !isSensitiveInputTarget(watched)) {
                    QInputMethodEvent normalized(QtUtils::normalizeSmartQuotes(ime->preeditString()),
                                                 ime->attributes());
                    normalized.setCommitString(QtUtils::normalizeSmartQuotes(ime->commitString()),
                                               ime->replacementStart(),
                                               ime->replacementLength());
                    QScopedValueRollback<bool> guard(_rewritingInput, true);
                    QCoreApplication::sendEvent(watched, &normalized);
                    ime->setAccepted(normalized.isAccepted());
                    return true;
                }
            }
        }

        return QObject::eventFilter(watched, event);
    }

    void PlatformInfo::onMemoryWarningReceived()
    {
        LOG_MSG(QStringLiteral("OS memory warning received: trimming caches"), LogSeverity::Warning, false);
        emit memoryWarningReceived();
    }
}
