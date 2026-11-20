  // ---------- cursors ----------

  const closeCursor = (id) => { if (id !== null && id !== undefined) host.cursorClose(id); };

  // find(): nothing is sent until the cursor is read (toArray, forEach, hasNext/next, count, ...) or
  // until it is the value of a statement. It records the query so the C++ side can rebuild
  // MongoQueryInfo for its paging UI.
  class QueryCursor {
    constructor(db, coll, query, fields, opts) {
      this._dbName = db;
      this._collName = coll;
      this._query = query || {};
      this._fields = fields;
      this._limit = (opts && opts.limit) || 0;
      this._skip = (opts && opts.skip) || 0;
      this._batchSize = (opts && opts.batchSize) || 0;
      this._sort = opts && opts.sort;
      this._hint = opts && opts.hint;
      this._maxTimeMS = opts && opts.maxTimeMS;
      this._collation = opts && opts.collation;
      this._id = null;   // the host cursor, once opened
    }
    sort(s) { this._sort = s; return this; }
    limit(n) { this._limit = Number(n) || 0; return this; }
    skip(n) { this._skip = Number(n) || 0; return this; }
    batchSize(n) { this._batchSize = Number(n) || 0; return this; }
    hint(h) { this._hint = h; return this; }
    maxTimeMS(ms) { this._maxTimeMS = Number(ms) || 0; return this; }
    collation(c) { this._collation = c; return this; }
    projection(p) { this._fields = p; return this; }
    pretty() { return this; }
    readPref() { return this; }
    count(applySkipLimit) {
      const opts = {};
      if (applySkipLimit) {
        if (this._skip) opts.skip = this._skip;
        if (this._limit) opts.limit = this._limit;
      }
      return host.countDocuments(this._dbName, this._collName, this._query, opts);
    }
    size() { return this.count(true); }
    itcount() {
      this._ensureOpen();
      let n = 0;
      try {
        let batch;
        while ((batch = host.cursorNext(this._id, 1000)).length > 0) n += batch.length;
      } finally {
        this.close();
      }
      return n;
    }
    toArray() {
      this._ensureOpen();
      try {
        return host.cursorNext(this._id, 0);
      } finally {
        this.close();
      }
    }
    forEach(fn) {
      this._ensureOpen();
      try {
        let batch;
        while ((batch = host.cursorNext(this._id, 100)).length > 0) {
          for (const doc of batch) fn(doc);
        }
      } finally {
        this.close();
      }
    }
    map(fn) { return this.toArray().map(fn); }
    hasNext() {
      this._ensureOpen();
      return host.cursorHasNext(this._id);
    }
    next() {
      this._ensureOpen();
      const batch = host.cursorNext(this._id, 1);
      if (batch.length === 0) throw error('Error', 'error hasNext: false');
      return batch[0];
    }
    close() { closeCursor(this._id); this._id = null; }
    [Symbol.iterator]() {
      const self = this;
      return { next() { return self.hasNext() ? { value: self.next(), done: false } : { value: undefined, done: true }; } };
    }
    explain(verbosity) {
      const findCmd = { find: this._collName, filter: this._query || {} };
      if (this._fields) findCmd.projection = this._fields;
      if (this._sort) findCmd.sort = this._sort;
      if (this._skip) findCmd.skip = this._skip;
      if (this._limit) findCmd.limit = this._limit;
      if (this._hint) findCmd.hint = this._hint;
      if (this._collation) findCmd.collation = this._collation;
      if (this._maxTimeMS) findCmd.maxTimeMS = this._maxTimeMS;
      return command(this._dbName, { explain: findCmd, verbosity: verbosity || 'queryPlanner' });
    }
    _ensureOpen() { if (this._id === null) this._open(); }
    _open(pageBatchSize) {
      this.close();
      const findOpts = {};
      if (this._fields) findOpts.projection = this._fields;
      if (this._sort) findOpts.sort = this._sort;
      if (this._limit) findOpts.limit = this._limit;
      if (this._skip) findOpts.skip = this._skip;
      const targetBatch = this._batchSize > 0
        ? this._batchSize
        : (pageBatchSize > 0 ? pageBatchSize + 1 : 0);
      if (targetBatch > 0)
        findOpts.batchSize = this._limit > 0 ? Math.min(this._limit, targetBatch) : targetBatch;
      if (this._hint) findOpts.hint = this._hint;
      if (this._maxTimeMS) findOpts.maxTimeMS = this._maxTimeMS;
      if (this._collation) findOpts.collation = this._collation;
      this._id = host.find(this._dbName, this._collName, this._query, findOpts);
      return this._id;
    }
  }

  class AggCursor {
    constructor(db, coll, pipeline, opts) {
      this._dbName = db;
      this._collName = coll;
      this._pipeline = Array.isArray(pipeline) ? pipeline : (pipeline ? [pipeline] : []);
      this._options = opts || {};
      this._id = null;
    }
    pretty() { return this; }
    toArray() {
      this._ensureOpen();
      try {
        return host.cursorNext(this._id, 0);
      } finally {
        this.close();
      }
    }
    forEach(fn) {
      this._ensureOpen();
      try {
        let batch;
        while ((batch = host.cursorNext(this._id, 100)).length > 0) {
          for (const doc of batch) fn(doc);
        }
      } finally {
        this.close();
      }
    }
    map(fn) { return this.toArray().map(fn); }
    itcount() {
      this._ensureOpen();
      let n = 0;
      try {
        let batch;
        while ((batch = host.cursorNext(this._id, 1000)).length > 0) n += batch.length;
      } finally {
        this.close();
      }
      return n;
    }
    hasNext() { this._ensureOpen(); return host.cursorHasNext(this._id); }
    next() {
      this._ensureOpen();
      const batch = host.cursorNext(this._id, 1);
      if (batch.length === 0) throw error('Error', 'error hasNext: false');
      return batch[0];
    }
    close() { closeCursor(this._id); this._id = null; }
    [Symbol.iterator]() {
      const self = this;
      return { next() { return self.hasNext() ? { value: self.next(), done: false } : { value: undefined, done: true }; } };
    }
    explain(verbosity) {
      return command(this._dbName, {
        explain: { aggregate: this._collName, pipeline: this._pipeline, cursor: this._options.cursor || {} },
        verbosity: verbosity || 'queryPlanner',
      });
    }
    _ensureOpen() { if (this._id === null) this._open(); }
    _open(pageBatchSize) {
      this.close();
      const aggOpts = Object.assign({}, omit(this._options, ['cursor']));
      if (this._options.cursor && this._options.cursor.batchSize !== undefined && aggOpts.batchSize === undefined)
        aggOpts.batchSize = this._options.cursor.batchSize;
      if (pageBatchSize > 0 && aggOpts.batchSize === undefined)
        aggOpts.batchSize = pageBatchSize + 1;
      this._id = host.aggregate(this._dbName, this._collName, this._pipeline, aggOpts);
      return this._id;
    }
  }

  function makeExplainableCollection(dbName, collName, verbosity) {
    const v = verbosity || 'queryPlanner';
    return {
      find(q, f) { return new QueryCursor(dbName, collName, q, f, {}).explain(v); },
      findOne(q, f) { return new QueryCursor(dbName, collName, q, f, { limit: 1 }).explain(v); },
      aggregate(p, o) { return new AggCursor(dbName, collName, p, o).explain(v); },
      count(q) {
        return command(dbName, { explain: { count: collName, query: q || {} }, verbosity: v });
      },
      distinct(field, q) {
        return command(dbName, { explain: { distinct: collName, key: field, query: q || {} }, verbosity: v });
      },
      remove(q, justOne) {
        return command(dbName, {
          explain: { delete: collName, deletes: [{ q: q || {}, limit: justOne ? 1 : 0 }] }, verbosity: v });
      },
    };
  }

  function normalizeCollStatsArg(arg) {
    if (typeof arg === 'number') return { scale: arg };
    if (arg && typeof arg === 'object') return arg;
    return {};
  }

  // ---------- collection ----------

  function collectionProxy(dbName, collName) {
    const runCollStats = (arg) => command(dbName, Object.assign({ collStats: collName }, normalizeCollStatsArg(arg)));

    const methods = {
      find(q, f) { return new QueryCursor(dbName, collName, q, f, {}); },
      findOne(q, f) {
        const cursor = new QueryCursor(dbName, collName, q || {}, f, { limit: 1 });
        const docs = cursor.toArray();
        return docs.length > 0 ? docs[0] : null;
      },
      aggregate(p, o) { return new AggCursor(dbName, collName, p, o); },
      count(q, o) { return host.countDocuments(dbName, collName, q || {}, o || {}); },
      countDocuments(q, o) { return host.countDocuments(dbName, collName, q || {}, o || {}); },
      estimatedDocumentCount(o) { return host.estimatedDocumentCount(dbName, collName); },
      distinct(field, q, o) { return host.distinct(dbName, collName, String(field), q || {}, o || {}); },
      insert(docs, o) {
        const arr = Array.isArray(docs) ? docs : [docs];
        return arr.length === 1 ? insertOne(dbName, collName, arr[0], o) : insertMany(dbName, collName, arr, o);
      },
      insertOne(d, o) { return insertOne(dbName, collName, d, o); },
      insertMany(d, o) { return insertMany(dbName, collName, d, o); },
      save(d, o) {
        return (d && d._id !== undefined)
          ? replaceOne(dbName, collName, { _id: d._id }, d, Object.assign({ upsert: true }, o || {}))
          : insertOne(dbName, collName, d, o);
      },
      replaceOne(q, d, o) { return replaceOne(dbName, collName, q, d, o); },
      update(q, u, o) {
        o = o || {};
        return runUpdate(dbName, collName, q, u, !!o.multi, { upsert: !!o.upsert });
      },
      updateOne(q, u, o) { return updateWithOperators(dbName, collName, q, u, false, o); },
      updateMany(q, u, o) { return updateWithOperators(dbName, collName, q, u, true, o); },
      findAndModify(spec) {
        const s = spec || {};
        const cmd = { findAndModify: collName, query: s.query || {} };
        if (s.sort) cmd.sort = s.sort;
        if (s.remove) cmd.remove = true;
        if (s.update) cmd.update = s.update;
        if (s.new !== undefined) cmd.new = !!s.new;
        if (s.fields) cmd.fields = s.fields;
        if (s.upsert !== undefined) cmd.upsert = !!s.upsert;
        return command(dbName, cmd).value;
      },
      findOneAndUpdate(q, u, o) { return runFindAndModify(dbName, collName, q, { update: u }, o); },
      findOneAndReplace(q, r, o) { return runFindAndModify(dbName, collName, q, { update: r }, o); },
      findOneAndDelete(q, o) { return runFindAndModify(dbName, collName, q, { remove: true }, o); },
      remove(q, justOne) {
        const onlyOne = (typeof justOne === 'object' && justOne !== null) ? !!justOne.justOne : !!justOne;
        return runDelete(dbName, collName, q, onlyOne);
      },
      deleteOne(q, o) { return runDelete(dbName, collName, q, true, o); },
      deleteMany(q, o) { return runDelete(dbName, collName, q, false, o); },
      drop() {
        try {
          // MongoDB <= 6.0 throws NamespaceNotFound (code 26); MongoDB 7.0+ (SERVER-43894) returns
          // {ok: 1} without `ns` when nothing was dropped, and includes `ns` when a collection/view existed.
          const reply = command(dbName, { drop: collName });
          return Boolean(reply && (reply.ns !== undefined || reply.nIndexesWas !== undefined));
        } catch (e) {
          if (e && (e.code === 26 || /ns not found/i.test(e.message))) return false;
          throw e;
        }
      },
      renameCollection(target, dropTarget) {
        command('admin', { renameCollection: dbName + '.' + collName, to: dbName + '.' + target, dropTarget: !!dropTarget });
        return { ok: 1 };
      },
      stats(arg) { return typed(runCollStats(arg), 'collectionStats'); },
      storageSize() { return runCollStats().storageSize; },
      totalIndexSize() { return runCollStats().totalIndexSize; },
      dataSize() { return runCollStats().size; },
      totalSize() { const r = runCollStats(); return (r.storageSize || 0) + (r.totalIndexSize || 0); },
      isCapped() { return !!runCollStats().capped; },
      validate(full) {
        const cmd = (full && typeof full === 'object')
          ? Object.assign({ validate: collName }, full)
          : { validate: collName, full: !!full };
        return command(dbName, cmd);
      },
      reIndex() { return command(dbName, { reIndex: collName }); },
      getIndexes() { return host.listIndexes(dbName, collName); },
      getIndexSpecs() { return host.listIndexes(dbName, collName); },
      getIndexKeys() { return host.listIndexes(dbName, collName).map((i) => i.key); },
      createIndex(keys, opts) { return createIndexes(dbName, collName, [Object.assign({ key: keys }, opts || {})])[0]; },
      ensureIndex(keys, opts) { return createIndexes(dbName, collName, [Object.assign({ key: keys }, opts || {})])[0]; },
      createIndexes(keysArr, opts) {
        if (Array.isArray(keysArr) && keysArr.length > 0 && !keysArr[0].key)
          return createIndexes(dbName, collName, keysArr.map((k) => Object.assign({ key: k }, opts || {})));
        return createIndexes(dbName, collName, keysArr);
      },
      dropIndex(indexName) { return command(dbName, { dropIndexes: collName, index: indexName }); },
      dropIndexes(indexNames) {
        const reply = command(dbName, { dropIndexes: collName, index: indexNames || '*' });
        return indexNames ? reply : true;
      },
      hideIndex(indexName) {
        return command(dbName, { collMod: collName, index: { name: String(indexName), hidden: true } });
      },
      unhideIndex(indexName) {
        return command(dbName, { collMod: collName, index: { name: String(indexName), hidden: false } });
      },
      bulkWrite(ops, opts) { return bulkWrite(dbName, collName, ops, opts); },
      compact(opts) {
        return command(dbName, Object.assign({ compact: collName }, (opts && typeof opts === 'object') ? opts : {}));
      },
      convertToCapped(sizeBytes) { return command(dbName, { convertToCapped: collName, size: Number(sizeBytes) }); },
      analyzeShardKey(key, opts) {
        return command('admin', Object.assign({ analyzeShardKey: dbName + '.' + collName, key: key }, opts || {}));
      },
      explain(verbosity) { return makeExplainableCollection(dbName, collName, verbosity); },
      latencyStats(opts) {
        return new AggCursor(dbName, collName, [{ $collStats: { latencyStats: opts || { histograms: true } } }]).toArray();
      },
      getShardDistribution() { return runCollStats(); },
      getShardVersion() { return command('admin', { getShardVersion: dbName + '.' + collName }); },
      getName() { return collName; },
      getFullName() { return dbName + '.' + collName; },
      getDB() { return makeDb(dbName); },
      toString() { return dbName + '.' + collName; },
    };
    return new Proxy(methods, {
      get(t, prop) {
        if (prop in t) return t[prop];
        if (typeof prop === 'symbol') return t[prop];
        // Support dotted sub-collection access like db.foo.bar.find()
        return collectionProxy(dbName, collName + '.' + String(prop));
      },
    });
  }

