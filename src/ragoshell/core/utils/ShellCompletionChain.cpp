#include "ragoshell/core/utils/ShellCompletionInternal.h"

namespace RagoShell::ShellCompletion::detail
{
            // ---- Chain Evaluation ----------------------------------------------

            bool collectChainStepsBack(const QString &t,
                                       const std::vector<bool> &skipMask,
                                       const std::map<int, int> &closeToOpen,
                                       int endPos,
                                       std::vector<ChainStep> &outSteps)
            {
                outSteps.clear();
                int p = endPos;
                for (int guard = 0; guard < 64; ++guard) {
                    skipTriviaBack(t, skipMask, p);
                    if (p < 0)
                        break;

                    if (t[p] == QLatin1Char(')')) {
                        const auto it = closeToOpen.find(p);
                        if (it == closeToOpen.end())
                            return false;
                        const int open = it->second;
                        const QString rawArgs = t.mid(open + 1, p - open - 1);
                        int q = open - 1;
                        skipTriviaBack(t, skipMask, q);
                        const QString ident = identifierBack(t, q);
                        if (ident.isEmpty() || ident[0].isDigit())
                            return false;
                        outSteps.push_back({ChainStep::Call, ident, rawArgs});
                        skipTriviaBack(t, skipMask, q);
                        if (q >= 0 && t[q] == QLatin1Char('.')) {
                            if (q > 0 && t[q - 1] == QLatin1Char('.'))
                                return false;
                            p = (q > 0 && t[q - 1] == QLatin1Char('?')) ? q - 2 : q - 1;
                            continue;
                        }
                        break;
                    }

                    if (t[p] == QLatin1Char(']')) {
                        const auto it = closeToOpen.find(p);
                        if (it == closeToOpen.end())
                            return false;
                        const int open = it->second;
                        const QString prop = unquoteSingleArg(t.mid(open + 1, p - open - 1));
                        if (prop.isEmpty())
                            return false;
                        outSteps.push_back({ChainStep::BracketProp, prop, {}});
                        int q = open - 1;
                        skipTriviaBack(t, skipMask, q);
                        if (q >= 0 && t[q] == QLatin1Char('.')) {
                            p = (q > 0 && t[q - 1] == QLatin1Char('?')) ? q - 2 : q - 1;
                        } else {
                            p = q;
                        }
                        continue;
                    }

                    if (isIdentChar(t[p])) {
                        const QString ident = identifierBack(t, p);
                        if (ident.isEmpty() || ident[0].isDigit())
                            return false;
                        outSteps.push_back({ChainStep::Prop, ident, {}});
                        skipTriviaBack(t, skipMask, p);
                        if (p >= 0 && t[p] == QLatin1Char('.')) {
                            if (p > 0 && t[p - 1] == QLatin1Char('.'))
                                return false;
                            p = (p > 0 && t[p - 1] == QLatin1Char('?')) ? p - 2 : p - 1;
                            continue;
                        }
                        break;
                    }

                    return false;
                }

                if (outSteps.empty())
                    return false;
                std::reverse(outSteps.begin(), outSteps.end());
                return true;
            }

