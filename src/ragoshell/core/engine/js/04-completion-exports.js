  // ---------- autocomplete ----------

  const DB_METHOD_COMPLETIONS = [
    'getCollection(', 'getCollectionNames()', 'getCollectionInfos()',
    'createCollection(', 'createView(', 'getSiblingDB(', 'getName()', 'getMongo()', 'system',
    'stats()', 'runCommand(', 'adminCommand(', 'serverStatus()', 'serverBuildInfo()',
    'hostInfo()', 'version()', 'hello()', 'isMaster()', 'listCommands()',
    'currentOp()', 'killOp(', 'connPoolStats()', 'getFeatureCompatibilityVersion()',
    'printCollectionStats()', 'getProfilingStatus()', 'setProfilingLevel(',
    'getUsers()', 'getUser(', 'createUser(', 'updateUser(', 'changeUserPassword(',
    'dropUser(', 'dropAllUsers()', 'grantRolesToUser(', 'revokeRolesFromUser(',
    'getRoles()', 'dropDatabase()', 'repairDatabase()',
  ];

  const DB_METHOD_NAME_SET = new Set([
    'getName', 'toString', 'getSiblingDB', 'getCollection', 'getMongo',
    'runCommand', 'adminCommand', 'getCollectionNames', 'getCollectionInfos',
    'createCollection', 'createView', 'connPoolStats', 'getFeatureCompatibilityVersion',
    'dropDatabase', 'repairDatabase', 'stats', 'printCollectionStats',
    'serverStatus', 'hostInfo', 'version', 'serverBuildInfo', 'listCommands',
    'currentOp', 'killOp', 'getUsers', 'getUser', 'createUser', 'updateUser',
    'changeUserPassword', 'dropUser', 'dropAllUsers', 'grantRolesToUser',
    'revokeRolesFromUser', 'getRoles', 'getProfilingStatus', 'setProfilingLevel',
    'isMaster', 'hello', 'system',
  ]);

  const COLL_METHOD_COMPLETIONS = [
    'find({})', 'findOne({})', 'aggregate([])', 'countDocuments({})',
    'estimatedDocumentCount()', 'count({})', 'distinct(',
    'insertOne({})', 'insertMany([])', 'updateOne(', 'updateMany(',
    'replaceOne(', 'deleteOne(', 'deleteMany(',
    'findOneAndUpdate(', 'findOneAndReplace(', 'findOneAndDelete(',
    'findAndModify(', 'bulkWrite([])', 'insert(', 'update(', 'save(', 'remove(',
    'getIndexes()', 'getIndexSpecs()', 'getIndexKeys()',
    'createIndex(', 'createIndexes([])', 'ensureIndex(',
    'dropIndex(', 'dropIndexes()', 'hideIndex(', 'unhideIndex(', 'reIndex()',
    'explain()', 'stats()', 'latencyStats()', 'storageSize()', 'totalIndexSize()',
    'dataSize()', 'totalSize()', 'isCapped()', 'validate()', 'compact()',
    'renameCollection(', 'convertToCapped(', 'analyzeShardKey(',
    'getShardDistribution()', 'getShardVersion()', 'getName()', 'getFullName()',
    'getDB()', 'drop()',
  ];

  const QUERY_CURSOR_COMPLETIONS = [
    'sort({})', 'limit(', 'skip(', 'projection({})', 'hint(', 'collation({})',
    'batchSize(', 'maxTimeMS(', 'pretty()', 'readPref(',
    'toArray()', 'count()', 'size()', 'itcount()',
    'forEach(', 'map(', 'hasNext()', 'next()', 'explain()', 'close()',
  ];

  const CHAINABLE_CURSOR_METHODS = new Set([
    'sort', 'limit', 'skip', 'projection', 'hint', 'collation',
    'batchSize', 'maxTimeMS', 'pretty', 'readPref',
  ]);

  const AGG_CURSOR_COMPLETIONS = [
    'toArray()', 'pretty()', 'forEach(', 'map(', 'itcount()',
    'hasNext()', 'next()', 'explain()', 'close()',
  ];

  const EXPLAINABLE_COMPLETIONS = [
    'find({})', 'findOne({})', 'aggregate([])', 'count({})', 'distinct(', 'remove(',
  ];

  const SYSTEM_NAMESPACE_COMPLETIONS = ['users', 'roles', 'profile', 'views', 'js'];

  const MONGO_METHOD_COMPLETIONS = [
    'getDBNames()', 'getDBs()', 'host', 'setReadPref(', 'setSlaveOk()',
  ];

  const RS_METHOD_COMPLETIONS = [
    'status()', 'conf()', 'config()', 'hello()', 'isMaster()', 'secondaryOk()', 'slaveOk()',
  ];

  const SH_METHOD_COMPLETIONS = ['status()', 'getBalancerState()'];

  const EJSON_METHOD_COMPLETIONS = ['stringify(', 'parse(', 'serialize(', 'deserialize('];

  const CONSOLE_METHOD_COMPLETIONS = ['log(', 'info(', 'warn(', 'error('];

  const STAGE_OPERATORS = [
    '$match', '$group', '$project', '$sort', '$limit', '$skip', '$lookup',
    '$unwind', '$addFields', '$set', '$unset', '$count', '$facet',
    '$replaceRoot', '$replaceWith', '$bucket', '$bucketAuto', '$sortByCount',
    '$sample', '$unionWith', '$setWindowFields', '$densify', '$fill',
    '$redact', '$geoNear', '$graphLookup', '$merge', '$out',
    '$collStats', '$indexStats', '$search', '$vectorSearch',
  ];

  const UPDATE_OPERATORS = [
    '$set', '$unset', '$inc', '$push', '$pull', '$addToSet', '$pop',
    '$pullAll', '$min', '$max', '$mul', '$rename', '$currentDate',
    '$setOnInsert', '$bit',
  ];

  const FIELD_QUERY_OPERATORS = [
    '$eq', '$ne', '$gt', '$gte', '$lt', '$lte', '$in', '$nin',
    '$exists', '$type', '$regex', '$options', '$not', '$elemMatch',
    '$all', '$size', '$mod', '$bitsAllClear', '$bitsAllSet',
    '$bitsAnyClear', '$bitsAnySet', '$geoWithin', '$geoIntersects',
    '$near', '$nearSphere',
  ];

  const TOP_FILTER_OPERATORS = [
    '$and', '$or', '$nor', '$expr', '$jsonSchema', '$text', '$where', '$comment',
  ];

  const MONGO_OPERATORS = Array.from(new Set(
    STAGE_OPERATORS.concat(
      UPDATE_OPERATORS,
      FIELD_QUERY_OPERATORS,
      TOP_FILTER_OPERATORS,
      ['$sum', '$avg', '$first', '$last', '$topN', '$bottomN', '$maxN', '$minN',
       '$median', '$percentile', '$stdDevPop', '$stdDevSamp', '$cond', '$ifNull',
       '$switch', '$concat', '$toLower', '$toUpper', '$trim', '$split', '$filter',
       '$map', '$reduce', '$arrayElemAt', '$mergeObjects', '$getField',
       '$dateToString', '$dateTrunc', '$dateAdd', '$dateDiff'])
  ));

  const GLOBAL_COMPLETIONS = [
    'db', 'rs', 'sh',
    'show dbs', 'show databases', 'show collections', 'show tables',
    'show users', 'show roles', 'show profile', 'show logs', 'show log',
    'use ', 'ObjectId(', 'ISODate(', 'NumberLong(', 'NumberInt(', 'NumberDecimal(',
    'Timestamp(', 'UUID(', 'BinData(', 'HexData(', 'MinKey()', 'MaxKey()', 'DBRef(',
    'EJSON', 'console', 'print(', 'printjson(',
  ];

  const JS_RESERVED = new Set([
    'await', 'break', 'case', 'catch', 'class', 'const', 'continue', 'debugger',
    'default', 'delete', 'do', 'else', 'export', 'extends', 'false', 'finally',
    'for', 'function', 'if', 'import', 'in', 'instanceof', 'let', 'new', 'null',
    'return', 'super', 'switch', 'this', 'throw', 'true', 'try', 'typeof', 'var',
    'void', 'while', 'with', 'yield',
  ]);

  function isDotCollectionName(name) {
    return Boolean(name) && !name.startsWith('_') && /^[A-Za-z$][\w$]*$/.test(name)
      && !DB_METHOD_NAME_SET.has(name) && !JS_RESERVED.has(name);
  }

  function formatDbCollection(name) {
    if (isDotCollectionName(name)) return 'db.' + name;
    return 'db.getCollection(\'' + String(name).replace(/\\/g, '\\\\').replace(/'/g, '\\\'') + '\')';
  }

  // The collection names of the current database, sorted. Only the names are asked for: a database can
  // have thousands of collections and this runs while the user types.
  function currentCollectionNames() {
    let names;
    try {
      const reply = command(state.dbName, { listCollections: 1, nameOnly: true });
      const more = reply.cursor && Number(reply.cursor.id) !== 0;
      names = more ? host.listCollections(state.dbName, {}).map((c) => c.name)
                   : reply.cursor.firstBatch.map((c) => c.name);
    } catch (e) {
      return [];
    }
    return names.sort();
  }

  // Scan `src` skipping comments, strings, and regexes; returns null when the end of `src`
  // lies inside a comment or unclosed string/regex, otherwise `{ stack, closeToOpen, skipMask }`.
  function scanPrefix(src) {
    const n = src.length;
    const skipMask = new Array(n).fill(false);
    const stack = [];
    const closeToOpen = new Map();
    let lastSig = '';

    for (let i = 0; i < n; i++) {
      const c = src[i];
      const next = i + 1 < n ? src[i + 1] : '';
      if (c === '/' && next === '/') {
        const nl = src.indexOf('\n', i);
        if (nl < 0) return null;
        for (let k = i; k < nl; k++) skipMask[k] = true;
        i = nl - 1;
        continue;
      }
      if (c === '/' && next === '*') {
        const end = src.indexOf('*/', i + 2);
        if (end < 0) return null;
        for (let k = i; k <= end + 1; k++) skipMask[k] = true;
        i = end + 1;
        continue;
      }
      if (c === '/' && (lastSig === '' || '(,:[{=!&|?;+-*%^~<>'.indexOf(lastSig) >= 0)) {
        let j = i + 1;
        let inClass = false;
        for (; j < n; j++) {
          const d = src[j];
          if (d === '\\') j++;
          else if (d === '[') inClass = true;
          else if (d === ']') inClass = false;
          else if ((d === '/' && !inClass) || d === '\n') break;
        }
        if (j >= n) return null;
        if (src[j] === '/') {
          i = j;
          while (i + 1 < n && /[A-Za-z]/.test(src[i + 1])) i++;
          lastSig = '/';
          continue;
        }
      }
      if (c === '\'' || c === '"' || c === '`') {
        let j = i + 1;
        for (; j < n; j++) {
          if (src[j] === '\\') j++;
          else if (src[j] === c) break;
          else if (src[j] === '\n' && c !== '`') break;
        }
        if (j >= n) return null;
        if (src[j] === c) {
          const top = stack.length > 0 ? stack[stack.length - 1] : null;
          if (top && top.open === '{' && top.keyPos) top.keyToken = src.slice(i + 1, j);
          i = j;
          lastSig = c;
          continue;
        }
      }
      if (/\s/.test(c)) continue;

      const top = stack.length > 0 ? stack[stack.length - 1] : null;
      if (c === '(' || c === '[' || c === '{') {
        let callName = '';
        if (c === '(') {
          let p = i - 1;
          while (p >= 0 && (/\s/.test(src[p]) || skipMask[p])) p--;
          const end = p;
          while (p >= 0 && /[\w$]/.test(src[p])) p--;
          callName = src.slice(p + 1, end + 1);
        }
        stack.push({ open: c, pos: i, callName: callName, arg: 0, keyPos: true, keyToken: '', lastKey: '' });
        lastSig = c;
        continue;
      }
      if (c === ')' || c === ']' || c === '}') {
        if (stack.length > 0) {
          const popped = stack.pop();
          closeToOpen.set(i, popped.pos);
        }
        lastSig = c;
        continue;
      }
      if (top && top.open === '{') {
        if (c === ':' && top.keyPos) {
          top.lastKey = top.keyToken;
          top.keyToken = '';
          top.keyPos = false;
        } else if (c === ',') {
          top.keyPos = true;
          top.keyToken = '';
          top.lastKey = '';
        } else if (top.keyPos && /[\w$]/.test(c)) {
          top.keyToken += c;
        }
      } else if (top && c === ',') {
        top.arg++;
      }
      lastSig = c;
    }
    return { stack: stack, closeToOpen: closeToOpen, skipMask: skipMask };
  }

  function resolveReceiverBeforeDot(src, dotPos, scan) {
    const steps = [];
    let p = dotPos - 1;
    for (let guard = 0; guard < 64; guard++) {
      while (p >= 0 && (/\s/.test(src[p]) || scan.skipMask[p])) p--;
      if (p < 0) break;
      if (src[p] === ')') {
        const open = scan.closeToOpen.get(p);
        if (open === undefined) return 'none';
        const rawArgs = src.slice(open + 1, p).trim();
        let q = open - 1;
        while (q >= 0 && (/\s/.test(src[q]) || scan.skipMask[q])) q--;
        const end = q;
        while (q >= 0 && /[\w$]/.test(src[q])) q--;
        const ident = src.slice(q + 1, end + 1);
        if (!ident || /^\d/.test(ident)) return 'none';
        steps.push({ kind: 'call', name: ident, rawArgs: rawArgs });
        while (q >= 0 && (/\s/.test(src[q]) || scan.skipMask[q])) q--;
        if (q >= 0 && src[q] === '.') {
          if (q > 0 && src[q - 1] === '.') return 'none';
          p = q - 1;
          continue;
        }
        break;
      }
      if (src[p] === ']') {
        const open = scan.closeToOpen.get(p);
        if (open === undefined) return 'none';
        const inner = src.slice(open + 1, p).trim();
        if (inner.length < 2 || (inner[0] !== '\'' && inner[0] !== '"') || inner[inner.length - 1] !== inner[0])
          return 'none';
        steps.push({ kind: 'bracket', name: inner.slice(1, -1) });
        let q = open - 1;
        while (q >= 0 && (/\s/.test(src[q]) || scan.skipMask[q])) q--;
        p = (q >= 0 && src[q] === '.') ? q - 1 : q;
        continue;
      }
      if (/[\w$]/.test(src[p])) {
        const end = p;
        while (p >= 0 && /[\w$]/.test(src[p])) p--;
        const ident = src.slice(p + 1, end + 1);
        if (!ident || /^\d/.test(ident)) return 'none';
        steps.push({ kind: 'prop', name: ident });
        while (p >= 0 && (/\s/.test(src[p]) || scan.skipMask[p])) p--;
        if (p >= 0 && src[p] === '.') {
          if (p > 0 && src[p - 1] === '.') return 'none';
          p = p - 1;
          continue;
        }
        break;
      }
      return 'none';
    }
    if (steps.length === 0) return 'none';
    steps.reverse();
    const root = steps[0];
    if (root.kind !== 'prop') return 'none';
    let stateKind = 'none';
    if (root.name === 'db') stateKind = 'db';
    else if (root.name === 'rs') stateKind = 'rs';
    else if (root.name === 'sh') stateKind = 'sh';
    else if (root.name === 'EJSON') stateKind = 'ejson';
    else if (root.name === 'console') stateKind = 'console';
    else return 'none';

    for (let i = 1; i < steps.length; i++) {
      const s = steps[i];
      if (stateKind === 'db' || stateKind === 'siblingDb') {
        if (s.kind === 'prop' || s.kind === 'bracket') {
          if (s.name === 'system') stateKind = 'system';
          else if (!s.name || s.name.startsWith('_') || DB_METHOD_NAME_SET.has(s.name)) return 'none';
          else stateKind = 'collection';
        } else {
          if (s.name === 'getCollection') stateKind = 'collection';
          else if (s.name === 'getSiblingDB') stateKind = 'siblingDb';
          else if (s.name === 'getMongo') stateKind = 'mongo';
          else return 'none';
        }
      } else if (stateKind === 'system') {
        if ((s.kind === 'prop' || s.kind === 'bracket') && s.name) stateKind = 'collection';
        else return 'none';
      } else if (stateKind === 'collection') {
        if (s.kind === 'prop' || s.kind === 'bracket') {
          if (!s.name) return 'none';
          stateKind = 'collection';
        } else {
          if (s.name === 'find') stateKind = 'queryCursor';
          else if (s.name === 'aggregate') stateKind = 'aggCursor';
          else if (s.name === 'explain') stateKind = 'explainable';
          else if (s.name === 'getDB') stateKind = 'siblingDb';
          else return 'none';
        }
      } else if (stateKind === 'queryCursor') {
        if (s.kind === 'call' && CHAINABLE_CURSOR_METHODS.has(s.name)) stateKind = 'queryCursor';
        else return 'none';
      } else if (stateKind === 'aggCursor') {
        if (s.kind === 'call' && s.name === 'pretty') stateKind = 'aggCursor';
        else return 'none';
      } else {
        return 'none';
      }
    }
    return stateKind;
  }

  function operatorsForStack(stack) {
    if (stack.length === 0) return MONGO_OPERATORS;
    const top = stack[stack.length - 1];
    if (top.open !== '{' || !top.keyPos) return [];
    const parent = stack.length >= 2 ? stack[stack.length - 2] : null;
    const grand = stack.length >= 3 ? stack[stack.length - 3] : null;
    if (parent && parent.open === '[' && grand && grand.open === '('
        && (grand.callName === 'aggregate' || grand.callName === 'watch'
            || (grand.callName === 'createView' && grand.arg === 2))) {
      return STAGE_OPERATORS;
    }
    if (parent && parent.open === '(' && parent.arg === 1
        && ['updateOne', 'updateMany', 'update', 'findOneAndUpdate'].indexOf(parent.callName) >= 0) {
      return UPDATE_OPERATORS;
    }
    if (parent && parent.open === '{' && parent.lastKey && !parent.lastKey.startsWith('$')
        && ['query', 'update', 'filter', 'sort', 'fields', 'pipeline'].indexOf(parent.lastKey) < 0) {
      return FIELD_QUERY_OPERATORS;
    }
    if (parent && parent.open === '(' && parent.arg === 0
        && ['find', 'findOne', 'count', 'countDocuments', 'deleteOne', 'deleteMany',
            'remove', 'updateOne', 'updateMany', 'findOneAndUpdate', 'findOneAndDelete'].indexOf(parent.callName) >= 0) {
      return TOP_FILTER_OPERATORS;
    }
    if (parent && parent.open === '{' && parent.lastKey === '$match') {
      return TOP_FILTER_OPERATORS;
    }
    return MONGO_OPERATORS;
  }

  function buildCompletions(rawPrefix, includeCollections) {
    const prefix = String(rawPrefix || '');
    const allowColls = includeCollections !== false;
    if (prefix === '') return GLOBAL_COMPLETIONS.slice();

    const scan = scanPrefix(prefix);
    if (!scan) return [];   // Inside comment or unclosed string / regex

    const items = new Set();

    // Case 1: Dollar operator (`$ma` or contextual `db.c.aggregate([{ $m`)
    const dollarMatch = prefix.match(/(\$[\w]*)$/);
    if (dollarMatch) {
      const opPrefix = dollarMatch[1];
      const head = prefix.slice(0, prefix.length - opPrefix.length);
      if (head === '' || /[{,\s]$/.test(head)) {
        const pool = head === '' ? MONGO_OPERATORS : operatorsForStack(scan.stack);
        for (const op of pool) {
          if (op.startsWith(opPrefix)) items.add(head + op);
        }
        return Array.from(items);
      }
      return [];
    }

    // Case 2: Member access `<chain>.<memberPrefix>`
    const memberMatch = prefix.match(/\.([\w$]*)$/);
    if (memberMatch) {
      const memberPrefix = memberMatch[1] || '';
      const dotPos = prefix.length - memberPrefix.length - 1;
      if (dotPos > 0 && prefix[dotPos - 1] === '.') return [];
      const baseExpr = prefix.slice(0, dotPos + 1);
      const kind = resolveReceiverBeforeDot(prefix, dotPos, scan);

      const addMethods = (list) => {
        for (const m of list) {
          if (m.startsWith(memberPrefix)) items.add(baseExpr + m);
        }
      };

      if (kind === 'db') {
        if (allowColls) {
          for (const c of currentCollectionNames()) {
            const full = formatDbCollection(c);
            const afterDb = full.slice(3);   // strip leading "db."
            if (c.startsWith(memberPrefix) || afterDb.startsWith(memberPrefix))
              items.add(full);
          }
        }
        addMethods(DB_METHOD_COMPLETIONS);
      } else if (kind === 'siblingDb') {
        addMethods(DB_METHOD_COMPLETIONS);
      } else if (kind === 'system') {
        addMethods(SYSTEM_NAMESPACE_COMPLETIONS);
      } else if (kind === 'mongo') {
        addMethods(MONGO_METHOD_COMPLETIONS);
      } else if (kind === 'collection') {
        addMethods(COLL_METHOD_COMPLETIONS);
      } else if (kind === 'queryCursor') {
        addMethods(QUERY_CURSOR_COMPLETIONS);
      } else if (kind === 'aggCursor') {
        addMethods(AGG_CURSOR_COMPLETIONS);
      } else if (kind === 'explainable') {
        addMethods(EXPLAINABLE_COMPLETIONS);
      } else if (kind === 'rs') {
        addMethods(RS_METHOD_COMPLETIONS);
      } else if (kind === 'sh') {
        addMethods(SH_METHOD_COMPLETIONS);
      } else if (kind === 'ejson') {
        addMethods(EJSON_METHOD_COMPLETIONS);
      } else if (kind === 'console') {
        addMethods(CONSOLE_METHOD_COMPLETIONS);
      }
      return Array.from(items);
    }

    // Case 3: `show ...` or bare top-level global/command prefix
    if (scan.stack.length === 0 && /^(?:show\s+[\w ]*|use\s*|[A-Za-z_$][\w$]*)$/.test(prefix)) {
      for (const g of GLOBAL_COMPLETIONS) {
        if (g.startsWith(prefix)) items.add(g);
      }
    }
    return Array.from(items);
  }

  return {
    configure(options) {
      if (options.dbName) state.dbName = options.dbName;
      if (options.batchSize > 0) state.batchSize = options.batchSize;
      state.startupScript = null;
      let startupError = '';
      if (options.startupScript && options.startupScript.trim().length > 0) {
        startupError = checkSyntax(options.startupScript);
        if (startupError === '') state.startupScript = options.startupScript;
      }
      return startupError;
    },
    setBatchSize(n) { state.batchSize = n > 0 ? n : 50; },
    use(dbName) { state.dbName = String(dbName); },
    checkSyntax: checkSyntax,
    evalScript: evalScript,
    complete: buildCompletions,
  };
})()
