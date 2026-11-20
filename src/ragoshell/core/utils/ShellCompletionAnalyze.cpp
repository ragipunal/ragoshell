#include "ragoshell/core/utils/ShellCompletionInternal.h"

#include <algorithm>

#include <QRegularExpression>

#include "ragoshell/core/utils/FieldCompletion.h"

namespace RagoShell::ShellCompletion
{
    using namespace detail;

        Context analyze(const QString &textBeforeCaret)
        {
            const QString t = textBeforeCaret.right(kMaxScan);
            const int n = t.size();
            if (n == 0) {
                Context ctx;
                ctx.scope = Scope::StatementStart;
                return ctx;
            }

            std::vector<bool> skipMask(static_cast<size_t>(n), false);
            std::vector<Frame> stack;
            std::map<int, int> closeToOpen;
            std::map<int, int> openToClose;
            std::vector<PendingAssignment> pendingAssignments;

            int stringStart = -1;
            QChar quote;
            QChar lastSignificant;
            QChar prevSignificant;
            QChar lastNonIdentifier;
            QString currentWord;
            QString lastCompletedWord;

            auto markSignificant = [&](QChar ch) {
                if (!isIdentChar(ch) && !currentWord.isEmpty()) {
                    lastCompletedWord = currentWord;
                    currentWord.clear();
                }
                prevSignificant = lastSignificant;
                lastSignificant = ch;
                if (!isIdentChar(ch))
                    lastNonIdentifier = ch;
            };

            for (int i = 0; i < n; ++i) {
                const QChar c = t[i];
                const QChar next = (i + 1 < n) ? t[i + 1] : QChar();

                // Line comment //
                if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
                    const int end = t.indexOf(QLatin1Char('\n'), i);
                    if (end < 0)
                        return {};   // Caret is inside a line comment
                    for (int k = i; k < end; ++k)
                        skipMask[static_cast<size_t>(k)] = true;
                    if (!currentWord.isEmpty()) {
                        lastCompletedWord = currentWord;
                        currentWord.clear();
                    }
                    i = end - 1;
                    continue;
                }

                // Block comment /* ... */
                if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
                    const int end = t.indexOf(QStringLiteral("*/"), i + 2);
                    if (end < 0)
                        return {};   // Caret is inside a block comment
                    for (int k = i; k <= end + 1; ++k)
                        skipMask[static_cast<size_t>(k)] = true;
                    if (!currentWord.isEmpty()) {
                        lastCompletedWord = currentWord;
                        currentWord.clear();
                    }
                    i = end + 1;
                    continue;
                }

                // Regex literal /pattern/flags
                const QString wordBeforeSlash = !currentWord.isEmpty() ? currentWord : lastCompletedWord;
                const bool regexAfterKeyword = (lastSignificant.isLetter()
                    && (wordBeforeSlash == QLatin1String("return") || wordBeforeSlash == QLatin1String("typeof")
                        || wordBeforeSlash == QLatin1String("throw") || wordBeforeSlash == QLatin1String("case")
                        || wordBeforeSlash == QLatin1String("in") || wordBeforeSlash == QLatin1String("delete")
                        || wordBeforeSlash == QLatin1String("void")));
                if (c == QLatin1Char('/')
                    && (lastSignificant.isNull()
                        || QStringLiteral("(,:[{=!&|?;+-*%^~<>").contains(lastSignificant)
                        || regexAfterKeyword)) {
                    int j = i + 1;
                    bool inClass = false;
                    for (; j < n; ++j) {
                        const QChar d = t[j];
                        if (d == QLatin1Char('\\')) {
                            ++j;
                        } else if (d == QLatin1Char('[')) {
                            inClass = true;
                        } else if (d == QLatin1Char(']')) {
                            inClass = false;
                        } else if ((d == QLatin1Char('/') && !inClass) || d == QLatin1Char('\n')) {
                            break;
                        }
                    }
                    if (j >= n)
                        return {};   // Caret is inside an unclosed regex literal
                    if (t[j] == QLatin1Char('/')) {
                        i = j;
                        while (i + 1 < n && t[i + 1].isLetter())
                            ++i;
                        markSignificant(QLatin1Char('/'));
                        continue;
                    }
                }

