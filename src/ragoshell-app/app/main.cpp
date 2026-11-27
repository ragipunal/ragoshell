#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>
#include <QFontDatabase>
#include <QSslSocket>
#include <QSysInfo>
#include <QTimer>

#include <locale.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/settings/ConnectionSettings.h"

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/services/UiServices.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppLog.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell/core/utils/WasmMode.h"
#include "ragoshell/ssh/ssh.h"
#include "ragoshell-app/CallTipSelfTest.h"
#include "ragoshell-app/ImeSelfTest.h"
#include "ragoshell-app/MatrixSelfTest.h"
#include "ragoshell-app/ShellSelfTest.h"
#include "ragoshell-app/LanguageManager.h"
#include "ragoshell-app/TabModelSelfTest.h"
#include "ragoshell/core/BrandInfo.h"
#include "ragoshell-app/BrandBridge.h"
#include "ragoshell-app/PlatformInfo.h"
#include "ragoshell-app/ThemeBridge.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"
#include "ragoshell-app/QmlUiServices.h"
#include "ragoshell-app/QueryHistoryModel.h"
#include "ragoshell-app/ScriptDraftModel.h"
#include "ragoshell-app/ConnectionListModel.h"
#include "ragoshell-app/DraftsSelfTest.h"
#include "ragoshell-app/ExplorerSelfTest.h"
#include "ragoshell-app/ResultSelectionSelfTest.h"
#include "ragoshell-app/ScintillaQuick.h"
#include "ragoshell-app/ShellController.h"
#include "ragoshell-app/McpSettingsBridge.h"
#include "ragoshell-app/SettingsBridge.h"
#include "ragoshell-app/CloudService.h"
#include "ragoshell-app/UpdateService.h"

#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
#include <QSystemTrayIcon>
#include <QMenu>
#include <QMessageBox>
#endif
#include <QIcon>
#include <QCursor>
#include <QScreen>
#include <QWindow>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QImage>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <algorithm>

namespace
{
    // Window geometry in the app's QSettings store, own group.
    constexpr auto kWindowGroup = "QmlMainWindow";

    /// Restores the last normal geometry if it is still mostly on a connected
    /// screen; returns false so the caller can fall back to centering.
    /// *maximized tells the caller to showMaximized() (show() would reset it).
    bool restoreWindowGeometry(QQuickWindow *window, bool *maximizedOut)
    {
        const QString brand = QString::fromUtf8(RagoShell::BrandInfo::productName);
        QSettings settings(brand, brand);
        settings.beginGroup(kWindowGroup);
        const QRect rect = settings.value("geometry").toRect();
        const bool maximized = settings.value("maximized").toBool();
        settings.endGroup();
        if (!rect.isValid())
            return false;

        for (QScreen *screen : QGuiApplication::screens()) {
            const QRect visible = screen->availableGeometry().intersected(rect);
            // At least a third of the window (and its title bar) must be reachable.
            if (visible.width() * 3 >= rect.width() && visible.height() * 3 >= rect.height()
                && screen->availableGeometry().contains(QPoint(rect.center().x(), rect.top() + 10))) {
                window->setScreen(screen);
                window->setGeometry(rect);
                *maximizedOut = maximized;
                return true;
            }
        }
        return false;
    }

    void saveWindowGeometry(QQuickWindow *window)
    {
        const QString brand = QString::fromUtf8(RagoShell::BrandInfo::productName);
        QSettings settings(brand, brand);
        settings.beginGroup(kWindowGroup);
        const bool maximized = window->windowStates().testFlag(Qt::WindowMaximized);
        settings.setValue("maximized", maximized);
        // Keep the last normal rect while maximized/minimized/fullscreen so
        // un-maximizing after a restart returns to it.
        if (window->windowStates() == Qt::WindowNoState)
            settings.setValue("geometry", window->geometry());
        settings.endGroup();
    }
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
    // Pure GUI subsystem (/SUBSYSTEM:WINDOWS): never spawn a console window,
    // but attach to an existing parent terminal when CLI flags are passed
    // interactively and stderr is not already redirected to a pipe/file.
    if (argc > 1) {
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD fileType = (hErr && hErr != INVALID_HANDLE_VALUE)
                             ? GetFileType(hErr)
                             : FILE_TYPE_UNKNOWN;
        bool stderrIsReachable = fileType != FILE_TYPE_UNKNOWN;   // file, pipe or console
        if (fileType == FILE_TYPE_UNKNOWN && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            stderrIsReachable = true;
        }
        // Qt's default message handler sends messages to the debugger instead of
        // stderr in a GUI-subsystem exe; CLI runs (--check-qml, self-tests,
        // --snapshot) want them on stderr, as the fprintf calls they replaced did.
        if (stderrIsReachable && qEnvironmentVariableIsEmpty("QT_FORCE_STDERR_LOGGING"))
            qputenv("QT_FORCE_STDERR_LOGGING", "1");
    }
#endif

