#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QStringList>

namespace RagoShell
{
    /**
     * @brief The work-area tab strip: an optional Welcome tab plus shell and
     *        server-monitor tabs in the order they were opened.
     *
     * Rows are inserted, removed and moved individually, so QML never
     * rebuilds the other tab buttons. The model owns the current tab; when
     * the current tab closes, its right neighbour (else the left one)
     * becomes current. Each tab keeps the palette colour it got when it
     * opened.
     */
    class TabModel : public QAbstractListModel
    {
        Q_OBJECT
        Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
        Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged FINAL)
        /// Shell / monitor controller of the current tab (null for Welcome).
        Q_PROPERTY(QObject *currentObject READ currentObject NOTIFY currentChanged FINAL)
        /// "welcome", "shell", "monitor" or "" when no tab is open.
        Q_PROPERTY(QString currentKind READ currentKind NOTIFY currentChanged FINAL)
        Q_PROPERTY(QString currentColor READ currentColor NOTIFY currentChanged FINAL)
        Q_PROPERTY(bool welcomeOpen READ welcomeOpen WRITE setWelcomeOpen NOTIFY welcomeOpenChanged FINAL)

    public:
        enum Roles {
            KindRole = Qt::UserRole + 1,
            ObjectRole,
            ColorRole,
            TitleRole
        };

        explicit TabModel(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        int count() const { return static_cast<int>(_tabs.size()); }
        int currentIndex() const { return _current; }
        void setCurrentIndex(int index);
        QObject *currentObject() const { return objectAt(_current); }
        QString currentKind() const { return kindAt(_current); }
        QString currentColor() const;
        bool welcomeOpen() const;
        /// Opening inserts the Welcome tab first (it does not become current).
        void setWelcomeOpen(bool open);

        Q_INVOKABLE QString kindAt(int row) const;
        Q_INVOKABLE QObject *objectAt(int row) const;
        Q_INVOKABLE int indexOf(QObject *object) const;
        /// Moves a tab; the current tab stays current.
        Q_INVOKABLE void move(int from, int to);

        /// Appends a shell / monitor tab.
        void addTab(const QString &kind, QObject *object, bool makeCurrent);
        /// Removes the tab of a closing controller (no-op when unknown).
        void removeObject(QObject *object);

        /// Refreshes the Welcome tab title after a language switch.
        void retranslate();

        /// Dev/CI helper (--open-view tabs): Welcome plus placeholder tabs.
        Q_INVOKABLE void showDebugTabs(const QStringList &titles);

    Q_SIGNALS:
        void countChanged();
        void currentChanged();
        void welcomeOpenChanged();

    private:
        struct Tab
        {
            QString kind;
            QPointer<QObject> object;
            QString color;
            QString title;   ///< Welcome / placeholder tabs; controllers have their own
        };

        QString nextColor();
        void insertTab(int row, const Tab &tab);
        void removeRow(int row);

        QList<Tab> _tabs;
        int _current = -1;
        int _nextColorIndex = 0;
    };
}
