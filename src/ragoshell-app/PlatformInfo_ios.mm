// iPadOS: is a hardware keyboard attached? (docs/DesignSystem.md)
//
// The GameController framework reports keyboards (Smart Keyboard, Magic Keyboard, Bluetooth) with
// connect and disconnect notifications; the on-screen keyboard is not one.

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#include <TargetConditionals.h>
#include <notify.h>
#include <objc/runtime.h>

#include <QByteArray>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>

#include "ragoshell/core/utils/AppPaths.h"

namespace
{
    UITextSmartQuotesType ragoSmartQuotesType(id, SEL)
    {
        return UITextSmartQuotesTypeNo;
    }

    void ragoSetSmartQuotesType(id, SEL, UITextSmartQuotesType)
    {
    }

    UITextSmartDashesType ragoSmartDashesType(id, SEL)
    {
        return UITextSmartDashesTypeNo;
    }

    void ragoSetSmartDashesType(id, SEL, UITextSmartDashesType)
    {
    }

    UITextSmartInsertDeleteType ragoSmartInsertDeleteType(id, SEL)
    {
        return UITextSmartInsertDeleteTypeNo;
    }

    void ragoSetSmartInsertDeleteType(id, SEL, UITextSmartInsertDeleteType)
    {
    }

    API_AVAILABLE(ios(17.0))
    UITextInlinePredictionType ragoInlinePredictionType(id, SEL)
    {
        return UITextInlinePredictionTypeNo;
    }

    API_AVAILABLE(ios(17.0))
    void ragoSetInlinePredictionType(id, SEL, UITextInlinePredictionType)
    {
    }
}

namespace RagoShell
{
    void iosDisableSmartPunctuation()
    {
        // Qt 6's QIOSTextInputResponder adopts UITextInputTraits for autocapitalization,
        // autocorrection, and spellChecking, but leaves smartQuotesType, smartDashesType,
        // smartInsertDeleteType, and inlinePredictionType unimplemented, so UIKit defaults to
        // converting ' / " into curly quotes and -- into an em-dash on both virtual and Mac/Magic
        // keyboards. Registering explicit No-returning trait methods disables Smart Punctuation
        // across every text field and code editor in the app.
        static bool installed = false;
        if (installed)
            return;
        installed = true;

        Class cls = objc_getClass("QIOSTextInputResponder");
        if (!cls)
            return;

        const QByteArray getterTypes = QByteArray(@encode(UITextSmartQuotesType)) + "@:";
        const QByteArray setterTypes = QByteArray("v@:") + @encode(UITextSmartQuotesType);

        class_replaceMethod(cls, @selector(smartQuotesType),
                            reinterpret_cast<IMP>(ragoSmartQuotesType), getterTypes.constData());
        class_replaceMethod(cls, @selector(setSmartQuotesType:),
                            reinterpret_cast<IMP>(ragoSetSmartQuotesType), setterTypes.constData());

        class_replaceMethod(cls, @selector(smartDashesType),
                            reinterpret_cast<IMP>(ragoSmartDashesType), getterTypes.constData());
        class_replaceMethod(cls, @selector(setSmartDashesType:),
                            reinterpret_cast<IMP>(ragoSetSmartDashesType), setterTypes.constData());

        class_replaceMethod(cls, @selector(smartInsertDeleteType),
                            reinterpret_cast<IMP>(ragoSmartInsertDeleteType), getterTypes.constData());
        class_replaceMethod(cls, @selector(setSmartInsertDeleteType:),
                            reinterpret_cast<IMP>(ragoSetSmartInsertDeleteType), setterTypes.constData());

        if (@available(iOS 17.0, *)) {
            class_replaceMethod(cls, @selector(inlinePredictionType),
                                reinterpret_cast<IMP>(ragoInlinePredictionType), getterTypes.constData());
            class_replaceMethod(cls, @selector(setInlinePredictionType:),
                                reinterpret_cast<IMP>(ragoSetInlinePredictionType), setterTypes.constData());
        }
    }

