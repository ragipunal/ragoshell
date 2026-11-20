#pragma once

#include <map>
#include <vector>

#include <QChar>
#include <QSet>
#include <QString>

#include "ragoshell/core/utils/FieldCompletion.h"
#include "ragoshell/core/utils/SchemaAnalysis.h"
#include "ragoshell/core/utils/ShellCompletion.h"

namespace RagoShell::ShellCompletion::detail
{
    inline constexpr int kMaxScan = 8000;
    inline constexpr double kRarePresence = 0.05;

    enum class ObjectRole
    {
        None,
        JsBlock,
        Filter,
        FilterList,
        FieldCondition,
        Fields,
        DocList,
        Update,
        FindAndModifySpec,
        BulkWriteList,
        BulkWriteOp,
        BulkWriteSpec,
        Pipeline,
        Stage,
        LookupBody,
        FacetBody,
        GroupBody,
        GroupFieldVal,
        AggExpression,
        AggExprList,
    };

    struct ChainState
    {
        Receiver receiver = Receiver::None;
        QString collection;
        bool inActiveDb = false;
    };

    struct Frame
    {
        QChar open;
        int pos = 0;
        bool isJsBlock = false;
        ObjectRole role = ObjectRole::None;
        bool inAggregate = false;

        // '(' frames
        QString callName;
        Receiver receiver = Receiver::None;
        QString collection;
        int arg = 0;

        // '{' frames
        bool keyPos = true;
        QString keyToken;
        QString lastKey;
        std::map<QString, QString> stringProps;
    };

    struct ChainStep
    {
        enum Kind { Prop, BracketProp, Call } kind = Prop;
        QString name;
        QString rawArgs;
    };

    struct PendingAssignment
    {
        QString varName;
        int rhsStart = 0;
    };

    struct EntryDef
    {
        const char *label;
        const char *insert;
        const char *detail;
    };

    // Trivia & string helpers
    bool isIdentStart(QChar c);
    bool isIdentChar(QChar c);
    void skipTriviaBack(const QString &t, const std::vector<bool> &skipMask, int &p);
    void skipTriviaForward(const QString &t, const std::vector<bool> &skipMask, int &p);
    QString identifierBack(const QString &t, int &p);
    QString identifierForward(const QString &t, int &p);
    QString unescapeJsString(const QString &raw);
    QString unquoteSingleArg(const QString &raw);
    QString escapeForQuote(const QString &s, QChar quoteChar);

    // Keyword & method name sets
    const QSet<QString> &dbMethodNameSet();
    const QSet<QString> &collectionMethodNameSet();
    const QSet<QString> &chainableQueryCursorMethods();
    const QSet<QString> &jsReservedKeywords();

    // Completion catalogs
    const std::vector<EntryDef> &dbMemberEntries();
    const std::vector<EntryDef> &systemNamespaceEntries();
    const std::vector<EntryDef> &mongoMemberEntries();
    const std::vector<EntryDef> &collectionMemberEntries();
    const std::vector<EntryDef> &queryCursorEntries();
    const std::vector<EntryDef> &aggCursorEntries();
    const std::vector<EntryDef> &explainableEntries();
    const std::vector<EntryDef> &replicaSetEntries();
    const std::vector<EntryDef> &shardEntries();
    const std::vector<EntryDef> &ejsonEntries();
    const std::vector<EntryDef> &consoleEntries();
    const std::vector<EntryDef> &statementEntries();
    const std::vector<EntryDef> &expressionEntries();

    const std::vector<const char *> &pipelineStageOperators();
    const std::vector<const char *> &updateOperators();
    const std::vector<const char *> &fieldConditionOperators();
    const std::vector<const char *> &topFilterOperators();
    const std::vector<const char *> &groupAccumulatorOperators();
    const std::vector<const char *> &aggExpressionOperators();

    // Candidate builders
    QString schemaDetailText(const SchemaField &f);
    void appendFilteredEntries(const std::vector<EntryDef> &defs,
                               const QString &prefix,
                               std::vector<Item> &out);
    void appendOperators(const std::vector<const char *> &ops,
                         const QString &prefix,
                         const char *detail,
                         bool quotedKey,
                         std::vector<Item> &out);
    void appendSchemaFields(const std::vector<SchemaField> &fields,
                            const QString &prefix,
                            FieldCompletion::Kind kind,
                            std::vector<Item> &out);

    // Chain & MQL role evaluation
    bool collectChainStepsBack(const QString &t,
                               const std::vector<bool> &skipMask,
                               const std::map<int, int> &closeToOpen,
                               int endPos,
                               std::vector<ChainStep> &outSteps);
    ChainState runChainSteps(const std::vector<ChainStep> &steps,
                             const std::map<QString, ChainState> &vars);
    ChainState evaluateChainEndingAt(const QString &t,
                                     const std::vector<bool> &skipMask,
                                     const std::map<int, int> &closeToOpen,
                                     const std::map<QString, ChainState> &vars,
                                     int endPos);
    int scanCompletedRhsChainEnd(const QString &t,
                                 const std::vector<bool> &skipMask,
                                 const std::map<int, int> &openToClose,
                                 int rhsStart);
    ObjectRole argumentRole(Receiver receiver, const QString &method, int arg, QChar open);
    ObjectRole valueRole(ObjectRole parent, bool inAggregate, const QString &key, QChar open);
    ObjectRole elementRole(ObjectRole parent, QChar open);
    bool isJsBlockBrace(const Frame *top, QChar lastSig, bool afterArrow, const QString &prevWord);
}
