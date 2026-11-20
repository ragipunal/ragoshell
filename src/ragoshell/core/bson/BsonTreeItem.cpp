#include "ragoshell/core/bson/BsonTreeItem.h"
#include <bsoncxx/builder/basic/document.hpp>
namespace
{
    struct removeIfFound
    {
        removeIfFound(RagoShell::BsonTreeItem *item) :_whatSearch(item) {}
        bool operator()(const RagoShell::BsonTreeItem* item) const
        {
            if (item == _whatSearch) {
                delete _whatSearch;
                return true;
            }
            return false;
        }
        const RagoShell::BsonTreeItem *const _whatSearch;
    };

    const RagoShell::BsonTreeItem *findSuperRoot(const RagoShell::BsonTreeItem *const item)
    {
        RagoShell::BsonTreeItem *parent = qobject_cast<RagoShell::BsonTreeItem *>(item->parent());
        if (parent) {
            RagoShell::BsonTreeItem *grParent = qobject_cast<RagoShell::BsonTreeItem *>(parent->parent());
            if (grParent) {
               return findSuperRoot(parent);
            }
        }
        return item;
    }
}
namespace RagoShell
{
    namespace
    {
        bsoncxx::document::view emptyBsonView()
        {
            static const bsoncxx::document::value s_empty = bsoncxx::builder::basic::make_document();
            return s_empty.view();
        }
    }

    BsonTreeItem::BsonTreeItem(QObject *parent)
        : BaseClass(parent),
          _rootView(emptyBsonView())
    {
    }

    BsonTreeItem::BsonTreeItem(bsoncxx::document::view bsonObjRoot, QObject *parent)
        : BaseClass(parent),
          _rootView(bsonObjRoot)
    {
    }

    BsonTreeItem::BsonTreeItem(bsoncxx::document::value &&ownedBsonRoot, QObject *parent)
        : BaseClass(parent),
          _ownedRoot(std::make_unique<const bsoncxx::document::value>(std::move(ownedBsonRoot))),
          _rootView(_ownedRoot->view())
    {
    }

    unsigned BsonTreeItem::childrenCount() const
    {
        return _items.size();
    }

    void BsonTreeItem::clear()
    {
        _items.clear();
    }

    void BsonTreeItem::addChild(BsonTreeItem *item)
    {
        _items.push_back(item);
    }

    BsonTreeItem* BsonTreeItem::child(unsigned pos) const
    {
        return _items[pos];
    }

    BsonTreeItem* BsonTreeItem::childSafe(unsigned pos) const
    {
        if (childrenCount() > pos) {
            return _items[pos];
        }
        else {
            return NULL;
        }
    }

    BsonTreeItem* BsonTreeItem::childByKey(const QString &val)
    {
        for (unsigned i = 0; i < _items.size(); ++i) {
            if (_items[i]->key() == val) {
                return _items[i];
            }
        }
        return NULL;
    }

    const BsonTreeItem *BsonTreeItem::superParent() const
    {
        return findSuperRoot(this);
    }

    bsoncxx::document::view BsonTreeItem::superRoot() const
    {
        return superParent()->root();
    }

    bsoncxx::document::view BsonTreeItem::root() const
    {
        return _rootView;
    }

    int BsonTreeItem::indexOf(BsonTreeItem *item) const
    {
        for (unsigned i = 0; i < _items.size(); ++i) {
            if (item == _items[i]) {
                return i;
            }
        }
        return -1;
    }

    QString BsonTreeItem::key() const
    {
        return _fields._key;
    }

    QString BsonTreeItem::value() const
    {
        return _fields._value;
    }

    bsoncxx::type BsonTreeItem::type() const
    {
        return _fields._type;
    }

    void BsonTreeItem::setKey(const QString &key)
    {
        _fields._key = key;
    }

    void BsonTreeItem::setValue(const QString &value)
    {
        _fields._value = value;
    }

    void BsonTreeItem::setType(bsoncxx::type type)
    {
       _fields._type = type;
    }

    bsoncxx::binary_sub_type BsonTreeItem::binType() const
    {
        return _fields._binType;
    }

    void BsonTreeItem::setBinType(bsoncxx::binary_sub_type type)
    {
        _fields._binType = type;
    }

    void BsonTreeItem::removeChild(BsonTreeItem *item)
    {
        _items.erase(std::remove_if(_items.begin(), _items.end(), removeIfFound(item)), _items.end());
    }
}
