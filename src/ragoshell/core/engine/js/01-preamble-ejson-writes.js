// shell-runtime.js -- the shell API, running inside QJSEngine (docs/Shell.md).
//
// A port of the Node shell (src/rago-shell/rago-shell.js, removed, see docs/Shell.md; it is in
// the git history before that commit). The structure was kept (QueryCursor / AggCursor,
// collectionProxy, makeDb, shell helpers, statement splitting, result materialisation) so the two
// files can be compared method by method. The differences:
//
//   - Calls are synchronous. `__host` (ShellHost) answers when the server has; the Node shell had a LazyOp.
//     `db.c.findOne()` returns the document, so `for (...) { var d = db.c.findOne(...); if (d.x) ... }`
//     works as it did in the legacy mongo shell.
//   - Reads go through __host.find / aggregate / count / distinct (the driver's own calls); writes
//     are built here as server commands (insert, update, delete, findAndModify, createIndexes ...)
//     and answered in the shape the Node driver returned.
//   - Every eval runs in a fresh scope: `with (sandbox) { eval(statement) }` inside one function,
//     so variables live on between the statements of a script and not beyond it.
//   - A statement whose value is a cursor is not drained here: the result carries the host's
//     cursor id and the C++ side takes the first page straight from BSON.
//
// The script evaluates to the object `__shell` is set to (see JsShellBackend.cpp):
//   { configure, setBatchSize, use, checkSyntax, evalScript }.