            ChainState runChainSteps(const std::vector<ChainStep> &steps,
                                     const std::map<QString, ChainState> &vars)
            {
                if (steps.empty())
                    return {};

                ChainState state;
                const ChainStep &root = steps.front();
                if (root.kind != ChainStep::Prop)
                    return {};

                if (root.name == QLatin1String("db")) {
                    state = {Receiver::Database, QString(), true};
                } else if (root.name == QLatin1String("rs")) {
                    state = {Receiver::ReplicaSet, QString(), false};
                } else if (root.name == QLatin1String("sh")) {
                    state = {Receiver::Shard, QString(), false};
                } else if (root.name == QLatin1String("EJSON")) {
                    state = {Receiver::Ejson, QString(), false};
                } else if (root.name == QLatin1String("console")) {
                    state = {Receiver::Console, QString(), false};
                } else {
                    const auto it = vars.find(root.name);
                    if (it == vars.end())
                        return {};
                    state = it->second;
                }

                for (size_t i = 1; i < steps.size(); ++i) {
                    const ChainStep &step = steps[i];
                    switch (state.receiver) {
                    case Receiver::Database:
                    case Receiver::SiblingDatabase: {
                        const bool activeDb = (state.receiver == Receiver::Database);
                        if (step.kind == ChainStep::Prop || step.kind == ChainStep::BracketProp) {
                            if (step.name == QLatin1String("system")) {
                                state = {Receiver::SystemNamespace, QString(), activeDb};
                            } else if (step.name.isEmpty() || step.name.startsWith(QLatin1Char('_'))
                                       || dbMethodNameSet().contains(step.name)) {
                                return {};
                            } else {
                                state = {Receiver::Collection, activeDb ? step.name : QString(), activeDb};
                            }
                        } else {
                            if (step.name == QLatin1String("getCollection")) {
                                const QString coll = unquoteSingleArg(step.rawArgs);
                                state = {Receiver::Collection, activeDb ? coll : QString(), activeDb};
                            } else if (step.name == QLatin1String("getSiblingDB")) {
                                state = {Receiver::SiblingDatabase, QString(), false};
                            } else if (step.name == QLatin1String("getMongo")) {
                                state = {Receiver::Mongo, QString(), false};
                            } else {
                                return {};
                            }
                        }
                        break;
                    }
                    case Receiver::SystemNamespace: {
                        if ((step.kind == ChainStep::Prop || step.kind == ChainStep::BracketProp)
                            && !step.name.isEmpty()) {
                            const QString coll = state.inActiveDb
                                ? (QStringLiteral("system.") + step.name)
                                : QString();
                            state = {Receiver::Collection, coll, state.inActiveDb};
                        } else {
                            return {};
                        }
                        break;
                    }
                    case Receiver::Collection: {
                        if (step.kind == ChainStep::Prop || step.kind == ChainStep::BracketProp) {
                            if (step.name.isEmpty() || collectionMethodNameSet().contains(step.name))
                                return {};
                            const QString sub = (state.inActiveDb && !state.collection.isEmpty())
                                ? (state.collection + QLatin1Char('.') + step.name)
                                : QString();
                            state = {Receiver::Collection, sub, state.inActiveDb};
                        } else {
                            if (step.name == QLatin1String("find")) {
                                state = {Receiver::QueryCursor, state.collection, state.inActiveDb};
                            } else if (step.name == QLatin1String("aggregate")) {
                                state = {Receiver::AggCursor, state.collection, state.inActiveDb};
                            } else if (step.name == QLatin1String("explain")) {
                                state = {Receiver::Explainable, state.collection, state.inActiveDb};
                            } else if (step.name == QLatin1String("getDB")) {
                                state = {state.inActiveDb ? Receiver::Database : Receiver::SiblingDatabase,
                                         QString(), state.inActiveDb};
                            } else {
                                return {};
                            }
                        }
                        break;
                    }
                    case Receiver::QueryCursor: {
                        if (step.kind == ChainStep::Call
                            && chainableQueryCursorMethods().contains(step.name)) {
                            // Remains QueryCursor on the same collection.
                        } else {
                            return {};
                        }
                        break;
                    }
                    case Receiver::AggCursor: {
                        if (step.kind == ChainStep::Call && step.name == QLatin1String("pretty")) {
                            // Remains AggCursor on the same collection.
                        } else {
                            return {};
                        }
                        break;
                    }
                    default:
                        return {};
                    }
                }
                return state;
            }

            ChainState evaluateChainEndingAt(const QString &t,
                                             const std::vector<bool> &skipMask,
                                             const std::map<int, int> &closeToOpen,
                                             const std::map<QString, ChainState> &vars,
                                             int endPos)
            {
                std::vector<ChainStep> steps;
                if (!collectChainStepsBack(t, skipMask, closeToOpen, endPos, steps))
                    return {};
                return runChainSteps(steps, vars);
            }

            // Advance across a complete RHS chain expression starting at `rhsStart`; returns
            // the inclusive end index of the completed chain, or -1 if incomplete / still being typed.
            int scanCompletedRhsChainEnd(const QString &t,
                                         const std::vector<bool> &skipMask,
                                         const std::map<int, int> &openToClose,
                                         int rhsStart)
            {
                const int n = t.size();
                int p = rhsStart;
                skipTriviaForward(t, skipMask, p);
                if (identifierForward(t, p).isEmpty())
                    return -1;
                int lastEnd = p - 1;
                for (int guard = 0; guard < 64; ++guard) {
                    int q = p;
                    skipTriviaForward(t, skipMask, q);
                    if (q >= n)
                        return -1;   // Caret is right at the end of this RHS; not a finished prior statement.
                    if (q + 1 < n && t[q] == QLatin1Char('?') && t[q + 1] == QLatin1Char('.'))
                        ++q;
                    if (t[q] == QLatin1Char('.')) {
                        ++q;
                        skipTriviaForward(t, skipMask, q);
                        if (identifierForward(t, q).isEmpty())
                            return -1;
                        p = q;
                        lastEnd = p - 1;
                        continue;
                    }
                    if (t[q] == QLatin1Char('(') || t[q] == QLatin1Char('[')) {
                        const auto it = openToClose.find(q);
                        if (it == openToClose.end())
                            return -1;   // Unclosed call/bracket: caret is inside this RHS.
                        p = it->second + 1;
                        lastEnd = it->second;
                        continue;
                    }
                    break;
                }
                return lastEnd < n - 1 ? lastEnd : -1;
            }