    if (rbm_ssh_init())
        return 1;

    // mongocxx requires exactly one instance per process
    mongocxx::instance mongoInstance{};

    // Use Basic style so QML has full GPU styling control (no native Win32 boxes)
    QQuickStyle::setStyle("Basic");
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
    // Use native OS glyph rasterization (DirectWrite/ClearType on Windows, CoreText on macOS,
    // FreeType hinting on Linux) for crisp, high-legibility code and data tables.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
#endif

    // QApplication (not QGuiApplication) is required because the QML
    // Scintilla item uses a hidden QWidget as its platform surface.
    QApplication app(argc, argv);
    setlocale(LC_NUMERIC, "C");

    app.setApplicationName(PROJECT_NAME_TITLE);
    app.setApplicationVersion(PROJECT_VERSION);
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    // Fallback for the symbol glyphs the icons use (see src/ragoshell-app/CMakeLists.txt).
    if (QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/DejaVuSans.ttf")) < 0)
        qWarning() << "The bundled symbol font could not be loaded; some icons may show as boxes";
#endif
    app.setOrganizationName(QString::fromUtf8(RagoShell::BrandInfo::productName));
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // The name of install/linux/<product id>.desktop: the desktop shows its icon for the window (Wayland app id).
    app.setDesktopFileName(QString::fromUtf8(RagoShell::BrandInfo::productId));
#endif
    app.setWindowIcon(QIcon(QStringLiteral(":/qml/app_icon.png")));

    // Dev/CI helper: --ime-selftest drives the editor with scripted input
    // method events and exits with the number of failed checks.
    if (app.arguments().contains(QStringLiteral("--ime-selftest")))
        return RagoShell::runInputMethodSelfTest();
    // --shell-selftest=host:port: scripts through the shell backend against a MongoDB server.
    for (const QString &arg : app.arguments()) {
        const QString prefix = QStringLiteral("--shell-selftest=");
        if (arg.startsWith(prefix))
            return RagoShell::runShellSelfTest(arg.mid(prefix.size()));
        const QString matrixPrefix = QStringLiteral("--matrix-selftest=");
        if (arg.startsWith(matrixPrefix)) {
            const int rc = RagoShell::runMatrixSelfTest(arg.mid(matrixPrefix.size()));
            rbm_ssh_cleanup();
            return rc;
        }
    }
    // --calltip-selftest: shell call tips on real editor text.
    if (app.arguments().contains(QStringLiteral("--calltip-selftest")))
        return RagoShell::runCallTipSelfTest();
    // --tabs-selftest: the same for the tab strip model.
    if (app.arguments().contains(QStringLiteral("--tabs-selftest")))
        return RagoShell::runTabModelSelfTest();
    // --drafts-selftest: the script drafts model.
    if (app.arguments().contains(QStringLiteral("--drafts-selftest")))
        return RagoShell::runDraftsSelfTest();

    // Log file (appLogDir()): LOG_MSG lines, worker log events and every Qt
    // message. Qt's own handler still runs, so stderr / logcat / os_log work as before.
    if (RagoShell::AppLog::install(RagoShell::appLogDir())) {
        qInfo().noquote() << RagoShell::BrandInfo::productName << PROJECT_VERSION << "started on"
                          << QSysInfo::prettyProductName() << "with Qt" << qVersion()
                          << "- log file:" << RagoShell::AppLog::filePath();
#if defined(Q_OS_ANDROID)
        // Qt Network needs bundled OpenSSL on Android (QT_ANDROID_EXTRA_LIBS);
        // log it so the AI assistant's HTTPS state is diagnosable (M15).
        qInfo().noquote() << "QSslSocket::supportsSsl():" << QSslSocket::supportsSsl();
#endif
    } else {
        qWarning().noquote() << "Cannot open the log file in" << RagoShell::appLogDir();
    }

    // Load persisted settings (appDataDir(): ~/.ragoshell on desktop)
    auto *settings = RagoShell::AppRegistry::instance().settingsManager();

    // UI language: translators must be installed before any model or QML
    // item builds a translated string. --lang <code> overrides the stored
    // choice for this run only; --qm-file <file.qm> adds a catalog on top
    // (e.g. the pseudo-locale from ci/pseudo-translate.py).
    RagoShell::LanguageManager languageManager(settings);
    {
        const QStringList argv = app.arguments();
        const int li = argv.indexOf(QStringLiteral("--lang"));
        languageManager.initialize(li >= 0 && li + 1 < argv.size() ? argv.at(li + 1) : QString());
        const int qi = argv.indexOf(QStringLiteral("--qm-file"));
        if (qi >= 0 && qi + 1 < argv.size())
            languageManager.installExtraCatalog(argv.at(qi + 1));
    }

    RagoShell::QmlUiServices uiServices;
    RagoShell::UiServices::setInstance(&uiServices);

    const QStringList startupArgs = app.arguments();
    const bool headlessRun = startupArgs.contains(QStringLiteral("--check-qml"))
                          || startupArgs.contains(QStringLiteral("--check-geometry"))
                          || startupArgs.contains(QStringLiteral("--explorer-selftest"))
                          || startupArgs.contains(QStringLiteral("--selection-selftest"))
                          || startupArgs.contains(QStringLiteral("--snapshot"))
                          || startupArgs.contains(QStringLiteral("--open-view"));

    RagoShell::BrandBridge brandBridge;
    RagoShell::ThemeBridge themeBridge;
    RagoShell::PlatformInfo platformInfo;
    if (headlessRun && startupArgs.contains(QStringLiteral("--wasm-playground-preview")))
        platformInfo.enableHeadlessPlaygroundPreview();
    RagoShell::ConnectionListModel connectionModel(settings);
    // Declared before the backend: shell controllers record into it until
    // the backend (and its shells) are destroyed.
    RagoShell::QueryHistoryModel queryHistory(
        RagoShell::appDataDir() + QStringLiteral("query_history.json"));
    // Collection schemas analyzed for the AI assistant, per connection and database.
    RagoShell::SchemaCatalog schemaCatalog(RagoShell::appDataDir() + QStringLiteral("schema"));
    RagoShell::SchemaCatalog::setInstance(&schemaCatalog);
    // Every schema sampling goes through this queue (docs/SchemaIntelligence.md).
    RagoShell::SchemaService schemaService(&schemaCatalog);
    RagoShell::SchemaService::setInstance(&schemaService);
    // Unsaved script text of shell tabs, recoverable after a crash. Before the
    // backend too: shell controllers update it until the backend is destroyed.
    RagoShell::ScriptDraftModel scriptDrafts(RagoShell::appDataDir() + QStringLiteral("drafts.json"));
    scriptDrafts.setStartupPromptEnabled(!startupArgs.contains(QStringLiteral("--snapshot"))
                                         && !platformInfo.isWasmPlayground());
    QString playgroundAutoUuid;
#if defined(Q_OS_WASM)
    // Mod A playground: ensure the in-browser demo catalog exists so first
    // launch connects immediately without server setup (docs/WasmPlaygroundUx.md).
    {
        RagoShell::ConnectionSettings *memoryConn = nullptr;
        for (RagoShell::ConnectionSettings *c : settings->connections()) {
            if (c && c->transportKind() == "memory") {
                memoryConn = c;
                break;
            }
        }
        if (!memoryConn && (platformInfo.isWasmPlayground() || settings->connections().empty())) {
            auto *demo = new RagoShell::ConnectionSettings(mongocxx::uri("mongodb://demo/"), false);
            demo->setConnectionName(
                RagoShell::WasmMode::defaultPlaygroundConnectionName(
                    QString::fromUtf8(RagoShell::BrandInfo::productName)).toStdString());
            demo->setDefaultDatabase("catalog");
            demo->setTransportKind("memory");
            RagoShell::SettingsManager::addConnection(demo);
            memoryConn = demo;
            connectionModel.refresh();
        } else if (memoryConn && platformInfo.isWasmPlayground()
                   && memoryConn->defaultDatabase() != "catalog") {
            memoryConn->setDefaultDatabase("catalog");
        }
        if (memoryConn && platformInfo.isWasmPlayground())
            playgroundAutoUuid = memoryConn->uuid();
    }
#endif

    RagoShell::QmlBackend backend;
    // Dev/CI snapshot runs must not read or overwrite the remembered layout.
    backend.setPersistUiState(!startupArgs.contains(QStringLiteral("--snapshot")));
    if (platformInfo.isWasmPlayground() && !headlessRun)
        backend.tabs()->setWelcomeOpen(false);
    RagoShell::SettingsBridge settingsBridge(settings);
    RagoShell::McpSettingsBridge mcpSettings(settings);
    // A deleted connection also leaves the MCP allow-list, here and at start
    // (one removed by sync since the last run); the counts follow.
    QObject::connect(&connectionModel, &RagoShell::ConnectionListModel::connectionsChanged,
                     &mcpSettings, &RagoShell::McpSettingsBridge::pruneMissing);
    if (!headlessRun)
        mcpSettings.pruneMissing();
    RagoShell::CloudService cloudService(settings, &platformInfo, queryHistory.store(), headlessRun);
    // Sync merges rewrite the store the model wraps (docs/SyncClient.md §10 R1).
    QObject::connect(&cloudService, &RagoShell::CloudService::syncHistoryChanged,
                     &queryHistory, &RagoShell::QueryHistoryModel::reloadFromStore);
    RagoShell::UpdateService updateService(settings, &platformInfo, &languageManager, headlessRun);
    updateService.attachBackend(&backend);
    if (RagoShell::productEnvInt("EXIT_ON_UPDATE_CHECK") == 1) {
        QObject::connect(&updateService, &RagoShell::UpdateService::checkCompleted, &app,
                         [](bool ok, bool, bool) {
            QCoreApplication::exit(ok ? 0 : 2);
        });
    }

    // Typed QML singletons (Brand, Theme, Backend, …) so qmlsc can compile the
    // bindings that use them; they must outlive the engine below.
    RagoShell::QmlSingletonInstance<RagoShell::BrandBridge>::set(&brandBridge);
    RagoShell::QmlSingletonInstance<RagoShell::ThemeBridge>::set(&themeBridge);
    RagoShell::QmlSingletonInstance<RagoShell::PlatformInfo>::set(&platformInfo);
    RagoShell::QmlSingletonInstance<RagoShell::QmlBackend>::set(&backend);
    RagoShell::QmlSingletonInstance<RagoShell::SettingsBridge>::set(&settingsBridge);
    RagoShell::QmlSingletonInstance<RagoShell::McpSettingsBridge>::set(&mcpSettings);
    RagoShell::QmlSingletonInstance<RagoShell::QueryHistoryModel>::set(&queryHistory);
    RagoShell::QmlSingletonInstance<RagoShell::ScriptDraftModel>::set(&scriptDrafts);
    RagoShell::QmlSingletonInstance<RagoShell::ConnectionListModel>::set(&connectionModel);
    RagoShell::QmlSingletonInstance<RagoShell::QmlUiServices>::set(&uiServices);
    RagoShell::QmlSingletonInstance<RagoShell::LanguageManager>::set(&languageManager);
    RagoShell::QmlSingletonInstance<RagoShell::CloudService>::set(&cloudService);
    RagoShell::QmlSingletonInstance<RagoShell::UpdateService>::set(&updateService);

    // Dropping caches on low-memory notification from the OS (M8)
    QObject::connect(&platformInfo, &RagoShell::PlatformInfo::memoryWarningReceived,
                     &backend, &RagoShell::QmlBackend::trimMemory);

    // A phone or tablet app is often ended without aboutToQuit: write what is pending when it
    // leaves the foreground (the debounce timers would otherwise lose a second or two).
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app,
                     [settings, &queryHistory, &updateService](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            return;
        updateService.flushSessionMetrics();
        queryHistory.flush();
        settings->save();
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [settings, &updateService, headlessRun]() {
        if (headlessRun)
            return;
        updateService.flushSessionMetrics();
        settings->save();
    });

    QQmlApplicationEngine engine;
    languageManager.setEngine(&engine);
    if (startupArgs.contains(QStringLiteral("--explorer-selftest"))) {
        backend.setPersistUiState(false);
        const int rc = RagoShell::runExplorerSelfTest(&engine, &backend);
        fflush(stderr);
        rbm_ssh_cleanup();
        return rc;
    }
    if (startupArgs.contains(QStringLiteral("--selection-selftest"))) {
        backend.setPersistUiState(false);
        const int rc = RagoShell::runResultSelectionSelfTest(&engine);
        fflush(stderr);
        rbm_ssh_cleanup();
        return rc;
    }
    QObject::connect(&languageManager, &RagoShell::LanguageManager::languageChanged,
                     &backend, &RagoShell::QmlBackend::retranslate);
    QObject::connect(&languageManager, &RagoShell::LanguageManager::languageChanged,
                     &cloudService, &RagoShell::CloudService::retranslate);
    QObject::connect(&languageManager, &RagoShell::LanguageManager::languageChanged,
                     &updateService, &RagoShell::UpdateService::retranslate);
    // ScintillaEditor is registered by the QML module (QML_NAMED_ELEMENT).
    QStringList qmlErrors;
    QObject::connect(&engine, &QQmlEngine::warnings, [&qmlErrors](const QList<QQmlError> &warnings) {
        for (const auto &w : warnings) {
            const QString s = w.toString();
            qmlErrors.append(s);
            qWarning().noquote() << "[qml error]" << s;
        }
    });
    engine.loadFromModule("RagoShell", "Main");
    if (app.arguments().contains(QStringLiteral("--check-qml"))) {
        const bool ok = !engine.rootObjects().isEmpty() && qmlErrors.isEmpty();
        fflush(stderr);
        _exit(ok ? 0 : 1);
    }
    if (engine.rootObjects().isEmpty()) {
        qCritical("[qml] failed to load Main.qml");
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        if (argc <= 1) {
            QMessageBox::critical(
                nullptr,
                QString::fromUtf8(RagoShell::BrandInfo::productName),
                QStringLiteral("Failed to load QML user interface (Main.qml).\n\n") +
                    qmlErrors.join(QLatin1Char('\n')));
        }
#endif
        return 1;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    // Dev/CI snapshot runs use a fixed, centered window and must not
    // overwrite the user's remembered geometry.
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    const bool rememberGeometry = false;
#else
    const bool rememberGeometry = !app.arguments().contains(QStringLiteral("--snapshot"))
                                 && !app.arguments().contains(QStringLiteral("--window-size"));
#endif
    if (window && rememberGeometry) {
        QObject::connect(&app, &QCoreApplication::aboutToQuit, window,
                         [window]() { saveWindowGeometry(window); });
    }
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    if (window) {
        // The system sizes the window (full screen, split view, Stage Manager); show it as it is.
        window->showFullScreen();
    }
    if (false) {
#else
    if (window) {
#endif
        bool startMaximized = false;
        if (rememberGeometry && restoreWindowGeometry(window, &startMaximized)) {
            // restored
        } else if (QScreen *cursorScreen = QGuiApplication::screenAt(QCursor::pos())) {
            window->setScreen(cursorScreen);
            const QRect avail = cursorScreen->availableGeometry();
            const int w = std::min(window->width(), avail.width());
            const int h = std::min(window->height(), avail.height());
            window->resize(w, h);
            window->setPosition(avail.x() + (avail.width() - w) / 2,
                                avail.y() + (avail.height() - h) / 2);
        }
        // Dev/CI helper: --window-size 480x700 (narrow-window layouts, Slide Over sizes).
        {
            const QStringList sizeArgs = app.arguments();
            const int wi = sizeArgs.indexOf(QStringLiteral("--window-size"));
            if (wi >= 0 && wi + 1 < sizeArgs.size()) {
                const QStringList parts = sizeArgs.at(wi + 1).split(QLatin1Char('x'));
                if (parts.size() == 2 && parts.at(0).toInt() > 0 && parts.at(1).toInt() > 0) {
                    startMaximized = false;
                    window->resize(parts.at(0).toInt(), parts.at(1).toInt());
                }
            }
        }
        if (startMaximized)
            window->showMaximized();
        else
            window->show();
        window->raise();
        window->requestActivate();
        qInfo("[window] title='%s', visible=%d, size=%dx%d",
              qPrintable(window->title()), window->isVisible(),
              window->width(), window->height());
#ifdef _WIN32
        SetForegroundWindow(reinterpret_cast<HWND>(window->winId()));
#endif
    }

#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
    // System tray icon (desktop platforms only, when minimize-to-tray is enabled).
    QSystemTrayIcon *tray = nullptr;
    if (settings->minimizeToTray() && QSystemTrayIcon::isSystemTrayAvailable()) {
        tray = new QSystemTrayIcon(QIcon(QStringLiteral(":/qml/app_icon.png")),
                                   &app);
        auto *menu = new QMenu();
        auto *showAction = menu->addAction(
            QObject::tr("Show %1").arg(QString::fromUtf8(RagoShell::BrandInfo::productName)));
        auto *quitAction = menu->addAction(QObject::tr("Quit"));
        tray->setContextMenu(menu);
        tray->setToolTip(QString::fromUtf8(RagoShell::BrandInfo::productName));
        QObject::connect(&languageManager, &RagoShell::LanguageManager::languageChanged, menu,
                         [showAction, quitAction]() {
            showAction->setText(
                QObject::tr("Show %1").arg(QString::fromUtf8(RagoShell::BrandInfo::productName)));
            quitAction->setText(QObject::tr("Quit"));
        });
        QObject::connect(showAction, &QAction::triggered, &app,
                         [window]() {
            if (window) {
                window->show();
                window->raise();
                window->requestActivate();
            }
        });
        QObject::connect(quitAction, &QAction::triggered, &app,
                         &QApplication::quit);
        tray->show();
    }
#endif

    QStringList args = app.arguments();

    int ti = args.indexOf("--theme");
    if (ti >= 0 && ti + 1 < args.size()) {
        themeBridge.setTheme(args.at(ti + 1));
    }

    // Dev/CI helper: --switch-lang <code> changes the UI language 2.5 s after
    // start, the way the Preferences picker does (live switch, nothing saved).
    const int swi = args.indexOf("--switch-lang");
    if (swi >= 0 && swi + 1 < args.size()) {
        const QString code = args.at(swi + 1);
        QTimer::singleShot(2500, &languageManager, [&languageManager, code]() {
            languageManager.setCurrent(code);
        });
    }

    // Dev/CI helpers for the layout check (docs/DesignSystem.md): --window-size WxH and
    // --density Touch|Compact stand in for a phone or tablet on a desktop, nothing is saved.
    const int wsi = args.indexOf("--window-size");
    if (wsi >= 0 && wsi + 1 < args.size() && window) {
        const QStringList wh = args.at(wsi + 1).split(QLatin1Char('x'));
        if (wh.size() == 2) {
            window->setMinimumSize(QSize(0, 0));
            window->resize(wh.at(0).toInt(), wh.at(1).toInt());
        }
    }
    const int dei = args.indexOf("--density");
    if (dei >= 0 && dei + 1 < args.size())
        themeBridge.overrideDensity(args.at(dei + 1));

    // --check-geometry [ms] (with --open-view, or --connect --shell for a tab): after the delay (2.5 s; a tab
    // that waits for a server needs more) lists every visible item that lies beyond the edge of the window or
    // of its dialog and exits with 1 if there is one (Main.qml, geometryReport).
    const int cgi = args.indexOf("--check-geometry");
    if (cgi >= 0 && window) {
        int geometryDelay = 2500;
        if (cgi + 1 < args.size()) {
            bool ok = false;
            const int v = args.at(cgi + 1).toInt(&ok);
            if (ok)
                geometryDelay = v;
        }
        QTimer::singleShot(geometryDelay, window, [window]() {
            QVariant report;
            QMetaObject::invokeMethod(window, "geometryReport", Q_RETURN_ARG(QVariant, report));
            const QStringList lines = report.toStringList();
            for (const QString &line : lines)
                fprintf(stdout, "[geometry] %s\n", qPrintable(line));
            fprintf(stdout, "[geometry] %lld item(s) beyond the window\n", static_cast<long long>(lines.size()));
            fflush(stdout);
            QCoreApplication::exit(lines.isEmpty() ? 0 : 1);
        });
    }

    int ovi = args.indexOf("--open-view");
    if (ovi >= 0 && ovi + 1 < args.size() && window) {
        const QString viewName = args.at(ovi + 1);
        QTimer::singleShot(100, window, [window, viewName]() {
            QMetaObject::invokeMethod(window, "openDebugView",
                                      Q_ARG(QVariant, viewName));
        });
    }
    // Dev/CI helper: --hostkey-answer trust|refuse answers SSH host key questions
    // without the dialog and reports them on stderr.
    const int hostKeyArg = args.indexOf("--hostkey-answer");
    if (hostKeyArg >= 0 && hostKeyArg + 1 < args.size()) {
        const bool trust = args.at(hostKeyArg + 1) == QLatin1String("trust");
        QObject::connect(&backend, &RagoShell::QmlBackend::hostKeyRequested, &backend,
                         [&backend, trust](int id, const QString &host, int port, const QString &keyType,
                                           const QString &fingerprint, bool changed) {
            qInfo("[ssh] host key %s: %s:%d %s %s -> %s", changed ? "CHANGED" : "new", qPrintable(host), port,
                  qPrintable(keyType), qPrintable(fingerprint), trust ? "trust" : "refuse");
            backend.answerHostKey(id, trust);
        });
    }

    // Dev/CI helper: --recover-drafts does what "Open" does for every draft in
    // the recovery list (connecting first when needed), and reports each tab.
    if (args.contains("--recover-drafts")) {
        // --close-recovered then closes each recovered tab again, as the user would.
        const bool closeAgain = args.contains("--close-recovered");
        QObject::connect(&backend, &RagoShell::QmlBackend::shellOpened, &backend,
                         [&backend, closeAgain](QObject *tab) {
            qInfo("[drafts] tab opened: title='%s' dirty=%d file='%s' script=%lld chars",
                  qPrintable(tab->property("title").toString()), tab->property("dirty").toBool(),
                  qPrintable(tab->property("filePath").toString()),
                  static_cast<long long>(tab->property("script").toString().size()));
            if (closeAgain)
                QTimer::singleShot(2500, &backend, [&backend, tab]() {
                    qInfo("[drafts] closing the tab");
                    backend.closeTabObject(tab);
                });
        });
        QObject::connect(&backend, &RagoShell::QmlBackend::connectionError,
                         &backend, [](const QString &, const QString &msg) {
            qWarning("[drafts] error: %s", qPrintable(msg));
        });
        QTimer::singleShot(500, &backend, [&backend, &scriptDrafts]() {
            QStringList ids;
            for (int row = 0; row < scriptDrafts.rowCount(); ++row)
                ids << scriptDrafts.data(scriptDrafts.index(row), RagoShell::ScriptDraftModel::IdRole).toString();
            qInfo("[drafts] %lld to recover", static_cast<long long>(ids.size()));
            for (const QString &id : std::as_const(ids))
                backend.recoverDraft(id);
        });
    }
    const bool exitAfterSnap = args.contains("--exit-after-snapshot");

    int ci = args.indexOf("--connect");
    if (ci >= 0 && ci + 1 < args.size()) {
        QString uuid = args.at(ci + 1);
        QObject::connect(&backend, &RagoShell::QmlBackend::statusTextChanged,
                         &backend, [&backend]() {
            qInfo("[status] %s", qPrintable(backend.statusText()));
        });
        QObject::connect(&backend, &RagoShell::QmlBackend::connectionError,
                         &backend, [](const QString &, const QString &msg) {
            qWarning("[error] %s", qPrintable(msg));
        });
        backend.connectToServer(uuid);

        // Dev/CI helper: --pipeline <db> <collection> [stagesJson] opens the
        // aggregation builder once databases are listed; stagesJson is
        // [["$match", "{...}"], ...].
        const int pi = args.indexOf("--pipeline");
        if (pi >= 0 && pi + 2 < args.size() && window) {
            const QString db = args.at(pi + 1);
            const QString coll = args.at(pi + 2);
            const QString stages = (pi + 3 < args.size() && !args.at(pi + 3).startsWith("--"))
                                       ? args.at(pi + 3) : QString();
            auto opened = std::make_shared<bool>(false);
            QObject::connect(&backend, &RagoShell::QmlBackend::statusTextChanged,
                             window, [&backend, window, uuid, db, coll, stages, opened]() {
                if (*opened || !backend.databasesListed())
                    return;
                *opened = true;
                QMetaObject::invokeMethod(window, "openDebugPipeline",
                                          Q_ARG(QVariant, uuid), Q_ARG(QVariant, db),
                                          Q_ARG(QVariant, coll), Q_ARG(QVariant, stages));
            });
        }

        // Dev/CI helper: --schema <db> <collection> opens schema analysis
        // once databases are listed.
        const int sci = args.indexOf("--schema");
        if (sci >= 0 && sci + 2 < args.size() && window) {
            const QString db = args.at(sci + 1);
            const QString coll = args.at(sci + 2);
            auto opened = std::make_shared<bool>(false);
            QObject::connect(&backend, &RagoShell::QmlBackend::statusTextChanged,
                             window, [&backend, window, uuid, db, coll, opened]() {
                if (*opened || !backend.databasesListed())
                    return;
                *opened = true;
                QMetaObject::invokeMethod(window, "openSchemaAnalysis", Q_ARG(QVariant, uuid),
                                          Q_ARG(QVariant, db), Q_ARG(QVariant, coll));
            });
        }

        // Dev/CI helper: --monitor opens the server monitor tab once
        // databases are listed.
        if (args.contains("--monitor") && window) {
            auto opened = std::make_shared<bool>(false);
            QObject::connect(&backend, &RagoShell::QmlBackend::statusTextChanged,
                             window, [&backend, uuid, opened]() {
                if (*opened || !backend.databasesListed())
                    return;
                *opened = true;
                backend.openMonitor(uuid);
            });
        }

        // Dev/CI helper: --ai <db> <collection> [question] opens the AI query
        // assistant once databases are listed (and asks the question).
        const int ai = args.indexOf("--ai");
        if (ai >= 0 && ai + 2 < args.size() && window) {
            const QString db = args.at(ai + 1);
            const QString coll = args.at(ai + 2);
            const QString question = ai + 3 < args.size() && !args.at(ai + 3).startsWith("--")
                                         ? args.at(ai + 3) : QString();
            auto opened = std::make_shared<bool>(false);
            QObject::connect(&backend, &RagoShell::QmlBackend::statusTextChanged,
                             window, [&backend, window, uuid, db, coll, question, opened]() {
                if (*opened || !backend.databasesListed())
                    return;
                *opened = true;
                QMetaObject::invokeMethod(window, "openDebugAiAssistant", Q_ARG(QVariant, uuid),
                                          Q_ARG(QVariant, db), Q_ARG(QVariant, coll),
                                          Q_ARG(QVariant, question));
            });
        }

        // Dev/CI helper: --shell <db> <collection> [--script <js>] opens a
        // shell on the server once databases are listed, executes
        // db.<collection>.find({}) (or the given script) and logs the
        // document count to stderr.
        int si = args.indexOf("--shell");
        if (si >= 0 && si + 2 < args.size()) {
            const QString shellUuid = uuid;
            const QString dbName = args.at(si + 1);
            const QString collection = args.at(si + 2);
            const int scriptIdx = args.indexOf("--script");
            const QString script = (scriptIdx >= 0 && scriptIdx + 1 < args.size())
                                       ? args.at(scriptIdx + 1) : QString();
            QObject::connect(&backend,
                             &RagoShell::QmlBackend::statusTextChanged,
                             &backend, [&backend, shellUuid, dbName, collection, script]() {
                if (!backend.databasesListed() || !backend.shells().isEmpty())
                    return;
                if (!script.isEmpty())
                    backend.openShellWithScript(shellUuid, dbName, script, true);
                else
                    // Collection arg exercises the real UI path:
                    // prefills db.<coll>.find({}) and auto-executes.
                    backend.openShell(shellUuid, dbName, collection);
            });
            // --calltip: once the shell tab is up, put the caret at the end of
            // the script and show its call tip (dev/CI view of the overlay).
            const bool showCallTip = args.contains("--calltip");
            QObject::connect(&backend, &RagoShell::QmlBackend::shellOpened,
                             &backend, [window, showCallTip](QObject *obj) {
                auto *ctl =
                    qobject_cast<RagoShell::ShellController *>(obj);
                if (!ctl)
                    return;
                if (showCallTip && window) {
                    QTimer::singleShot(2500, window, [window]() {
                        QMetaObject::invokeMethod(window, "openDebugView",
                                                  Q_ARG(QVariant, QStringLiteral("calltip")));
                    });
                }
                QObject::connect(ctl,
                                 &RagoShell::ShellController::resultChanged,
                                 ctl, [ctl]() {
                    if (ctl->busy())
                        return;
                    qInfo("[shell] %d doc(s), %lld ms%s",
                          ctl->resultCount(),
                          static_cast<long long>(ctl->elapsedMs()),
                          ctl->errorText().isEmpty()
                              ? ""
                              : qPrintable(" err=" + ctl->errorText()));
                });
            });
        }
    } else if (platformInfo.isWasmPlayground() && !headlessRun && !playgroundAutoUuid.isEmpty()) {
        auto starterOpened = std::make_shared<bool>(false);
        QObject::connect(&backend, &RagoShell::QmlBackend::statusTextChanged,
                         &backend, [&backend, playgroundAutoUuid, starterOpened]() {
            if (*starterOpened || !backend.databasesListed() || !backend.shells().isEmpty())
                return;
            *starterOpened = true;
            backend.openShellWithScript(
                playgroundAutoUuid,
                QStringLiteral("catalog"),
                RagoShell::WasmMode::playgroundStarterScript(),
                true);
        });
        backend.connectToServer(playgroundAutoUuid);
    }

    // Dev/CI helper: --snapshot <file.png> [delayMs] grabs the QML window
    // after a delay and saves a real rendered frame (PrintWindow can't
    // capture RHI/D3D content reliably).
    int snapIdx = args.indexOf("--snapshot");
    if (snapIdx >= 0 && snapIdx + 1 < args.size()) {
        const QString file = args.at(snapIdx + 1);
        int delayMs = 15000;
        if (snapIdx + 2 < args.size()) {
            bool ok = false;
            int v = args.at(snapIdx + 2).toInt(&ok);
            if (ok)
                delayMs = v;
        }
        auto *window =
            qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (window) {
            QTimer::singleShot(delayMs, window, [window, file, exitAfterSnap]() {
                QImage img = window->grabWindow();
                bool saved = img.save(file);
                qInfo("[snapshot] %s -> %s", saved ? "saved" : "FAILED", qPrintable(file));
                // Other visible QML windows (e.g. the detached log) go to
                // <file>-2.png, -3.png, ...
                int extra = 1;
                for (QWindow *w : QGuiApplication::topLevelWindows()) {
                    auto *other = qobject_cast<QQuickWindow *>(w);
                    if (!other || other == window || !other->isVisible())
                        continue;
                    const QFileInfo info(file);
                    const QString extraFile = info.path() + QLatin1Char('/') + info.completeBaseName()
                                              + QStringLiteral("-%1.").arg(++extra) + info.suffix();
                    const bool extraSaved = other->grabWindow().save(extraFile);
                    qInfo("[snapshot] %s -> %s",
                          extraSaved ? "saved" : "FAILED", qPrintable(extraFile));
                }
                if (exitAfterSnap)
                    QCoreApplication::quit();
            });
        }
    }

    int rc = app.exec();
    rbm_ssh_cleanup();
    return rc;
}