(function () {
  'use strict';

  const bson = __bson;
  const host = __host;

  const state = { dbName: 'test', batchSize: 50, startupScript: null };
  let output = [];       // what print() wrote during the statement that is running

  // ---------- helpers ----------

  const isObject = (v) => v !== null && typeof v === 'object';
  const isPlain = (v) => isObject(v) && !Array.isArray(v) && !v._bsontype && !(v instanceof Date) && !(v instanceof RegExp);

  function omit(obj, keys) {
    const out = {};
    for (const k of Object.keys(obj || {})) {
      if (keys.indexOf(k) < 0) out[k] = obj[k];
    }
    return out;
  }

  function pick(obj, keys) {
    const out = {};
    if (!isObject(obj)) return out;
    for (const k of keys) {
      if (obj[k] !== undefined) out[k] = obj[k];
    }
    return out;
  }

  // Marks the value of a call with the result type the grid shows it as (stats() -> 'collectionStats').
  function typed(value, resultType) {
    if (isObject(value))
      Object.defineProperty(value, '_ragoType', { value: resultType, enumerable: false, configurable: true });
    return value;
  }

  function error(name, message, code) {
    const e = new Error(message);
    e.name = name;
    if (code !== undefined) e.code = code;
    return e;
  }

  const command = (db, cmd) => host.runCommand(db, cmd);

  // ---------- Extended JSON text (print, printjson, EJSON) ----------

  const hex2 = (n) => (n < 16 ? '0' : '') + n.toString(16);

  function regexOptions(flags) {
    let o = '';
    for (const f of ['i', 'm', 's', 'u']) {
      if (flags.indexOf(f) >= 0) o += f;
    }
    return o;
  }

  // The relaxed Extended JSON form of a value, as a plain tree JSON.stringify can write.
  function ejsonTree(v, relaxed) {
    switch (typeof v) {
      case 'undefined': case 'function': case 'symbol':
        return null;
      case 'number':
        if (Number.isFinite(v)) return v;
        return { $numberDouble: v !== v ? 'NaN' : (v > 0 ? 'Infinity' : '-Infinity') };
      case 'string': case 'boolean':
        return v;
    }
    if (v === null) return null;
    if (Array.isArray(v)) return v.map((x) => ejsonTree(x, relaxed));
    if (v instanceof Date) {
      const ms = v.getTime();
      if (ms !== ms) return null;
      const year = v.getUTCFullYear();
      // Like the Node shell: no '.000' for a date on a whole second.
      if (relaxed && year >= 1970 && year <= 9999) return { $date: v.toISOString().replace(/\.000Z$/, 'Z') };
      return { $date: { $numberLong: String(ms) } };
    }
    if (v instanceof RegExp)
      return { $regularExpression: { pattern: v.source, options: regexOptions(v.flags) } };
    switch (v._bsontype) {
      case 'ObjectId': return { $oid: v.hex };
      case 'Long': {
        const n = Number(v.value);
        return relaxed && Number.isSafeInteger(n) ? n : { $numberLong: v.value };
      }
      case 'Int32': return relaxed ? v.value : { $numberInt: String(v.value) };
      case 'Double': return relaxed && Number.isFinite(v.value) ? v.value : { $numberDouble: String(v.value) };
      case 'Decimal128': return { $numberDecimal: v.value };
      case 'Timestamp': return { $timestamp: { t: v.t, i: v.i } };
      case 'Binary': return { $binary: { base64: v.base64, subType: hex2(v.subtype) } };
      case 'BsonRegExp': return { $regularExpression: { pattern: v.pattern, options: v.options } };
      case 'MinKey': return { $minKey: 1 };
      case 'MaxKey': return { $maxKey: 1 };
      case 'Code': {
        const out = { $code: v.code };
        if (v.scope !== null && v.scope !== undefined) out.$scope = ejsonTree(v.scope, relaxed);
        return out;
      }
      case 'BsonSymbol': return { $symbol: v.value };
      case 'DBPointer': return { $dbPointer: { $ref: v.ns, $id: ejsonTree(v.id, relaxed) } };
    }
    const out = {};
    for (const k of Object.keys(v)) {
      const x = v[k];
      if (typeof x === 'function') continue;
      out[k] = ejsonTree(x, relaxed);
    }
    return out;
  }

  // Extended JSON text -> values with the BSON classes (EJSON.parse).
  function fromEjsonTree(v) {
    if (Array.isArray(v)) return v.map(fromEjsonTree);
    if (!isObject(v)) return v;
    const keys = Object.keys(v);
    if (keys.length >= 1 && keys[0].charAt(0) === '$') {
      switch (keys[0]) {
        case '$oid': return new bson.ObjectId(v.$oid);
        case '$date': {
          const d = v.$date;
          if (isObject(d)) return new Date(Number(d.$numberLong));
          return new Date(d);
        }
        case '$numberLong': return new bson.Long(v.$numberLong);
        case '$numberInt': return Number(v.$numberInt);
        case '$numberDouble': return Number(v.$numberDouble);
        case '$numberDecimal': return new bson.Decimal128(v.$numberDecimal);
        case '$timestamp': return new bson.Timestamp(v.$timestamp.t, v.$timestamp.i);
        case '$binary': return new bson.Binary(parseInt(v.$binary.subType, 16), v.$binary.base64);
        case '$regularExpression': {
          const r = v.$regularExpression;
          try { return new RegExp(r.pattern, r.options); } catch (e) { return new bson.BsonRegExp(r.pattern, r.options); }
        }
        case '$minKey': return new bson.MinKey();
        case '$maxKey': return new bson.MaxKey();
        case '$code': return new bson.Code(v.$code, v.$scope === undefined ? undefined : fromEjsonTree(v.$scope));
        case '$symbol': return new bson.BsonSymbol(v.$symbol);
        case '$undefined': return undefined;
      }
    }
    const out = {};
    for (const k of keys) out[k] = fromEjsonTree(v[k]);
    return out;
  }

  function jsonify(v) {
    try {
      // One line, as the Node shell printed it (its EJSON.stringify dropped the indent argument).
      const text = JSON.stringify(ejsonTree(v, true));
      return text === undefined ? String(v) : text;
    } catch (e) {
      return String(v);
    }
  }

  const EJSON = {
    stringify(value, options, indent) {
      if (typeof options === 'number' || typeof options === 'string') { indent = options; options = {}; }
      const relaxed = !(options && options.relaxed === false);
      return JSON.stringify(ejsonTree(value, relaxed), null, indent);
    },
    parse(text) { return fromEjsonTree(JSON.parse(text)); },
    serialize(value, options) { return ejsonTree(value, !(options && options.relaxed === false)); },
    deserialize(tree) { return fromEjsonTree(tree); },
  };

  // ---------- write results, in the shape the Node driver returned ----------

  function throwWriteErrors(reply) {
    if (reply.writeErrors && reply.writeErrors.length > 0) {
      const w = reply.writeErrors[0];
      const e = error('MongoServerError', w.errmsg, w.code);
      e.writeErrors = reply.writeErrors;
      throw e;
    }
    if (reply.writeConcernError) {
      const w = reply.writeConcernError;
      throw error('MongoWriteConcernError', w.errmsg, w.code);
    }
  }

  const writeOptions = (o) => pick(o, ['writeConcern', 'bypassDocumentValidation', 'comment', 'let']);

  function withId(doc) {
    if (!isPlain(doc)) throw error('MongoInvalidArgumentError', 'Document must be a valid JavaScript object');
    if (doc._id === undefined || doc._id === null) doc._id = new bson.ObjectId();
    return doc;
  }

  function insertDocuments(dbName, collName, docs, o) {
    const ordered = !(o && o.ordered === false);
    const chunk = 1000;
    for (let i = 0; i < docs.length; i += chunk) {
      const reply = command(dbName, Object.assign({
        insert: collName, documents: docs.slice(i, i + chunk), ordered: ordered }, writeOptions(o)));
      throwWriteErrors(reply);
    }
  }

  function insertOne(dbName, collName, d, o) {
    const doc = withId(d);
    insertDocuments(dbName, collName, [doc], o);
    return { acknowledged: true, insertedId: doc._id };
  }

  function insertMany(dbName, collName, docs, o) {
    if (!Array.isArray(docs)) throw error('MongoInvalidArgumentError', 'Argument "docs" must be an array of documents');
    const prepared = docs.map(withId);
    insertDocuments(dbName, collName, prepared, o);
    const ids = {};
    prepared.forEach((d, i) => { ids[String(i)] = d._id; });
    return { acknowledged: true, insertedCount: prepared.length, insertedIds: ids };
  }

  const hasOperators = (u) => Object.keys(u).some((k) => k.charAt(0) === '$');

  function runUpdate(dbName, collName, q, u, multi, o) {
    const spec = Object.assign({ q: q || {}, u: u, multi: multi, upsert: !!(o && o.upsert) },
      pick(o, ['arrayFilters', 'collation', 'hint']));
    const reply = command(dbName, Object.assign({ update: collName, updates: [spec], ordered: true }, writeOptions(o)));
    throwWriteErrors(reply);
    const upserted = reply.upserted || [];
    return {
      acknowledged: true,
      modifiedCount: reply.nModified || 0,
      upsertedId: upserted.length > 0 ? upserted[0]._id : null,
      upsertedCount: upserted.length,
      matchedCount: reply.n - upserted.length,
    };
  }

  function updateWithOperators(dbName, collName, q, u, multi, o) {
    if (isPlain(u) && !hasOperators(u))
      throw error('MongoInvalidArgumentError', 'Update document requires atomic operators');
    return runUpdate(dbName, collName, q, u, multi, o);
  }

  function replaceOne(dbName, collName, q, d, o) {
    if (isPlain(d) && hasOperators(d))
      throw error('MongoInvalidArgumentError', 'Replacement document must not contain atomic operators');
    return runUpdate(dbName, collName, q, d, false, o);
  }

  function runDelete(dbName, collName, q, one, o) {
    const spec = Object.assign({ q: q || {}, limit: one ? 1 : 0 }, pick(o, ['collation', 'hint']));
    const reply = command(dbName, Object.assign({ delete: collName, deletes: [spec], ordered: true }, writeOptions(o)));
    throwWriteErrors(reply);
    return { acknowledged: true, deletedCount: reply.n };
  }

  // mongo shell spells "return the modified document" as returnNewDocument;
  // the driver only understands returnDocument: 'after'.
  function normalizeFindAndModifyOpts(o) {
    const opts = Object.assign({}, o || {});
    if ('returnNewDocument' in opts) {
      if (opts.returnDocument === undefined)
        opts.returnDocument = opts.returnNewDocument ? 'after' : 'before';
      delete opts.returnNewDocument;
    }
    return opts;
  }

  function runFindAndModify(dbName, collName, q, change, o) {
    const opts = normalizeFindAndModifyOpts(o);
    const cmd = Object.assign({ findAndModify: collName, query: q || {} }, change);
    if (opts.sort) cmd.sort = opts.sort;
    if (opts.projection) cmd.fields = opts.projection;
    if (opts.upsert !== undefined) cmd.upsert = !!opts.upsert;
    if (opts.returnDocument === 'after') cmd.new = true;
    Object.assign(cmd, pick(opts, ['arrayFilters', 'collation', 'hint', 'maxTimeMS']), writeOptions(opts));
    return command(dbName, cmd).value;
  }

  // bulkWrite: consecutive operations of one kind go to the server as one command.
  function bulkWrite(dbName, collName, ops, o) {
    if (!Array.isArray(ops) || ops.length === 0)
      throw error('MongoInvalidArgumentError', 'Invalid BulkOperation, Batch cannot be empty');
    const ordered = !(o && o.ordered === false);
    const result = { insertedCount: 0, matchedCount: 0, modifiedCount: 0,
                     deletedCount: 0, upsertedCount: 0, upsertedIds: {}, insertedIds: {} };
    let failure = null;

    function kindOf(op) {
      const k = Object.keys(op)[0];
      if (['insertOne', 'updateOne', 'updateMany', 'replaceOne', 'deleteOne', 'deleteMany'].indexOf(k) < 0)
        throw error('MongoInvalidArgumentError', 'bulkWrite does not know the operation ' + k);
      return k === 'updateOne' || k === 'updateMany' || k === 'replaceOne' ? 'update'
           : k === 'deleteOne' || k === 'deleteMany' ? 'delete' : 'insert';
    }

    let i = 0;
    while (i < ops.length) {
      const kind = kindOf(ops[i]);
      let j = i;
      while (j < ops.length && kindOf(ops[j]) === kind) j++;
      const group = ops.slice(i, j);
      let cmd;
      if (kind === 'insert') {
        cmd = { insert: collName, documents: group.map((op, n) => {
          const d = withId(op.insertOne.document || op.insertOne);
          result.insertedIds[String(i + n)] = d._id;
          return d;
        }) };
      } else if (kind === 'update') {
        cmd = { update: collName, updates: group.map((op) => {
          const key = Object.keys(op)[0];
          const s = op[key];
          return Object.assign({
            q: s.filter || {},
            u: key === 'replaceOne' ? s.replacement : s.update,
            multi: key === 'updateMany',
            upsert: !!s.upsert,
          }, pick(s, ['arrayFilters', 'collation', 'hint']));
        }) };
      } else {
        cmd = { delete: collName, deletes: group.map((op) => {
          const key = Object.keys(op)[0];
          const s = op[key];
          return Object.assign({ q: s.filter || {}, limit: key === 'deleteOne' ? 1 : 0 }, pick(s, ['collation', 'hint']));
        }) };
      }
      cmd.ordered = ordered;
      Object.assign(cmd, writeOptions(o));
      const reply = command(dbName, cmd);
      if (kind === 'insert') {
        result.insertedCount += reply.n - (reply.writeErrors ? reply.writeErrors.length : 0);
      } else if (kind === 'update') {
        const upserted = reply.upserted || [];
        result.upsertedCount += upserted.length;
        upserted.forEach((u) => { result.upsertedIds[String(i + u.index)] = u._id; });
        result.matchedCount += reply.n - upserted.length;
        result.modifiedCount += reply.nModified || 0;
      } else {
        result.deletedCount += reply.n;
      }
      if (reply.writeErrors && reply.writeErrors.length > 0) {
        if (!failure) failure = reply;
        if (ordered) break;
      }
      i = j;
    }
    if (failure) {
      const w = failure.writeErrors[0];
      const e = error('MongoBulkWriteError', w.errmsg, w.code);
      e.result = result;
      e.writeErrors = failure.writeErrors;
      throw e;
    }
    return result;
  }

  // ---------- indexes ----------

  function indexName(keys) {
    return Object.keys(keys).map((k) => k + '_' + keys[k]).join('_');
  }

  function createIndexes(dbName, collName, specs) {
    const indexes = specs.map((s) => {
      const spec = Object.assign({}, s);
      if (spec.name === undefined) spec.name = indexName(spec.key);
      return spec;
    });
    command(dbName, { createIndexes: collName, indexes: indexes });
    return indexes.map((s) => s.name);
  }