            // ---- MQL Role Transitions ------------------------------------------

            ObjectRole argumentRole(Receiver receiver, const QString &method, int arg, QChar open)
            {
                static const QSet<QString> filterFirst = {
                    QStringLiteral("find"), QStringLiteral("findOne"), QStringLiteral("count"),
                    QStringLiteral("countDocuments"), QStringLiteral("deleteOne"), QStringLiteral("deleteMany"),
                    QStringLiteral("remove"), QStringLiteral("findOneAndDelete"), QStringLiteral("findOneAndUpdate"),
                    QStringLiteral("findOneAndReplace"), QStringLiteral("updateOne"), QStringLiteral("updateMany"),
                    QStringLiteral("update"), QStringLiteral("replaceOne")};
                static const QSet<QString> updates = {
                    QStringLiteral("updateOne"), QStringLiteral("updateMany"),
                    QStringLiteral("update"), QStringLiteral("findOneAndUpdate")};
                static const QSet<QString> fieldsFirst = {
                    QStringLiteral("sort"), QStringLiteral("projection"), QStringLiteral("hint"),
                    QStringLiteral("createIndex"), QStringLiteral("ensureIndex"), QStringLiteral("analyzeShardKey"),
                    QStringLiteral("insertOne"), QStringLiteral("insert"), QStringLiteral("save")};

                const bool onCollOrCursor = (receiver == Receiver::Collection
                                             || receiver == Receiver::QueryCursor
                                             || receiver == Receiver::Explainable);
                const bool onDb = (receiver == Receiver::Database || receiver == Receiver::SiblingDatabase);

                if (open == QLatin1Char('[')) {
                    if (onCollOrCursor && (method == QLatin1String("aggregate") || method == QLatin1String("watch")) && arg == 0)
                        return ObjectRole::Pipeline;
                    if (onDb && method == QLatin1String("createView") && arg == 2)
                        return ObjectRole::Pipeline;
                    if (onCollOrCursor && arg == 1 && updates.contains(method))
                        return ObjectRole::Pipeline;   // MongoDB 4.2+ aggregation pipeline update
                    if (onCollOrCursor && arg == 0
                        && (method == QLatin1String("insertMany") || method == QLatin1String("insert")
                            || method == QLatin1String("createIndexes")))
                        return ObjectRole::DocList;
                    if (onCollOrCursor && arg == 0 && method == QLatin1String("bulkWrite"))
                        return ObjectRole::BulkWriteList;
                    return ObjectRole::None;
                }

                if (!onCollOrCursor)
                    return ObjectRole::None;

                if (arg == 0 && filterFirst.contains(method))
                    return ObjectRole::Filter;
                if (arg == 0 && fieldsFirst.contains(method))
                    return ObjectRole::Fields;
                if (arg == 0 && method == QLatin1String("findAndModify"))
                    return ObjectRole::FindAndModifySpec;
                if (arg == 1 && (method == QLatin1String("find") || method == QLatin1String("findOne")))
                    return ObjectRole::Fields;
                if (arg == 1 && (method == QLatin1String("replaceOne") || method == QLatin1String("findOneAndReplace")))
                    return ObjectRole::Fields;
                if (arg == 1 && updates.contains(method))
                    return ObjectRole::Update;
                if (arg == 1 && method == QLatin1String("distinct"))
                    return ObjectRole::Filter;
                return ObjectRole::None;
            }

