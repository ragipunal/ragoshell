#include "ragoshell/core/bson/BsonTreeModel.h"

#include <QColor>

#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/bson/BsonTreeItem.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace
{
    using namespace RagoShell;

    QString arrayValue(int itemsCount) {
        QString elements = itemsCount == 1 ? "element" : "elements";
        return QString("[ %1 %2 ]").arg(itemsCount).arg(elements);
    }

    QString objectValue(int itemsCount) {
        QString fields = itemsCount == 1 ? "field" : "fields";
        return QString("{ %1 %2 }").arg(itemsCount).arg(fields);
    }

    void parseDocument(BsonTreeItem *root, const bsoncxx::document::view &doc, bool isArray)
    {            
            for (const auto &element : doc)
            {
                BsonTreeItem *childItemInner = new BsonTreeItem(doc, root);
                const auto key = element.key();
                std::string fieldName(key.data(), key.size());
                childItemInner->setFieldName(fieldName);

                QString uiFieldName = QtUtils::toQString(fieldName);
                childItemInner->setKey(uiFieldName);

                if (isArray) {
                    // When we iterate array, show field names in square brackets
                    // In this case field names are numeric, starting from 0.
                    childItemInner->setKey("[" + uiFieldName + "]");
                }

                if (BsonUtils::isArray(element)) {
                    int itemsCount = static_cast<int>(
                        std::distance(element.get_array().value.begin(),
                                      element.get_array().value.end()));
                    childItemInner->setValue(arrayValue(itemsCount));
                }
                else if (BsonUtils::isDocument(element)) {
                    int count = BsonUtils::elementsCount(element.get_document().view());
                    childItemInner->setValue(objectValue(count));
                }
                else {
                    // Cap in-memory BsonTreeItem string copies at 16 KB so multi-MB OCR/HTML
                    // strings in large collections do not duplicate hundreds of MB in RAM.
                    // Full document editing reads directly from MongoDocument::bsonObj().
                    static constexpr size_t kMaxTreeItemStringBytes = 16384;
                    std::string result;
                    BsonUtils::buildJsonString(
                        element, result,
                        AppRegistry::instance().settingsManager()->uuidEncoding(),
                        AppRegistry::instance().settingsManager()->timeZone(),
                        kMaxTreeItemStringBytes);
                    childItemInner->setValue(QtUtils::toQString(result));
                }
                childItemInner->setType(element.type());
                if (element.type() == bsoncxx::type::k_binary) {
                    childItemInner->setBinType(element.get_binary().sub_type);
                }
                root->addChild(childItemInner);
                //root->setValue(QString("{ %1 fields }").arg(root->childrenCount()));
            }            
    }
}

namespace RagoShell
{
    BsonTreeModel::BsonTreeModel(const std::vector<MongoDocumentPtr> &documents, QObject *parent) :
        BaseClass(parent),
        _documents(documents),
        _root(new BsonTreeItem(this))
    {
        for (size_t i = 0; i < _documents.size(); ++i) {
            const MongoDocumentPtr &doc = _documents[i]; 
            BsonTreeItem *child = new BsonTreeItem(doc->bsonObj(), _root);
            parseDocument(child, doc->bsonObj(), doc->isArray());

            QString idValue;
            BsonTreeItem *idItem = child->childByKey("_id");
            if (idItem) {
                idValue = idItem->value();
            }

            child->setKey(QString("(%1) %2").arg(i + 1).arg(BsonUtils::sanitizeForDisplay(idValue, true)));

            int count = BsonUtils::elementsCount(doc->bsonObj());

            if (doc->isArray()) {
                child->setValue(arrayValue(count));
                child->setType(bsoncxx::type::k_array);
            } else {
                child->setValue(objectValue(count));
                child->setType(bsoncxx::type::k_document);
            }
            _root->addChild(child);
        }
    }

    BsonTreeModel::~BsonTreeModel()
    {
        // Destroy child BsonTreeItems before _documents releases the underlying BSON buffers.
        delete _root;
        _root = nullptr;
    }

    void BsonTreeModel::fetchMore(const QModelIndex &parent)
    {
        BsonTreeItem *node = QtUtils::item<BsonTreeItem*>(parent);
        if (node) {
            bsoncxx::document::element elem = BsonUtils::indexOf(node->root(), parent.row());
            if (elem && BsonUtils::isDocument(elem.type())) {
                // Array and document BSON bodies share the same layout;
                // a document::view over the array body reads array elements
                // with their numeric keys.
                const bsoncxx::document::view docView =
                    elem.type() == bsoncxx::type::k_array
                        ? bsoncxx::document::view(elem.get_array().value.data(),
                                                  elem.get_array().value.length())
                        : elem.get_document().view();
                parseDocument(node, docView, elem.type() == bsoncxx::type::k_array);
            }            
        }
        return BaseClass::fetchMore(parent);
    }