                // String or template literal
                if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')) {
                    int j = i + 1;
                    for (; j < n; ++j) {
                        if (t[j] == QLatin1Char('\\')) {
                            ++j;
                        } else if (t[j] == c) {
                            break;
                        } else if (t[j] == QLatin1Char('\n') && c != QLatin1Char('`')) {
                            // Unterminated single-line string on an earlier line: do not bleed into later lines.
                            break;
                        }
                    }
                    if (j >= n) {
                        stringStart = i;
                        quote = c;
                        break;
                    }
                    if (t[j] == c) {
                        const QString unquoted = unescapeJsString(t.mid(i + 1, j - i - 1));
                        if (!stack.empty() && stack.back().open == QLatin1Char('{') && !stack.back().isJsBlock) {
                            if (stack.back().keyPos)
                                stack.back().keyToken = unquoted;
                            else if (!stack.back().lastKey.isEmpty())
                                stack.back().stringProps[stack.back().lastKey] = unquoted;
                        }
                        i = j;
                        markSignificant(c);
                        continue;
                    }
                }

                if (c.isSpace()) {
                    if (!currentWord.isEmpty()) {
                        lastCompletedWord = currentWord;
                        currentWord.clear();
                    }
                    continue;
                }

                Frame *top = stack.empty() ? nullptr : &stack.back();

                // Track simple variable assignments (`const cur = ...`, `cur = ...`) at top-level or JS block.
                if (c == QLatin1Char('=') && next != QLatin1Char('=') && next != QLatin1Char('>')
                    && (lastSignificant.isNull() || !QStringLiteral("=!<>+-*/%&|^?").contains(lastSignificant))
                    && (!top || top->isJsBlock)) {
                    const QString lhs = !currentWord.isEmpty() ? currentWord : lastCompletedWord;
                    if (!lhs.isEmpty() && isIdentStart(lhs[0]) && !jsReservedKeywords().contains(lhs))
                        pendingAssignments.push_back({lhs, i + 1});
                }

                if (c == QLatin1Char('(') || c == QLatin1Char('{') || c == QLatin1Char('[')) {
                    Frame f;
                    f.open = c;
                    f.pos = i;
                    if (c == QLatin1Char('{')) {
                        const bool afterArrow = (prevSignificant == QLatin1Char('=') && lastSignificant == QLatin1Char('>'));
                        const QString wordBefore = !currentWord.isEmpty() ? currentWord : lastCompletedWord;
                        f.isJsBlock = isJsBlockBrace(top, lastSignificant, afterArrow, wordBefore);
                    }
                    stack.push_back(std::move(f));
                    markSignificant(c);
                    continue;
                }

                if (c == QLatin1Char(')') || c == QLatin1Char('}') || c == QLatin1Char(']')) {
                    if (!stack.empty()) {
                        closeToOpen[i] = stack.back().pos;
                        openToClose[stack.back().pos] = i;
                        stack.pop_back();
                    }
                    markSignificant(c);
                    continue;
                }

                if (top && top->open == QLatin1Char('{') && !top->isJsBlock) {
                    if (c == QLatin1Char(':') && top->keyPos) {
                        top->lastKey = top->keyToken;
                        top->keyToken.clear();
                        top->keyPos = false;
                    } else if (c == QLatin1Char(',')) {
                        top->keyPos = true;
                        top->keyToken.clear();
                        top->lastKey.clear();
                    } else if (top->keyPos && isIdentChar(c)) {
                        const bool continues = (i > 0 && isIdentChar(t[i - 1]) && !top->keyToken.isEmpty());
                        top->keyToken = continues ? (top->keyToken + c) : QString(c);
                    }
                } else if (top && !top->isJsBlock && c == QLatin1Char(',')) {
                    ++top->arg;
                }

                if (isIdentChar(c))
                    currentWord += c;
                markSignificant(c);
            }

            // Phase 2a: Resolve completed variable bindings (`const cur = db.orders.find({});`).
            std::map<QString, ChainState> vars;
            for (const PendingAssignment &pa : pendingAssignments) {
                const int rhsEnd = scanCompletedRhsChainEnd(t, skipMask, openToClose, pa.rhsStart);
                if (rhsEnd >= pa.rhsStart) {
                    const ChainState cs = evaluateChainEndingAt(t, skipMask, closeToOpen, vars, rhsEnd);
                    if (cs.receiver != Receiver::None)
                        vars[pa.varName] = cs;
                    else
                        vars.erase(pa.varName);
                }
            }

            // Phase 2b: Resolve open frames on `stack` top-down.
            for (size_t idx = 0; idx < stack.size(); ++idx) {
                Frame &f = stack[idx];
                const Frame *parent = idx > 0 ? &stack[idx - 1] : nullptr;
                if (parent && !parent->isJsBlock) {
                    f.collection = parent->collection;
                    f.inAggregate = parent->inAggregate;
                }
                if (f.open == QLatin1Char('(')) {
                    int p = f.pos - 1;
                    skipTriviaBack(t, skipMask, p);
                    f.callName = identifierBack(t, p);
                    if (!f.callName.isEmpty() && !f.callName[0].isDigit()) {
                        int q = p;
                        skipTriviaBack(t, skipMask, q);
                        if (q >= 0 && t[q] == QLatin1Char('.')) {
                            const int beforeDot = (q > 0 && t[q - 1] == QLatin1Char('?')) ? q - 2 : q - 1;
                            const ChainState cs = evaluateChainEndingAt(t, skipMask, closeToOpen, vars, beforeDot);
                            f.receiver = cs.receiver;
                            if (cs.receiver != Receiver::None)
                                f.collection = cs.collection;
                        }
                        if (f.callName == QLatin1String("aggregate") || f.callName == QLatin1String("watch")
                            || (f.callName == QLatin1String("createView") && f.arg == 2)) {
                            f.inAggregate = true;
                        }
                    }
                } else if (f.isJsBlock) {
                    f.role = ObjectRole::JsBlock;
                    f.collection.clear();
                    f.inAggregate = false;
                } else if (parent) {
                    if (parent->open == QLatin1Char('('))
                        f.role = argumentRole(parent->receiver, parent->callName, parent->arg, f.open);
                    else if (parent->open == QLatin1Char('{'))
                        f.role = valueRole(parent->role, parent->inAggregate, parent->lastKey, f.open);
                    else if (parent->open == QLatin1Char('['))
                        f.role = elementRole(parent->role, f.open);

                    if (f.role == ObjectRole::Pipeline)
                        f.inAggregate = true;

                    // `$lookup: { from: "warehouses", pipeline: [ ... ] }` switches collection context to `warehouses`.
                    if (parent->open == QLatin1Char('{') && parent->role == ObjectRole::LookupBody
                        && parent->lastKey == QLatin1String("pipeline")) {
                        const auto itFrom = parent->stringProps.find(QStringLiteral("from"));
                        const auto itColl = parent->stringProps.find(QStringLiteral("coll"));
                        if (itFrom != parent->stringProps.end() && !itFrom->second.isEmpty())
                            f.collection = itFrom->second;
                        else if (itColl != parent->stringProps.end() && !itColl->second.isEmpty())
                            f.collection = itColl->second;
                    }
                }
            }

            const Frame *top = stack.empty() ? nullptr : &stack.back();
            QString activeCollection;
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                if (it->isJsBlock)
                    break;
                if (!it->collection.isEmpty()) {
                    activeCollection = it->collection;
                    break;
                }
            }

            // Phase 3a: Caret inside an unclosed string literal.
            if (stringStart >= 0) {
                const QString content = t.mid(stringStart + 1);
                if (quote == QLatin1Char('`') || content.contains(QLatin1Char('\n')) || !top)
                    return {};

                Context ctx;
                ctx.collection = activeCollection;
                ctx.quoteChar = quote;
                ctx.replaceLength = content.size();

                if (top->open == QLatin1Char('{') && !top->isJsBlock) {
                    if (top->keyPos) {
                        ctx.prefix = content;
                        if (content.startsWith(QLatin1Char('$'))) {
                            switch (top->role) {
                            case ObjectRole::Stage:          ctx.scope = Scope::PipelineStageKey; return ctx;
                            case ObjectRole::Update:         ctx.scope = Scope::UpdateOperatorKey; return ctx;
                            case ObjectRole::FieldCondition: ctx.scope = Scope::FieldConditionKey; return ctx;
                            case ObjectRole::Filter:         ctx.scope = Scope::FilterKey; return ctx;
                            case ObjectRole::GroupFieldVal:  ctx.scope = Scope::GroupFieldValue; return ctx;
                            case ObjectRole::AggExpression:  ctx.scope = Scope::AggExpressionKey; return ctx;
                            default:                         return {};
                            }
                        }
                        if ((top->role == ObjectRole::Filter || top->role == ObjectRole::Fields)
                            && !activeCollection.isEmpty()) {
                            ctx.scope = Scope::QuotedFieldKey;
                            return ctx;
                        }
                        return {};
                    }

                    // Value inside an object literal
                    if (content.startsWith(QLatin1Char('$')) && !content.startsWith(QStringLiteral("$$"))
                        && top->inAggregate
                        && (top->role == ObjectRole::Fields || top->role == ObjectRole::GroupBody
                            || top->role == ObjectRole::GroupFieldVal || top->role == ObjectRole::AggExpression)
                        && !activeCollection.isEmpty()) {
                        ctx.scope = Scope::FieldReference;
                        ctx.prefix = content.mid(1);
                        return ctx;
                    }
                    if (top->role == ObjectRole::LookupBody
                        && (top->lastKey == QLatin1String("from") || top->lastKey == QLatin1String("coll"))) {
                        ctx.scope = Scope::CollectionNameArg;
                        ctx.prefix = content;
                        return ctx;
                    }
                    if (top->role == ObjectRole::LookupBody && top->lastKey == QLatin1String("localField")
                        && !activeCollection.isEmpty()) {
                        ctx.scope = Scope::DistinctFieldArg;
                        ctx.prefix = content;
                        return ctx;
                    }
                    if (top->role == ObjectRole::LookupBody && top->lastKey == QLatin1String("foreignField")) {
                        const auto itFrom = top->stringProps.find(QStringLiteral("from"));
                        const auto itColl = top->stringProps.find(QStringLiteral("coll"));
                        const QString foreignColl = (itFrom != top->stringProps.end() && !itFrom->second.isEmpty())
                            ? itFrom->second
                            : ((itColl != top->stringProps.end()) ? itColl->second : QString());
                        if (!foreignColl.isEmpty()) {
                            ctx.collection = foreignColl;
                            ctx.scope = Scope::DistinctFieldArg;
                            ctx.prefix = content;
                            return ctx;
                        }
                    }
                    return {};
                }

                if (top->open == QLatin1Char('[')) {
                    // Check `db['coll|`
                    int p = top->pos - 1;
                    skipTriviaBack(t, skipMask, p);
                    if (p >= 0 && t[p] == QLatin1Char('.')) {
                        if (p > 0 && t[p - 1] == QLatin1Char('?'))
                            p -= 2;
                        else
                            --p;
                    }
                    const ChainState bracketBase = evaluateChainEndingAt(t, skipMask, closeToOpen, vars, p);
                    if (bracketBase.receiver == Receiver::Database && top->arg == 0) {
                        ctx.scope = Scope::CollectionNameArg;
                        ctx.prefix = content;
                        return ctx;
                    }
                    if (content.startsWith(QLatin1Char('$')) && !content.startsWith(QStringLiteral("$$"))
                        && top->inAggregate && top->role == ObjectRole::AggExprList && !activeCollection.isEmpty()) {
                        ctx.scope = Scope::FieldReference;
                        ctx.prefix = content.mid(1);
                        return ctx;
                    }
                    return {};
                }

                if (top->open == QLatin1Char('(')) {
                    ctx.prefix = content;
                    if (top->receiver == Receiver::Database && top->callName == QLatin1String("getCollection") && top->arg == 0) {
                        ctx.scope = Scope::CollectionNameArg;
                        return ctx;
                    }
                    if (top->receiver == Receiver::Database && top->callName == QLatin1String("createView") && top->arg == 1) {
                        ctx.scope = Scope::CollectionNameArg;
                        return ctx;
                    }
                    if (((top->receiver == Receiver::Database || top->receiver == Receiver::SiblingDatabase)
                         && top->callName == QLatin1String("getSiblingDB") && top->arg == 0)
                        || (top->receiver == Receiver::None && top->callName == QLatin1String("use") && top->arg == 0)) {
                        ctx.scope = Scope::DatabaseNameArg;
                        return ctx;
                    }
                    if ((top->receiver == Receiver::Collection || top->receiver == Receiver::Explainable)
                        && top->callName == QLatin1String("distinct") && top->arg == 0 && !activeCollection.isEmpty()) {
                        ctx.scope = Scope::DistinctFieldArg;
                        return ctx;
                    }
                    if ((top->receiver == Receiver::Collection || top->receiver == Receiver::QueryCursor
                         || top->receiver == Receiver::AggCursor)
                        && top->callName == QLatin1String("explain") && top->arg == 0) {
                        ctx.scope = Scope::ExplainVerbosityArg;
                        return ctx;
                    }
                }
                return {};
            }

            // Phase 3b: Caret outside strings and comments.
            int p = n - 1;
            const QString token = identifierBack(t, p);
            if (!token.isEmpty() && token[0].isDigit())
                return {};   // Numeric literal (`10`, `3e5`)

            // 1. Member access `<chain>.<token>` (only when no whitespace separates `.` and `<token>`,
            //    or when `.` is the very last non-comment character typed).
            if (p >= 0 && t[p] == QLatin1Char('.')) {
                if (p > 0 && t[p - 1] == QLatin1Char('.'))
                    return {};   // `..` spread or range
                const int beforeDot = (p > 0 && t[p - 1] == QLatin1Char('?')) ? p - 2 : p - 1;
                const ChainState cs = evaluateChainEndingAt(t, skipMask, closeToOpen, vars, beforeDot);
                if (cs.receiver == Receiver::None)
                    return {};   // e.g. `12.`, `unknown.`, `findOne({}).`, `toArray().`
                Context ctx;
                ctx.scope = Scope::MemberAccess;
                ctx.receiver = cs.receiver;
                ctx.collection = cs.collection;
                ctx.prefix = token;
                ctx.replaceLength = token.size();
                return ctx;
            }

            // 2. Shell helper commands `show <arg>` and `use <db>` at statement start.
            if (!top || top->isJsBlock) {
                int stmtStart = top ? top->pos + 1 : 0;
                for (int k = n - 1; k >= stmtStart; --k) {
                    if (skipMask[static_cast<size_t>(k)])
                        continue;
                    if (t[k] == QLatin1Char('\n') || t[k] == QLatin1Char(';') || t[k] == QLatin1Char('}')) {
                        stmtStart = k + 1;
                        break;
                    }
                }
                // Build uncommented text of current statement line.
                QString stmt;
                stmt.reserve(n - stmtStart);
                for (int k = stmtStart; k < n; ++k) {
                    if (!skipMask[static_cast<size_t>(k)])
                        stmt += t[k];
                }
                int lead = 0;
                while (lead < stmt.size() && stmt[lead].isSpace())
                    ++lead;
                const QString trimmedLeft = stmt.mid(lead);

                static const QRegularExpression showRe(QStringLiteral(R"(^show\s+([A-Za-z ]*)$)"));
                const QRegularExpressionMatch mShow = showRe.match(trimmedLeft);
                if (mShow.hasMatch()) {
                    Context ctx;
                    ctx.scope = Scope::ShowCommand;
                    ctx.prefix = mShow.captured(1);
                    ctx.replaceLength = ctx.prefix.size();
                    return ctx;
                }

                static const QRegularExpression useRe(QStringLiteral(R"(^use\s+([^\s;'"{}()\[\]]*)$)"));
                const QRegularExpressionMatch mUse = useRe.match(trimmedLeft);
                if (mUse.hasMatch()) {
                    Context ctx;
                    ctx.scope = Scope::UseCommand;
                    ctx.prefix = mUse.captured(1);
                    ctx.replaceLength = ctx.prefix.size();
                    return ctx;
                }

                // Bare statement start (`|`, `db|`, `Obj|`, `sh|`).
                if (trimmedLeft == token) {
                    Context ctx;
                    ctx.scope = Scope::StatementStart;
                    ctx.prefix = token;
                    ctx.replaceLength = token.size();
                    return ctx;
                }
            }

            // 3. Inside an object literal `{ ... }`
            if (top && top->open == QLatin1Char('{') && !top->isJsBlock) {
                if (top->keyPos) {
                    if (lastNonIdentifier != QLatin1Char('{') && lastNonIdentifier != QLatin1Char(','))
                        return {};
                    if (token != top->keyToken)
                        return {};

                    Context ctx;
                    ctx.collection = activeCollection;
                    ctx.prefix = token;
                    ctx.replaceLength = token.size();

                    switch (top->role) {
                    case ObjectRole::Stage:
                        ctx.scope = Scope::PipelineStageKey;
                        return ctx;
                    case ObjectRole::Update:
                        ctx.scope = Scope::UpdateOperatorKey;
                        return ctx;
                    case ObjectRole::FieldCondition:
                        ctx.scope = Scope::FieldConditionKey;
                        return ctx;
                    case ObjectRole::Filter:
                        ctx.scope = Scope::FilterKey;
                        return ctx;
                    case ObjectRole::Fields:
                        ctx.scope = Scope::FieldsKey;
                        return ctx;
                    case ObjectRole::GroupBody:
                        ctx.scope = Scope::GroupOutputKey;
                        return ctx;
                    case ObjectRole::GroupFieldVal:
                        ctx.scope = Scope::GroupFieldValue;
                        return ctx;
                    case ObjectRole::AggExpression:
                        ctx.scope = Scope::AggExpressionKey;
                        return ctx;
                    case ObjectRole::LookupBody:
                        ctx.scope = Scope::SpecKey;
                        ctx.helperKind = QStringLiteral("lookup");
                        return ctx;
                    case ObjectRole::FindAndModifySpec:
                        ctx.scope = Scope::SpecKey;
                        ctx.helperKind = QStringLiteral("findAndModify");
                        return ctx;
                    case ObjectRole::BulkWriteOp:
                        ctx.scope = Scope::SpecKey;
                        ctx.helperKind = QStringLiteral("bulkWriteOp");
                        return ctx;
                    case ObjectRole::BulkWriteSpec:
                        ctx.scope = Scope::SpecKey;
                        ctx.helperKind = QStringLiteral("bulkWriteSpec");
                        return ctx;
                    default:
                        if (token.startsWith(QLatin1Char('$'))) {
                            // E.g. `const q = { age: { $gt` vs `const stage = { $ma`
                            const Frame *parent = stack.size() >= 2 ? &stack[stack.size() - 2] : nullptr;
                            if (parent && parent->open == QLatin1Char('{') && !parent->isJsBlock
                                && !parent->lastKey.isEmpty() && !parent->lastKey.startsWith(QLatin1Char('$'))) {
                                ctx.scope = Scope::FieldConditionKey;
                            } else {
                                ctx.scope = Scope::UnboundOperatorKey;
                            }
                            return ctx;
                        }
                        return {};
                    }
                }

                // Value position inside `{ key: | }`
                if (!token.startsWith(QLatin1Char('$'))
                    && !lastNonIdentifier.isNull()
                    && QStringLiteral(":([,?!+-*/=<>|&").contains(lastNonIdentifier)) {
                    Context ctx;
                    ctx.scope = Scope::Expression;
                    ctx.collection = activeCollection;
                    ctx.prefix = token;
                    ctx.replaceLength = token.size();
                    return ctx;
                }
                return {};
            }

            // 4. Value / argument position inside `(...)`, `[...]`, or after an operator (`const id = Obj|`).
            if (!token.startsWith(QLatin1Char('$'))
                && !lastNonIdentifier.isNull()
                && QStringLiteral("([,:=?!+-*/<>|&").contains(lastNonIdentifier)) {
                Context ctx;
                ctx.scope = Scope::Expression;
                ctx.collection = activeCollection;
                ctx.prefix = token;
                ctx.replaceLength = token.size();
                return ctx;
            }

            return {};
        }

}