            ObjectRole valueRole(ObjectRole parent, bool inAggregate, const QString &key, QChar open)
            {
                const bool object = (open == QLatin1Char('{'));
                switch (parent) {
                case ObjectRole::Filter:
                    if (!object && (key == QLatin1String("$and") || key == QLatin1String("$or")
                                    || key == QLatin1String("$nor")))
                        return ObjectRole::FilterList;
                    if (object && key == QLatin1String("$elemMatch"))
                        return ObjectRole::Filter;
                    if (object && key == QLatin1String("$not"))
                        return ObjectRole::FieldCondition;
                    if (key == QLatin1String("$expr"))
                        return object ? ObjectRole::AggExpression : ObjectRole::AggExprList;
                    return (object && !key.isEmpty() && !key.startsWith(QLatin1Char('$')))
                        ? ObjectRole::FieldCondition
                        : ObjectRole::None;
                case ObjectRole::FieldCondition:
                    if (object && key == QLatin1String("$elemMatch"))
                        return ObjectRole::Filter;
                    if (object && key == QLatin1String("$not"))
                        return ObjectRole::FieldCondition;
                    return ObjectRole::None;
                case ObjectRole::Update:
                    return (object && key.startsWith(QLatin1Char('$'))) ? ObjectRole::Fields : ObjectRole::None;
                case ObjectRole::FindAndModifySpec:
                    if (!object)
                        return ObjectRole::None;
                    if (key == QLatin1String("query"))
                        return ObjectRole::Filter;
                    if (key == QLatin1String("sort") || key == QLatin1String("fields"))
                        return ObjectRole::Fields;
                    if (key == QLatin1String("update"))
                        return ObjectRole::Update;
                    return ObjectRole::None;
                case ObjectRole::BulkWriteOp:
                    return object ? ObjectRole::BulkWriteSpec : ObjectRole::None;
                case ObjectRole::BulkWriteSpec:
                    if (!object)
                        return ObjectRole::None;
                    if (key == QLatin1String("filter"))
                        return ObjectRole::Filter;
                    if (key == QLatin1String("update"))
                        return ObjectRole::Update;
                    if (key == QLatin1String("replacement") || key == QLatin1String("document")
                        || key == QLatin1String("hint"))
                        return ObjectRole::Fields;
                    return ObjectRole::None;
                case ObjectRole::Stage:
                    if (key == QLatin1String("$match"))
                        return object ? ObjectRole::Filter : ObjectRole::None;
                    if (key == QLatin1String("$sort") || key == QLatin1String("$project")
                        || key == QLatin1String("$addFields") || key == QLatin1String("$set")
                        || key == QLatin1String("$unset"))
                        return object ? ObjectRole::Fields : ObjectRole::None;
                    if (key == QLatin1String("$group"))
                        return object ? ObjectRole::GroupBody : ObjectRole::None;
                    if (key == QLatin1String("$lookup") || key == QLatin1String("$unionWith"))
                        return object ? ObjectRole::LookupBody : ObjectRole::None;
                    if (key == QLatin1String("$facet"))
                        return object ? ObjectRole::FacetBody : ObjectRole::None;
                    return object ? ObjectRole::AggExpression : ObjectRole::AggExprList;
                case ObjectRole::LookupBody:
                    if (!object && key == QLatin1String("pipeline"))
                        return ObjectRole::Pipeline;
                    return ObjectRole::None;
                case ObjectRole::FacetBody:
                    return !object ? ObjectRole::Pipeline : ObjectRole::None;
                case ObjectRole::GroupBody:
                    if (key == QLatin1String("_id"))
                        return object ? ObjectRole::AggExpression : ObjectRole::AggExprList;
                    return (object && !key.isEmpty()) ? ObjectRole::GroupFieldVal : ObjectRole::None;
                case ObjectRole::Fields:
                    if (inAggregate)
                        return object ? ObjectRole::AggExpression : ObjectRole::AggExprList;
                    return ObjectRole::None;
                case ObjectRole::GroupFieldVal:
                case ObjectRole::AggExpression:
                    return object ? ObjectRole::AggExpression : ObjectRole::AggExprList;
                default:
                    return ObjectRole::None;
                }
            }

            ObjectRole elementRole(ObjectRole parent, QChar open)
            {
                const bool object = (open == QLatin1Char('{'));
                switch (parent) {
                case ObjectRole::Pipeline:      return object ? ObjectRole::Stage : ObjectRole::None;
                case ObjectRole::FilterList:    return object ? ObjectRole::Filter : ObjectRole::None;
                case ObjectRole::DocList:       return object ? ObjectRole::Fields : ObjectRole::None;
                case ObjectRole::BulkWriteList: return object ? ObjectRole::BulkWriteOp : ObjectRole::None;
                case ObjectRole::AggExprList:   return object ? ObjectRole::AggExpression : ObjectRole::AggExprList;
                default:                        return ObjectRole::None;
                }
            }

            bool isJsBlockBrace(const Frame *top, QChar lastSig, bool afterArrow, const QString &lastWord)
            {
                if (afterArrow)
                    return true;
                if (lastSig == QLatin1Char(')'))
                    return true;
                if (lastWord == QLatin1String("else") || lastWord == QLatin1String("try")
                    || lastWord == QLatin1String("finally") || lastWord == QLatin1String("do"))
                    return true;
                if (!top || top->role == ObjectRole::JsBlock) {
                    if (lastWord == QLatin1String("return"))
                        return false;
                    if (!lastSig.isNull() && QStringLiteral("=:(,?[").contains(lastSig))
                        return false;
                    return true;
                }
                return false;
            }

            // ---- Formatting & Ranking Helpers ----------------------------------

}
