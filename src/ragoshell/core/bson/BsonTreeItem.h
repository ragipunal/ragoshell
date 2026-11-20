#pragma once

#include <memory>
#include <vector>
#include <QObject>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>

namespace RagoShell
{
    /**
     * @brief BSON tree item (represents array or object)
     */
    struct BsonItemFields
    {
        QString _key;
        QString _value;
        bsoncxx::type _type = bsoncxx::type::k_null;
        bsoncxx::binary_sub_type _binType = bsoncxx::binary_sub_type::k_binary;
    };

    class BsonTreeItem : public QObject
    {
        Q_OBJECT
    public:
        enum eColumn
        {
            eKey = 0,
            eValue = 1,
            eType = 2,
            eCountColumns = 3
        };

        typedef QObject BaseClass;
        typedef std::vector<BsonTreeItem*> ChildContainerType;

        explicit BsonTreeItem(QObject *parent = nullptr);
        explicit BsonTreeItem(bsoncxx::document::view bsonObjRoot, QObject *parent = nullptr);
        explicit BsonTreeItem(bsoncxx::document::value &&ownedBsonRoot, QObject *parent = nullptr);

        unsigned childrenCount() const;
        void clear();
        void addChild(BsonTreeItem *item);
        void removeChild(BsonTreeItem *item);
        BsonTreeItem* child(unsigned pos) const;
        BsonTreeItem* childSafe(unsigned pos) const;
        BsonTreeItem* childByKey(const QString &val);
        int indexOf(BsonTreeItem *item) const;

        const BsonTreeItem* superParent() const;
        bsoncxx::document::view root() const;
        bsoncxx::document::view superRoot() const;

        std::string fieldName() const { return _fieldName; };
        void setFieldName(const std::string &fieldName) { _fieldName = fieldName; };

        QString key() const;
        void setKey(const QString &key);

        QString value() const;
        void setValue(const QString &value);

        bsoncxx::type type() const;
        void setType(bsoncxx::type type);

        bsoncxx::binary_sub_type binType() const;
        void setBinType(bsoncxx::binary_sub_type type);

    protected:
        std::unique_ptr<const bsoncxx::document::value> _ownedRoot;
        bsoncxx::document::view _rootView;
        ChildContainerType _items;
        BsonItemFields _fields;
        std::string _fieldName;
    };
}