    bool iosHardwareKeyboardPresent()
    {
#if TARGET_OS_SIMULATOR
        // Xcode Simulator bridges the host Mac keyboard as a GCKeyboard on first key press
        // ("Connect Hardware Keyboard"), even though the simulated iPhone/iPad has no physical
        // mobile keyboard attached. Keep Simulator in pure touch mode unless explicitly enabled.
        if (RagoShell::productEnvInt("SIM_HW_KEYBOARD") <= 0)
            return false;
#endif
        if (@available(iOS 14.0, *))
            return GCKeyboard.coalescedKeyboard != nil;
        return false;
    }

    void iosObserveHardwareKeyboard(QObject *target, const char *slot)
    {
        if (@available(iOS 14.0, *)) {
            // The observers live as long as the app; `target` is the PlatformInfo singleton, which does too.
            QPointer<QObject> guarded(target);
            const QByteArray slotName(slot);
            void (^changed)(NSNotification *) = ^(NSNotification *) {
                if (guarded)
                    QMetaObject::invokeMethod(guarded.data(), slotName.constData(), Qt::QueuedConnection);
            };
            NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
            [center addObserverForName:GCKeyboardDidConnectNotification object:nil queue:nil usingBlock:changed];
            [center addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:nil usingBlock:changed];
        }
    }

    void iosObserveMemoryWarning(QObject *target, const char *slot)
    {
        static bool registered = false;
        if (registered)
            return;
        registered = true;

        QPointer<QObject> guarded(target);
        const QByteArray slotName(slot);
        auto notifyTarget = ^{
            if (guarded)
                QMetaObject::invokeMethod(guarded.data(), slotName.constData(), Qt::QueuedConnection);
        };

        // 1. Observe UIKit application-level memory warning (OS Jetsam / Xcode Debug -> Simulate Memory Warning)
        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
        [center addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *) {
                            notifyTarget();
                        }];

        // 2. Observe Darwin low-memory notifications (CLI `simctl spawn ... notifyutil -p org.apple.memory-warning`)
        static int tokenOrg = 0;
        notify_register_dispatch("org.apple.memory-warning", &tokenOrg, dispatch_get_main_queue(), ^(int) {
            notifyTarget();
        });

        static int tokenSb = 0;
        notify_register_dispatch("com.apple.springboard.memorywarning", &tokenSb, dispatch_get_main_queue(), ^(int) {
            notifyTarget();
        });
    }

    bool iosShareFile(const QString &filePath)
    {
        if (filePath.isEmpty())
            return false;

        NSString *nsPath = filePath.toNSString();
        if (![[NSFileManager defaultManager] fileExistsAtPath:nsPath])
            return false;

        NSURL *fileUrl = [NSURL fileURLWithPath:nsPath];
        if (!fileUrl)
            return false;

        dispatch_async(dispatch_get_main_queue(), ^{
            UIWindow *activeWindow = nil;
            for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
                if (![scene isKindOfClass:[UIWindowScene class]])
                    continue;
                UIWindowScene *windowScene = (UIWindowScene *)scene;
                if (windowScene.activationState != UISceneActivationStateForegroundActive
                    && windowScene.activationState != UISceneActivationStateForegroundInactive)
                    continue;
                for (UIWindow *win in windowScene.windows) {
                    if (win.isKeyWindow) {
                        activeWindow = win;
                        break;
                    }
                }
                if (!activeWindow && windowScene.windows.count > 0)
                    activeWindow = windowScene.windows.firstObject;
                if (activeWindow)
                    break;
            }

            UIViewController *presenter = activeWindow.rootViewController;
            while (presenter.presentedViewController && !presenter.presentedViewController.isBeingDismissed)
                presenter = presenter.presentedViewController;
            if (!presenter || !presenter.view)
                return;

            UIActivityViewController *vc =
                [[UIActivityViewController alloc] initWithActivityItems:@[fileUrl]
                                                  applicationActivities:nil];
            UIPopoverPresentationController *pop = vc.popoverPresentationController;
            if (pop) {
                pop.sourceView = presenter.view;
                pop.sourceRect = CGRectMake(CGRectGetMidX(presenter.view.bounds),
                                            CGRectGetMidY(presenter.view.bounds), 1.0, 1.0);
                pop.permittedArrowDirections = 0;
            }
            [presenter presentViewController:vc animated:YES completion:nil];
        });
        return true;
    }
}
