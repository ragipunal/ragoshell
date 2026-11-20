#include "ragoshell-app/TabModel.h"

#include <algorithm>

#include <QSet>

namespace RagoShell
{
    namespace
    {
        const QString kWelcome = QStringLiteral("welcome");
        const QString kWelcomeColor = QStringLiteral("#334155");

        // Ordered for maximum hue and luminance contrast between neighbours:
        // deep tones (white text) alternate with bright ones (dark text).
        const QStringList kPalette = {
            QStringLiteral("#1d4ed8"),  // royal blue
            QStringLiteral("#047857"),  // deep emerald
            QStringLiteral("#fbbf24"),  // golden amber
            QStringLiteral("#6d28d9"),  // royal violet
            QStringLiteral("#0e7490"),  // ocean cyan
            QStringLiteral("#be123c"),  // crimson rose
            QStringLiteral("#a3e635"),  // fresh lime
            QStringLiteral("#4338ca"),  // deep indigo
            QStringLiteral("#c2410c"),  // burnt orange
            QStringLiteral("#38bdf8"),  // sky cyan
            QStringLiteral("#0f766e"),  // deep jade
            QStringLiteral("#a21caf"),  // rich magenta
        };
    }

    TabModel::TabModel(QObject *parent)
        : QAbstractListModel(parent)
    {
        _tabs.append({kWelcome, nullptr, kWelcomeColor, QString()});
        _current = 0;
    }

    int TabModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : count();
    }

    QVariant TabModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() >= count())
            return QVariant();
        const Tab &tab = _tabs.at(index.row());
        switch (role) {
        case KindRole:   return tab.kind;
        case ObjectRole: return QVariant::fromValue<QObject *>(tab.object.data());
        case ColorRole:  return tab.color;
        case TitleRole:
            // Looked up on every read, so a language switch only needs a dataChanged.
            if (tab.kind == kWelcome)
                return tr("Welcome");
            if (!tab.title.isEmpty() || !tab.object)
                return tab.title;
            return tab.object->property("title").toString();
        default:         return QVariant();
        }
    }

    QHash<int, QByteArray> TabModel::roleNames() const
    {
        return {
            {KindRole, "kind"},
            {ObjectRole, "tabObject"},
            {ColorRole, "tabColor"},
            {TitleRole, "title"},
        };
    }

    void TabModel::setCurrentIndex(int index)
    {
        index = count() == 0 ? -1 : std::clamp(index, 0, count() - 1);
        if (_current == index)
            return;
        _current = index;
        emit currentChanged();
    }

    QString TabModel::currentColor() const
    {
        return _current >= 0 && _current < count() ? _tabs.at(_current).color : QString();
    }

    void TabModel::retranslate()
    {
        for (int row = 0; row < count(); ++row) {
            if (_tabs.at(row).kind == kWelcome) {
                const QModelIndex idx = index(row);
                emit dataChanged(idx, idx, {TitleRole});
            }
        }
    }

    bool TabModel::welcomeOpen() const
    {
        return std::any_of(_tabs.cbegin(), _tabs.cend(),
                           [](const Tab &tab) { return tab.kind == kWelcome; });
    }

    void TabModel::setWelcomeOpen(bool open)
    {
        if (welcomeOpen() == open)
            return;
        if (open) {
            insertTab(0, {kWelcome, nullptr, kWelcomeColor, QString()});
        } else {
            for (int row = count() - 1; row >= 0; --row) {
                if (_tabs.at(row).kind == kWelcome)
                    removeRow(row);
            }
        }
        emit welcomeOpenChanged();
    }

    QString TabModel::kindAt(int row) const
    {
        return row >= 0 && row < count() ? _tabs.at(row).kind : QString();
    }

    QObject *TabModel::objectAt(int row) const
    {
        return row >= 0 && row < count() ? _tabs.at(row).object.data() : nullptr;
    }

    int TabModel::indexOf(QObject *object) const
    {
        if (!object)
            return -1;
        for (int row = 0; row < count(); ++row) {
            if (_tabs.at(row).object == object)
                return row;
        }
        return -1;
    }

    void TabModel::move(int from, int to)
    {
        if (from < 0 || from >= count() || to < 0 || to >= count() || from == to)
            return;
        // beginMoveRows wants the destination as an insertion point before the move.
        if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to))
            return;
        _tabs.move(from, to);
        endMoveRows();
        const int oldCurrent = _current;
        if (_current == from)
            _current = to;
        else if (from < _current && _current <= to)
            --_current;
        else if (to <= _current && _current < from)
            ++_current;
        if (_current != oldCurrent)
            emit currentChanged();
    }

    void TabModel::addTab(const QString &kind, QObject *object, bool makeCurrent)
    {
        if (!object || indexOf(object) >= 0)
            return;
        insertTab(count(), {kind, object, nextColor(), QString()});
        if (makeCurrent || _current < 0)
            setCurrentIndex(count() - 1);
    }

    void TabModel::removeObject(QObject *object)
    {
        const int row = indexOf(object);
        if (row >= 0)
            removeRow(row);
    }

    void TabModel::showDebugTabs(const QStringList &titles)
    {
        beginResetModel();
        _tabs.clear();
        _tabs.append({kWelcome, nullptr, kWelcomeColor, QString()});
        _nextColorIndex = 0;
        for (const QString &title : titles)
            _tabs.append({QStringLiteral("debug"), nullptr, nextColor(), title});
        endResetModel();
        _current = std::min(2, count() - 1);
        emit countChanged();
        emit currentChanged();
        emit welcomeOpenChanged();
    }

    QString TabModel::nextColor()
    {
        QSet<QString> used;
        for (const Tab &tab : _tabs)
            used.insert(tab.color);
        const int n = static_cast<int>(kPalette.size());
        for (int step = 0; step < n; ++step) {
            const QString candidate = kPalette.at((_nextColorIndex + step) % n);
            if (!used.contains(candidate)) {
                _nextColorIndex = (_nextColorIndex + step + 1) % n;
                return candidate;
            }
        }
        // More tabs than colours: reuse them in order.
        const QString color = kPalette.at(_nextColorIndex % n);
        _nextColorIndex = (_nextColorIndex + 1) % n;
        return color;
    }

    void TabModel::insertTab(int row, const Tab &tab)
    {
        beginInsertRows(QModelIndex(), row, row);
        _tabs.insert(row, tab);
        endInsertRows();
        emit countChanged();
        if (_current >= row) {
            ++_current;
            emit currentChanged();
        }
    }

    void TabModel::removeRow(int row)
    {
        const int oldCurrent = _current;
        beginRemoveRows(QModelIndex(), row, row);
        _tabs.removeAt(row);
        endRemoveRows();
        emit countChanged();
        if (count() == 0)
            _current = -1;
        else if (oldCurrent > row)
            _current = oldCurrent - 1;
        else if (oldCurrent == row)
            _current = std::min(row, count() - 1);   // right neighbour, else the left one
        // Also when the index stayed the same: it now points at another tab.
        if (oldCurrent >= row)
            emit currentChanged();
    }
}
