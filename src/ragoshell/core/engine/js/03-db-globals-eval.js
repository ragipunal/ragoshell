  // ---------- database ----------

  function serverHost() { return host.serverAddress(); }

  function makeDb(dbName) {
    const base = {
      getName() { return dbName; },
      toString() { return dbName; },
      getSiblingDB(other) { return makeDb(String(other)); },
      getCollection(n) { return collectionProxy(dbName, String(n)); },
      getMongo() {
        return {
          host: serverHost(),
          getDBNames: () => (command('admin', { listDatabases: 1, nameOnly: true }).databases || []).map((d) => d.name),
          getDBs: () => command('admin', { listDatabases: 1 }),
          setSlaveOk: () => 'slaveOk',
          setReadPref: () => {},
        };
      },
      runCommand(c) { return command(dbName, c); },
      adminCommand(c) { return command('admin', c); },
      getCollectionNames() { return host.listCollections(dbName, {}).map((c) => c.name).sort(); },
      getCollectionInfos(filter, opts) { return host.listCollections(dbName, filter || {}); },
      createCollection(name, opts) {
        command(dbName, Object.assign({ create: String(name) }, opts || {}));
        return { ok: 1 };
      },
      createView(viewName, viewOn, pipeline, opts) {
        return command(dbName, Object.assign({
          create: String(viewName),
          viewOn: String(viewOn),
          pipeline: Array.isArray(pipeline) ? pipeline : (pipeline ? [pipeline] : []),
        }, opts || {}));
      },
      connPoolStats() { return command('admin', { connPoolStats: 1 }); },
      getFeatureCompatibilityVersion() {
        return command('admin', { getParameter: 1, featureCompatibilityVersion: 1 });
      },
      dropDatabase() {
        const r = command(dbName, { dropDatabase: 1 });
        return { dropped: dbName, ok: r.ok ? 1 : 0 };
      },
      repairDatabase() { return command(dbName, { repairDatabase: 1 }); },
      stats(scale) {
        const cmd = (scale && typeof scale === 'object')
          ? Object.assign({ dbStats: 1 }, scale)
          : (scale ? { dbStats: 1, scale: scale } : { dbStats: 1 });
        return command(dbName, cmd);
      },
      printCollectionStats(scale) {
        const names = host.listCollections(dbName, {}).map((c) => c.name).filter((n) => !n.startsWith('system.')).sort();
        const out = [];
        const extra = normalizeCollStatsArg(scale);
        for (const n of names) {
          try {
            out.push(command(dbName, Object.assign({ collStats: n }, extra)));
          } catch (e) {
            out.push({ ns: dbName + '.' + n, ok: 0, errmsg: e.message });
          }
        }
        return typed(out, 'collectionStats');
      },
      serverStatus(opts) { return command('admin', Object.assign({ serverStatus: 1 }, opts || {})); },
      hostInfo() { return command('admin', { hostInfo: 1 }); },
      version() { return command('admin', { buildInfo: 1 }).version; },
      serverBuildInfo() { return command('admin', { buildInfo: 1 }); },
      listCommands() { return command(dbName, { listCommands: 1 }); },
      currentOp(arg) {
        const extra = (arg === true) ? { $all: true } : (arg && typeof arg === 'object' ? arg : {});
        return command('admin', Object.assign({ currentOp: 1 }, extra));
      },
      killOp(opId) { return command('admin', { killOp: 1, op: opId }); },
      getUsers(opts) {
        const extra = (opts && typeof opts === 'object') ? opts : {};
        return command(dbName, Object.assign({ usersInfo: 1 }, extra)).users || [];
      },
      getUser(username, opts) {
        const extra = (opts && typeof opts === 'object') ? opts : {};
        const r = command(dbName, Object.assign({ usersInfo: String(username) }, extra));
        return r.users && r.users.length > 0 ? r.users[0] : null;
      },
      createUser(userDoc) {
        if (!userDoc || !userDoc.user)
          throw new Error('createUser requires a user document with a "user" field');
        const rest = omit(userDoc, ['user', 'pwd', 'roles', 'customData', 'mechanisms', 'digestPassword']);
        const cmd = Object.assign({ createUser: userDoc.user }, rest);
        if (userDoc.pwd !== undefined) cmd.pwd = userDoc.pwd;
        cmd.roles = userDoc.roles || [];
        if (userDoc.customData !== undefined) cmd.customData = userDoc.customData;
        if (userDoc.mechanisms !== undefined) cmd.mechanisms = userDoc.mechanisms;
        if (userDoc.digestPassword !== undefined) cmd.digestPassword = userDoc.digestPassword;
        return command(dbName, cmd);
      },
      updateUser(username, userDoc) {
        const d = userDoc || {};
        const cmd = Object.assign({ updateUser: String(username) }, omit(d, ['pwd', 'roles', 'customData', 'mechanisms']));
        if (d.pwd !== undefined) cmd.pwd = d.pwd;
        if (d.roles !== undefined) cmd.roles = d.roles;
        if (d.customData !== undefined) cmd.customData = d.customData;
        if (d.mechanisms !== undefined) cmd.mechanisms = d.mechanisms;
        return command(dbName, cmd);
      },
      changeUserPassword(username, pwd) { return command(dbName, { updateUser: String(username), pwd: pwd }); },
      dropUser(username) { return command(dbName, { dropUser: String(username) }); },
      dropAllUsers() { return command(dbName, { dropAllUsersFromDatabase: 1 }); },
      grantRolesToUser(username, roles) { return command(dbName, { grantRolesToUser: String(username), roles: roles }); },
      revokeRolesFromUser(username, roles) { return command(dbName, { revokeRolesFromUser: String(username), roles: roles }); },
      getRoles(opts) {
        const extra = (opts && typeof opts === 'object') ? opts : {};
        return command(dbName, Object.assign({ rolesInfo: 1 }, extra)).roles || [];
      },
      getProfilingStatus() { return command(dbName, { profile: -1 }); },
      setProfilingLevel(level, slowms) {
        const cmd = { profile: level };
        if (typeof slowms === 'number') cmd.slowms = slowms;
        else if (slowms && typeof slowms === 'object') Object.assign(cmd, slowms);
        return command(dbName, cmd);
      },
      isMaster() { return command(dbName, { isMaster: 1 }); },
      hello() { return command(dbName, { hello: 1 }); },
      system: new Proxy({}, {
        get(t, prop) { return collectionProxy(dbName, 'system.' + String(prop)); },
      }),
    };
    return new Proxy(base, {
      get(t, prop) {
        if (prop in t) return t[prop];
        if (typeof prop === 'string' && !prop.startsWith('_'))
          return collectionProxy(dbName, prop);
        return undefined;
      },
    });
  }

  // ---------- shell helpers ----------

  function shellHelpers(ctx) {
    ctx.shellHelper = (cmd, rawArg) => {
      const arg = String(rawArg || '').trim();
      if (cmd === 'show') {
        if (arg === 'dbs' || arg === 'databases') {
          const r = command('admin', { listDatabases: 1 });
          return r.databases || r;
        }
        if (arg === 'collections' || arg === 'tables')
          return host.listCollections(state.dbName, {});
        if (arg === 'users')
          return ctx.db.getUsers();
        if (arg === 'roles')
          return ctx.db.getRoles();
        if (arg === 'profile')
          return new QueryCursor(state.dbName, 'system.profile', {}, undefined, { sort: { ts: -1 }, limit: 20 }).toArray();
        if (arg === 'logs')
          return command('admin', { getLog: '*' });
        if (arg === 'log' || arg.startsWith('log ')) {
          const parts = arg.split(/\s+/);
          return command('admin', { getLog: parts[1] || 'global' });
        }
      }
      if (cmd === 'use' && arg.length > 0) {
        state.dbName = arg;
        ctx.db = makeDb(arg);
        return 'switched to db ' + arg;
      }
      return 'unsupported command: ' + cmd + ' ' + arg;
    };
    ctx.use = (name) => {
      state.dbName = String(name);
      ctx.db = makeDb(state.dbName);
      return 'switched to db ' + state.dbName;
    };
    ctx.rs = {
      slaveOk() { return 'slaveOk'; },
      secondaryOk() { return 'secondaryOk'; },
      status() { return command('admin', { replSetGetStatus: 1 }); },
      conf() { const r = command('admin', { replSetGetConfig: 1 }); return r.config || r; },
      config() { return ctx.rs.conf(); },
      isMaster() { return command('admin', { isMaster: 1 }); },
      hello() { return command('admin', { hello: 1 }); },
    };
    ctx.sh = {
      status() {
        const shards = command('admin', { listShards: 1 });
        let balancer = null;
        try { balancer = command('admin', { balancerStatus: 1 }); } catch (e) { /* no balancer info */ }
        return balancer ? { shards: shards.shards, balancer: balancer } : shards;
      },
      getBalancerState() { return command('admin', { balancerStatus: 1 }).mode !== 'off'; },
    };
  }

  // ---------- BSON constructors, mongo-shell compatible names ----------

  // Plain functions, so that both ObjectId() and new ObjectId() work; the prototype is the class's, so
  // `x instanceof ObjectId` holds.
  function constructorFor(cls, make) {
    const fn = function () { return make.apply(null, arguments); };
    fn.prototype = cls.prototype;
    return fn;
  }

  function uuidHex(s) {
    if (s === undefined) {
      const raw = host.randomHex(16);
      return raw.slice(0, 12) + '4' + raw.slice(13, 16) + ['8', '9', 'a', 'b'][parseInt(raw.charAt(16), 16) & 3] + raw.slice(17);
    }
    const hex = String(s).replace(/-/g, '');
    if (!/^[0-9a-fA-F]{32}$/.test(hex)) throw new Error('UUID needs 32 hex digits, got: ' + String(s));
    return hex;
  }

  function installConstructors(ctx) {
    ctx.ObjectId = constructorFor(bson.ObjectId, (s) => (s === undefined ? new bson.ObjectId() : new bson.ObjectId(s)));
    ctx.ObjectID = ctx.ObjectId;
    ctx.ISODate = (s) => (s === undefined ? new Date() : new Date(s));
    ctx.NumberLong = constructorFor(bson.Long, (v) => new bson.Long(v));
    ctx.NumberInt = (v) => v | 0;
    ctx.NumberDecimal = constructorFor(bson.Decimal128, (v) => new bson.Decimal128(v));
    ctx.Timestamp = constructorFor(bson.Timestamp, (t, i) => new bson.Timestamp(t, i));
    ctx.UUID = (s) => new bson.Binary(4, host.hexToBase64(uuidHex(s)));
    ctx.BinData = (sub, b64) => new bson.Binary(sub, b64);
    ctx.HexData = (sub, hex) => new bson.Binary(sub, host.hexToBase64(String(hex)));
    ctx.MinKey = constructorFor(bson.MinKey, () => new bson.MinKey());
    ctx.MaxKey = constructorFor(bson.MaxKey, () => new bson.MaxKey());
    ctx.DBRef = (coll, id, db) => (db ? { $ref: coll, $id: id, $db: db } : { $ref: coll, $id: id });
    ctx.EJSON = EJSON;
  }

  function makeSandbox() {
    const sandbox = Object.create(null);
    shellHelpers(sandbox);
    sandbox.db = makeDb(state.dbName);
    sandbox.print = function () {
      output.push(Array.prototype.map.call(arguments, (x) => (typeof x === 'string' ? x : jsonify(x))).join(' '));
    };
    sandbox.printjson = (x) => { output.push(jsonify(x)); };
    sandbox.printjsononeline = (x) => { output.push(JSON.stringify(ejsonTree(x, true))); };
    sandbox.tojson = (x) => jsonify(x);
    sandbox.console = { log: sandbox.print, info: sandbox.print, warn: sandbox.print, error: sandbox.print };
    installConstructors(sandbox);
    return sandbox;
  }

  // ---------- statements ----------

  // Syntax check of a piece of source (the same job vm.Script did): '' when it compiles.
  function checkSyntax(src) {
    try {
      new Function(src);
      return '';
    } catch (e) {
      return (e.name || 'Error') + ': ' + (e.message || e);
    }
  }

  const compiles = (src) => checkSyntax(src) === '';

  // Split a multi-statement script at top-level ';' (and top-level newlines when
  // both sides compile as standalone JS statements) while respecting strings,
  // template literals, comments, and bracket depth.
  function splitTopLevelStatements(src) {
    const candidates = [];
    let start = 0;
    let depthParen = 0, depthBracket = 0, depthBrace = 0;
    let inSingle = false, inDouble = false, inTemplate = false;
    let inLineComment = false, inBlockComment = false;

    for (let i = 0; i < src.length; i++) {
      const ch = src[i];
      const next = src[i + 1];

      if (inLineComment) {
        if (ch === '\n') inLineComment = false;
        else continue;
      }
      if (inBlockComment) {
        if (ch === '*' && next === '/') { inBlockComment = false; i++; }
        continue;
      }
      if (inSingle) {
        if (ch === '\\') { i++; continue; }
        if (ch === '\'') inSingle = false;
        continue;
      }
      if (inDouble) {
        if (ch === '\\') { i++; continue; }
        if (ch === '"') inDouble = false;
        continue;
      }
      if (inTemplate) {
        if (ch === '\\') { i++; continue; }
        if (ch === '`') inTemplate = false;
        continue;
      }

      if (ch === '/' && next === '/') { inLineComment = true; i++; continue; }
      if (ch === '/' && next === '*') { inBlockComment = true; i++; continue; }
      if (ch === '\'') { inSingle = true; continue; }
      if (ch === '"') { inDouble = true; continue; }
      if (ch === '`') { inTemplate = true; continue; }

      if (ch === '(') depthParen++;
      else if (ch === ')' && depthParen > 0) depthParen--;
      else if (ch === '[') depthBracket++;
      else if (ch === ']' && depthBracket > 0) depthBracket--;
      else if (ch === '{') depthBrace++;
      else if (ch === '}' && depthBrace > 0) depthBrace--;
      else if (depthParen === 0 && depthBracket === 0 && depthBrace === 0) {
        if (ch === ';') {
          const piece = src.slice(start, i + 1).trim();
          if (piece && piece !== ';') candidates.push(piece);
          start = i + 1;
        } else if (ch === '\n') {
          const piece = src.slice(start, i).trim();
          const rest = src.slice(i + 1).trim();
          if (piece && rest &&
              !/[.,+\-*/%=?:&|(\\[]$/.test(piece) &&
              !/^(\.|\(|\[|\?|:|else\b|catch\b|finally\b|while\b)/.test(rest)) {
            if (compiles(piece) && compiles(rest)) {
              candidates.push(piece);
              start = i + 1;
            }
          }
        }
      }
    }
    const tail = src.slice(start).trim();
    if (tail && tail !== ';') candidates.push(tail);
    if (candidates.length <= 1) return [src];

    // Validate every chunk compiles cleanly; if any fails (e.g. regex literal edge
    // case), fall back to single-script evaluation so nothing ever breaks.
    for (const c of candidates) {
      if (!compiles(c)) return [src];
    }
    return candidates;
  }

  // Every statement is evaluated by its own eval(), and let / const / class of an eval are private to
  // it. A script written statement by statement ("const a = ...;" then "printjson(a)") needs them
  // to live on, as they did in the Node vm context, so a leading let / const / class becomes a var.
  const DECLARATION = /^((?:\s|\/\/[^\n]*\n|\/\*[\s\S]*?\*\/)*)(?:const|let)\s+(?=[\w$[{])/;
  const CLASS_DECLARATION = /^((?:\s|\/\/[^\n]*\n|\/\*[\s\S]*?\*\/)*)class\s+([A-Za-z_$][\w$]*)/;

  function lifted(stmt) {
    if (DECLARATION.test(stmt)) return stmt.replace(DECLARATION, '$1var ');
    if (CLASS_DECLARATION.test(stmt)) return stmt.replace(CLASS_DECLARATION, '$1var $2 = class $2');
    return stmt;
  }

  // Translate shell commands ('show dbs', 'show log global', 'use x', 'set y') into calls.
  function translateShellCommands(script) {
    return String(script).replace(
      /^\s*(show|use|set)\s+([^\r\n;]+?)\s*;?\s*$/gm,
      (_, cmd, arg) => 'shellHelper(' + JSON.stringify(cmd) + ', ' + JSON.stringify(arg.trim()) + ');');
  }

  const rowsOf = (val) =>
    Array.isArray(val) && val.length > 0 && val.every((x) => isObject(x) && !Array.isArray(x)) ? val
    : (isObject(val) && !Array.isArray(val) ? [val] : []);

  // The result item of a statement's value, or null when the statement produced nothing to show.
  function materializeValue(val, pageBatchSize, stmtSource) {
    const base = { dbName: state.dbName, server: serverHost(), statement: stmtSource || '' };

    if (val instanceof QueryCursor) {
      const effectiveBatch = val._batchSize > 0 ? val._batchSize : pageBatchSize;
      const cursorId = val._open(effectiveBatch);
      return Object.assign({
        type: 'query',
        output: output.join('\n'),
        cursorId: cursorId,
        pageSize: effectiveBatch,
        queryInfo: {
          db: val._dbName,
          coll: val._collName,
          query: val._query || {},
          fields: val._fields || null,
          limit: val._limit,
          skip: val._skip,
          batchSize: effectiveBatch,
        },
      }, base);
    }

    if (val instanceof AggCursor) {
      const cursorId = val._open(pageBatchSize);
      const pipeline = {};
      val._pipeline.forEach((stage, i) => { pipeline[String(i)] = stage; });
      return Object.assign({
        type: 'aggregate',
        output: output.join('\n'),
        cursorId: cursorId,
        pageSize: pageBatchSize,
        aggrInfo: {
          db: val._dbName,
          coll: val._collName,
          pipeline: pipeline,
          options: val._options || {},
        },
      }, base);
    }

    let documents = [];
    let resultType = 'custom';
    if (val !== undefined) {
      output.push(jsonify(val));
      documents = rowsOf(val);
      if (val && val._ragoType) resultType = val._ragoType;
    }
    if (documents.length > 0)
      return Object.assign({ type: resultType, output: output.join('\n'), documents: documents }, base);
    if (output.length > 0)
      return Object.assign({ type: 'text', output: output.join('\n'), documents: [] }, base);
    return null;
  }

  // JavaScript newer than Qt's engine runs (snippets pasted from mongosh or the web): the engine's own message
  // ("Unexpected token `...'", "Property 'replaceAll' of object abc is not a function") does not say why, so a
  // hint is added. Only what was measured to be missing is listed; ?. ?? and array spread work.
  function unsupportedFeature(e, stmt) {
    const name = e && e.name;
    const message = String(e && e.message !== undefined ? e.message : e);
    const text = String(stmt);
    if (name === 'SyntaxError') {
      if (/\bawait\b|\basync\b/.test(text)) return 'async / await';
      if (/\b\d+n\b/.test(text)) return 'BigInt literals (10n)';
      if (/\?\?=|\|\|=|&&=/.test(text)) return 'the logical assignment operators (??=, ||=, &&=)';
      if (/\bcatch\s*\{/.test(text)) return 'a catch clause without a variable (catch { })';
      if (/\{[^{}]*\.\.\.\s*[\w$({\[]/.test(text)) return 'object spread and rest ({ ...x })';
      if (/\bclass\b[^{]*\{[^}]*\b\w+\s*=[^=]/.test(text)) return 'class fields';
    } else if (name === 'TypeError') {
      const m = /Property '(replaceAll|flat|at|fromEntries)'/.exec(message);
      if (m) return m[1] === 'fromEntries' ? 'Object.fromEntries' : 'the ' + m[1] + ' method';
    } else if (name === 'ReferenceError') {
      const m = /^(globalThis|setTimeout|setInterval|clearTimeout|clearInterval) is not defined/.exec(message);
      if (m) return m[1];
    }
    return null;
  }

  function errorItem(e, fallbackName, stmt) {
    let output = (e && e.name ? e.name : fallbackName) + ': ' + (e && e.message !== undefined ? e.message : e);
    const feature = unsupportedFeature(e, stmt);
    if (feature) {
      output += '\nHint: ' + feature + ' is newer than the JavaScript of this shell (Qt\'s engine; the shell follows the'
        + ' mongo shell, not mongosh). Write it in older JavaScript, for example Object.assign({}, x) for { ...x }.';
    }
    return {
      type: 'error',
      output: output,
      documents: [],
      dbName: state.dbName,
      server: serverHost(),
      statement: stmt,
    };
  }

  // Runs the script: the startup script first, then every statement in the same scope. `begin` and
  // `end` are called around each statement by the evaluator below.
  function evalScript(script, dbName, pageBatchSize) {
    host.closeAllCursors();
    if (dbName) state.dbName = dbName;
    const pageSize = pageBatchSize > 0 ? pageBatchSize : state.batchSize;
    const sandbox = makeSandbox();

    const startup = state.startupScript ? splitTopLevelStatements(state.startupScript).map(lifted) : [];
    const statements = splitTopLevelStatements(translateShellCommands(script));
    const results = [];

    function begin() { output = []; }
    function end(stmt, val, failed, err) {
      if (failed) {
        results.push(errorItem(err, 'Error', stmt));
        return false;
      }
      try {
        const item = materializeValue(val, pageSize, stmt);
        if (item) results.push(item);
      } catch (e) {
        results.push(errorItem(e, 'MongoServerError', stmt));
        return false;
      }
      return true;
    }

    // One function, one scope: the vars of the statements live on from one to the next, and nothing
    // outlives the call. Names are long so that user variables do not collide with them.
    const evaluator = new Function('__rago_sandbox', '__rago_startup', '__rago_statements', '__rago_begin', '__rago_end', '__rago_lift',
      'with (__rago_sandbox) {' +
      '  for (var __rago_i = 0; __rago_i < __rago_startup.length; __rago_i++) {' +
      '    try { eval(__rago_startup[__rago_i]); } catch (__rago_e) { }' +
      '  }' +
      '  for (var __rago_k = 0; __rago_k < __rago_statements.length; __rago_k++) {' +
      '    var __rago_stmt = __rago_statements[__rago_k], __rago_val, __rago_failed = false, __rago_err = null;' +
      '    __rago_begin();' +
      '    try { __rago_val = eval(__rago_lift(__rago_stmt)); } catch (__rago_e) { __rago_failed = true; __rago_err = __rago_e; }' +
      '    if (__rago_end(__rago_stmt, __rago_val, __rago_failed, __rago_err) === false) break;' +
      '  }' +
      '}');
    evaluator(sandbox, startup, statements, begin, end, lifted);

    if (results.length === 0) {
      const empty = { type: 'text', output: '', documents: [], dbName: state.dbName, server: serverHost() };
      results.push(empty);
    }
    return { results: results, dbName: state.dbName };
  }

