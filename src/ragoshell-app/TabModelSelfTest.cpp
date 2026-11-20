#include "ragoshell-app/TabModelSelfTest.h"

#include <QDebug>

#include <QObject>
#include <QSet>

#include "ragoshell-app/TabModel.h"

namespace
{
    int failures = 0;

    void check(const char *name, bool ok, const QString &detail = QString())
    {
        if (!ok)
            ++failures;
        qInfo("%s  %s%s", ok ? "PASS" : "FAIL", name,
              ok || detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -- ") + detail));
    }

    QString colorOf(RagoShell::TabModel &tabs, QObject *object)
    {
        return tabs.data(tabs.index(tabs.indexOf(object)), RagoShell::TabModel::ColorRole).toString();
    }
}

int RagoShell::runTabModelSelfTest()
{
    TabModel tabs;
    int resets = 0, currentSignals = 0;
    QObject::connect(&tabs, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(&tabs, &TabModel::currentChanged, [&currentSignals]() { ++currentSignals; });

    check("starts with the Welcome tab current", tabs.count() == 1 && tabs.currentIndex() == 0
                                                     && tabs.currentKind() == QStringLiteral("welcome")
                                                     && tabs.welcomeOpen() && !tabs.currentObject());

    QObject a, b, c, m;
    a.setProperty("title", QStringLiteral("users"));
    tabs.addTab(QStringLiteral("shell"), &a, true);
    tabs.addTab(QStringLiteral("shell"), &b, true);
    tabs.addTab(QStringLiteral("monitor"), &m, true);
    check("opened tabs append in order", tabs.objectAt(1) == &a && tabs.objectAt(2) == &b
                                             && tabs.objectAt(3) == &m && tabs.count() == 4);
    check("a newly opened tab becomes current", tabs.currentObject() == &m
                                                    && tabs.currentKind() == QStringLiteral("monitor"));
    check("title role falls back to the controller",
          tabs.data(tabs.index(1), TabModel::TitleRole).toString() == QStringLiteral("users"));
    const QString colorA = colorOf(tabs, &a), colorB = colorOf(tabs, &b), colorM = colorOf(tabs, &m);
    check("each tab gets its own colour", !colorA.isEmpty() && colorA != colorB && colorB != colorM
                                              && colorA != colorM);
    tabs.addTab(QStringLiteral("shell"), &a, true);
    check("the same controller is not added twice", tabs.count() == 4);

    // Closing a tab left of the current one keeps the current tab.
    tabs.removeObject(&b);
    check("closing a tab before the current keeps it", tabs.currentObject() == &m && tabs.currentIndex() == 2);
    check("other tabs keep their colours", colorOf(tabs, &a) == colorA && colorOf(tabs, &m) == colorM);

    // Closing the current tab selects its right neighbour, else the left one.
    tabs.addTab(QStringLiteral("shell"), &c, false);
    check("opening in the background keeps the current tab", tabs.currentObject() == &m);
    tabs.setCurrentIndex(tabs.indexOf(&a));
    tabs.removeObject(&a);
    check("closing the current tab selects its right neighbour", tabs.currentObject() == &m);
    tabs.setCurrentIndex(tabs.indexOf(&c));
    tabs.removeObject(&c);
    check("closing the last tab selects its left neighbour", tabs.currentObject() == &m);

    // Welcome tab: closing and reopening keeps the current tab.
    tabs.setWelcomeOpen(false);
    check("closing Welcome keeps the current tab", !tabs.welcomeOpen() && tabs.currentObject() == &m
                                                       && tabs.currentIndex() == 0);
    tabs.setWelcomeOpen(true);
    check("reopened Welcome is the first tab", tabs.kindAt(0) == QStringLiteral("welcome")
                                                   && tabs.currentObject() == &m && tabs.currentIndex() == 1);

    // Moving tabs keeps the current one.
    tabs.addTab(QStringLiteral("shell"), &b, false);   // Welcome, m, b
    tabs.move(1, 2);                                    // Welcome, b, m
    check("move reorders tabs", tabs.objectAt(1) == &b && tabs.objectAt(2) == &m);
    check("move keeps the current tab", tabs.currentObject() == &m && tabs.currentIndex() == 2);

    // Colours freed by closed tabs are reused before repeating.
    QSet<QString> colours;
    QList<QObject *> many;
    for (int i = 0; i < 9; ++i) {
        many.append(new QObject(&tabs));
        tabs.addTab(QStringLiteral("shell"), many.last(), false);
    }
    for (int row = 0; row < tabs.count(); ++row)
        colours.insert(tabs.data(tabs.index(row), TabModel::ColorRole).toString());
    check("up to twelve tabs never share a colour", colours.size() == tabs.count(),
          QStringLiteral("%1 colours for %2 tabs").arg(colours.size()).arg(tabs.count()));

    // Closing everything leaves no current tab.
    for (QObject *o : many)
        tabs.removeObject(o);
    tabs.removeObject(&b);
    tabs.removeObject(&m);
    tabs.setWelcomeOpen(false);
    check("no tabs means no current tab", tabs.count() == 0 && tabs.currentIndex() == -1
                                              && tabs.currentKind().isEmpty() && !tabs.currentObject());
    tabs.setCurrentIndex(3);
    check("current index stays -1 without tabs", tabs.currentIndex() == -1);
    check("only row inserts / removes, never a model reset", resets == 0, QString::number(resets));
    check("current changes are signalled", currentSignals > 0);

    qInfo("%s", failures ? qPrintable(QStringLiteral("%1 check(s) failed").arg(failures))
                         : "all checks passed");
    return failures;
}