    bool BsonTreeModel::canFetchMore(const QModelIndex &parent) const
    {
        BsonTreeItem *node = QtUtils::item<BsonTreeItem*>(parent);
        if (node && !node->childrenCount()) {
            return BsonUtils::isDocument(node->type());
        }
        return false;
    }

    bool BsonTreeModel::hasChildren(const QModelIndex &parent) const
    {
        BsonTreeItem *node = QtUtils::item<BsonTreeItem*>(parent);
        if (node) {
            return BsonUtils::isDocument(node->type());
        }
        return true;
    }

    QString BsonTreeModel::bsonTypeName(BsonTreeItem *item)
    {
        return BsonUtils::BSONTypeToString(
            item->type(), item->binType(),
            AppRegistry::instance().settingsManager()->uuidEncoding());
    }

    QString BsonTreeModel::buildFieldPath(const BsonTreeItem *item)
    {
        if (!item)
            return QString();
        QStringList parts;
        const BsonTreeItem *cur = item;
        while (cur) {
            const BsonTreeItem *p = qobject_cast<const BsonTreeItem *>(cur->parent());
            // Stop when parent is _root (top-level document rows have empty fieldName)
            if (!p || !p->parent())
                break;
            if (!cur->fieldName().empty())
                parts.prepend(QtUtils::toQString(cur->fieldName()));
            cur = p;
        }
        return parts.join(QLatin1Char('.'));
    }

    bool BsonTreeModel::isEditableScalarItem(const BsonTreeItem *item)
    {
        if (!item)
            return false;
        const BsonTreeItem *p = qobject_cast<const BsonTreeItem *>(item->parent());
        if (!p || !p->parent())
            return false; // top-level document row itself
        if (item->fieldName() == "_id" && (!p->parent()->parent()))
            return false; // top-level _id is immutable
        // Require that the owning top-level document actually contains an '_id' field
        // (prevents in-place editing on explain(), stats(), aggregate(), or {_id: 0} projections).
        const BsonTreeItem *top = item->superParent();
        if (!top || !const_cast<BsonTreeItem *>(top)->childByKey("_id"))
            return false;
        // Keys containing '.' cannot be addressed unambiguously via dotted fieldPath.
        for (const BsonTreeItem *cur = item; cur; cur = qobject_cast<const BsonTreeItem *>(cur->parent())) {
            const BsonTreeItem *par = qobject_cast<const BsonTreeItem *>(cur->parent());
            if (!par || !par->parent())
                break;
            if (cur->fieldName().find('.') != std::string::npos)
                return false;
        }
        switch (item->type()) {
        case bsoncxx::type::k_string: {
            // Multi-KB strings truncated at kMaxTreeItemStringBytes and multi-line strings
            // must be edited via full Document Editor rather than single-line inline cell edit.
            if (item->value().contains(QLatin1Char('\n')) || item->value().contains(QLatin1Char('\r')))
                return false;
            if (!item->fieldName().empty()) {
                const auto el = item->root()[item->fieldName()];
                if (el && el.type() == bsoncxx::type::k_string &&
                    el.get_string().value.size() > 16384) {
                    return false;
                }
            }
            return true;
        }
        case bsoncxx::type::k_int32:
        case bsoncxx::type::k_int64:
        case bsoncxx::type::k_double:
        case bsoncxx::type::k_bool:
        case bsoncxx::type::k_null:
            return true;
        default:
            return false;
        }
    }

    int BsonTreeModel::documentIndexForItem(const BsonTreeItem *item) const
    {
        if (!item || !_root)
            return -1;
        const BsonTreeItem *top = item->superParent();
        return _root->indexOf(const_cast<BsonTreeItem *>(top));
    }

    QHash<int, QByteArray> BsonTreeModel::roleNames() const
    {
        QHash<int, QByteArray> roles = BaseClass::roleNames();
        roles[BsonKeyRole] = "key";
        roles[BsonValueRole] = "value";
        roles[BsonRawValueRole] = "rawValue";
        roles[BsonTypeNameRole] = "typeName";
        roles[BsonHasChildrenRole] = "expandable";
        roles[BsonFieldPathRole] = "fieldPath";
        roles[BsonDocIndexRole] = "docIndex";
        roles[BsonEditableScalarRole] = "editableScalar";
        return roles;
    }

    QVariant BsonTreeModel::data(const QModelIndex &index, int role) const
    {
        QVariant result;

        if (!index.isValid())
            return result;

        BsonTreeItem *node = QtUtils::item<BsonTreeItem*>(index);

        if (!node)
            return result;  

        int col = index.column();

        const bool isCut = node->type() == bsoncxx::type::k_string ||
                           node->type() == bsoncxx::type::k_code ||
                           node->type() == bsoncxx::type::k_codewscope;

        // Column-free roles used by QML delegates.
        // Slice long strings BEFORE sanitizeForDisplay so scrolling never scans multi-KB values.
        if (role == BsonKeyRole)            return BsonUtils::sanitizeForDisplay(node->key().left(512), true);
        if (role == BsonValueRole) {
            const QString rawSlice = isCut ? node->value().left(600) : node->value();
            const QString clean = BsonUtils::sanitizeForDisplay(rawSlice, true);
            return isCut ? clean.left(300) : clean;
        }
        if (role == BsonRawValueRole)       return node->value();
        if (role == BsonTypeNameRole)       return bsonTypeName(node);
        if (role == BsonHasChildrenRole)    return BsonUtils::isDocument(node->type());
        if (role == BsonFieldPathRole)      return buildFieldPath(node);
        if (role == BsonDocIndexRole)       return documentIndexForItem(node);
        if (role == BsonEditableScalarRole) return isEditableScalarItem(node);

        if (role == Qt::ForegroundRole && col == BsonTreeItem::eType) {
            return QColor(Qt::gray);
        }

        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
            if (col == BsonTreeItem::eKey) {
                if (role == Qt::DisplayRole) {
                    result = BsonUtils::sanitizeForDisplay(node->key().left(512), true);
                }
            }
            else if (col == BsonTreeItem::eValue) {
                if (role == Qt::ToolTipRole) {
                    const QString rawSlice = isCut ? node->value().left(1000) : node->value();
                    const QString clean = BsonUtils::sanitizeForDisplay(rawSlice, false);
                    result = isCut ? clean.left(500) : clean;
                }
                else {
                    const QString rawSlice = isCut ? node->value().left(600) : node->value();
                    const QString clean = BsonUtils::sanitizeForDisplay(rawSlice, true);
                    result = isCut ? clean.left(300) : clean;
                }
            }
            else if (col == BsonTreeItem::eType) {
                result = BsonUtils::BSONTypeToString(node->type(), node->binType(), AppRegistry::instance().settingsManager()->uuidEncoding());
            }
        }       

        return result;
    }

    Qt::ItemFlags BsonTreeModel::flags(const QModelIndex &index) const
    {
        Qt::ItemFlags result{};
        if (index.isValid()) {
            result = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        }
        return result;
    }

    int BsonTreeModel::rowCount(const QModelIndex &parent) const
    {
        const BsonTreeItem *parentItem = nullptr;
        if (parent.isValid())
            parentItem = QtUtils::item<BsonTreeItem*>(parent);
        else
            parentItem = _root;

        return parentItem ? static_cast<int>(parentItem->childrenCount()) : 0;
    }

    int BsonTreeModel::columnCount(const QModelIndex &parent) const
    {
        return BsonTreeItem::eCountColumns;
    }

    QVariant BsonTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (role != Qt::DisplayRole)
            return QVariant();

        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            if (section == BsonTreeItem::eKey) {
                return "Key";
            }
            else if (section == BsonTreeItem::eValue) {
                return "Value";
            }
            else {
                return "Type";
            }
        }

        return BaseClass::headerData(section, orientation, role);
    }

    QModelIndex BsonTreeModel::parent(const QModelIndex& index) const
    {
        QModelIndex result;
        if (index.isValid()) {
            BsonTreeItem *const childItem = QtUtils::item<BsonTreeItem*const>(index);
            BsonTreeItem *const parentItem = static_cast<BsonTreeItem*const>(childItem->parent());
            if (parentItem && parentItem != _root) {
                BsonTreeItem *const grandParent = static_cast<BsonTreeItem*const>(parentItem->parent());
                int row = grandParent->indexOf(parentItem);
                result = createIndex(row, 0, parentItem);
            }
        }
        return result;
    }

    QModelIndex BsonTreeModel::index(int row, int column, const QModelIndex &parent) const
    {
        QModelIndex index;
        if (hasIndex(row, column, parent)) {
            const BsonTreeItem * parentItem = NULL;
            if (!parent.isValid()) {
                parentItem = _root;
            } else {
                parentItem = QtUtils::item<BsonTreeItem*>(parent);
            }

            BsonTreeItem *childItem = parentItem->child(row);
            if (childItem) {
                index = createIndex(row, column, childItem);
            }
        }
        return index;
    }

    void BsonTreeModel::insertItem(BsonTreeItem *parent, BsonTreeItem *children)
    {
        QModelIndex index = createIndex(0, 0, parent);
        unsigned child_count = parent->childrenCount();
        beginInsertRows(index, child_count, child_count);
        parent->addChild(children);
        endInsertRows();
    }

    void BsonTreeModel::removeitem(BsonTreeItem *children)
    {
        BsonTreeItem *parent = static_cast<BsonTreeItem *>(children->parent());
        if (parent) {
            QModelIndex index = createIndex(0, 0, parent);
            int row = parent->indexOf(children);
            beginRemoveRows(index, row, row);
            parent->removeChild(children);
            endRemoveRows();
        }
    }
}
