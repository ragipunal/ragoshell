#!/usr/bin/env node
// End-to-end test of ragoshell-mcp over its real stdio transport against a
// local MongoDB (e.g. Docker mongo:4.0 on localhost:27017).
//
//   node src/ragoshell-mcp/tests/e2e_stdio.js <path/to/ragoshell-mcp[.exe]> [host] [port]
//
// Uses an isolated settings directory (RAGOSHELL_CONFIG_DIR), never the
// user's ~/.ragoshell. Seeds and drops the database "rago_mcp_stdio_e2e".
// Exit code 0 = all checks passed.

'use strict';

const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const readline = require('readline');

let MongoClient;
try {
    ({ MongoClient } = require('mongodb'));
} catch (_) {
    console.error('error: npm package "mongodb" is required to run e2e_stdio.js (run `npm install mongodb` in a scratch folder and set NODE_PATH).');
    process.exit(1);
}

const exe = process.argv[2];
const host = process.argv[3] || 'localhost';
const port = Number(process.argv[4] || 27017);
const DB = 'rago_mcp_stdio_e2e';
const CONNECTION_UUID = '{0e2e0000-0000-4000-8000-00000000e2e0}';

if (!exe || !fs.existsSync(exe)) {
    console.error('usage: node e2e_stdio.js <path/to/ragoshell-mcp> [host] [port]');
    process.exit(1);
}

let failures = 0;
function check(name, ok, detail) {
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${ok || detail === undefined ? '' : `  -- ${detail}`}`);
    if (!ok)
        failures++;
}

/** Minimal MCP client: newline-delimited JSON-RPC over the child's stdio. */
function startServer(env) {
    const child = spawn(exe, [], { env, stdio: ['pipe', 'pipe', 'pipe'] });
    const pending = new Map();
    const stray = [];
    let nextId = 1;
    let stderr = '';
    child.stderr.on('data', (d) => { stderr += d; });
    readline.createInterface({ input: child.stdout }).on('line', (line) => {
        let msg;
        try {
            msg = JSON.parse(line);
        } catch (e) {
            stray.push(line);
            return;
        }
        const waiter = pending.get(msg.id);
        if (waiter) {
            pending.delete(msg.id);
            waiter(msg);
        } else {
            stray.push(line);
        }
    });
    return {
        child,
        stray,
        stderr: () => stderr,
        request(method, params) {
            const id = nextId++;
            child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
            return new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error(`timeout waiting for ${method}`)), 20000);
                pending.set(id, (msg) => { clearTimeout(timer); resolve(msg); });
            });
        },
        notify(method, params) {
            child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
        },
        async callTool(name, args) {
            const msg = await this.request('tools/call', { name, arguments: args || {} });
            const result = msg.result || {};
            const text = (result.content && result.content[0] && result.content[0].text) || '';
            return { isError: !!result.isError, text, json: safeParse(text) };
        },
    };
}

function safeParse(text) {
    try {
        return JSON.parse(text);
    } catch (e) {
        return null;
    }
}

async function main() {
    const uri = `mongodb://${host}:${port}/?serverSelectionTimeoutMS=3000`;
    const mongo = new MongoClient(uri);
    try {
        await mongo.connect();
    } catch (e) {
        console.log(`SKIP  MongoDB not reachable at ${host}:${port}: ${e.message}`);
        return 0;
    }
    const db = mongo.db(DB);
    await db.dropDatabase();
    await db.collection('items').insertMany(
        Array.from({ length: 25 }, (_, n) => ({ n, kind: n % 2 ? 'odd' : 'even', at: new Date(2024, 0, n + 1) })));

    const configDir = fs.mkdtempSync(path.join(os.tmpdir(), 'rago-mcp-e2e-'));
    const env = { ...process.env, RAGOSHELL_CONFIG_DIR: configDir };
    fs.writeFileSync(path.join(configDir, 'ragoshell.json'), JSON.stringify({
        version: '2.0',
        connections: [{
            uuid: CONNECTION_UUID,
            connectionName: 'E2E Local',
            serverHost: host,
            serverPort: port,
            credentials: [],
        }],
    }, null, 2));

    try {
        // Off by default: the server refuses to start.
        let r = spawnSync(exe, [], { env, input: '', encoding: 'utf8' });
        check('disabled by default (exit 2)', r.status === 2, `status ${r.status} ${r.stderr}`);

        r = spawnSync(exe, ['--enable', '--allow', 'E2E Local'], { env, encoding: 'utf8' });
        check('--enable --allow', r.status === 0 && r.stdout.includes('[x]'), r.stdout + r.stderr);
        const mcpJsonPath = path.join(configDir, 'mcp.json');
        const mcpJson = JSON.parse(fs.readFileSync(mcpJsonPath, 'utf8'));
        check('mcp.json written in the settings dir', mcpJson.enabled && mcpJson.allowedConnections[0] === CONNECTION_UUID);
        mcpJson.maxDocuments = 10;
        fs.writeFileSync(mcpJsonPath, JSON.stringify(mcpJson));

        const server = startServer(env);
        const init = await server.request('initialize', {
            protocolVersion: '2025-06-18',
            capabilities: {},
            clientInfo: { name: 'rago-e2e', version: '1.0' },
        });
        check('initialize', init.result && init.result.protocolVersion === '2025-06-18' &&
              init.result.capabilities.tools, JSON.stringify(init));
        server.notify('notifications/initialized');

        const list = await server.request('tools/list', {});
        const names = (list.result.tools || []).map((t) => t.name);
        check('tools/list', ['list_connections', 'list_databases', 'list_collections', 'collection_schema',
                             'find', 'aggregate', 'count', 'explain'].every((n) => names.includes(n)), names.join(','));

        let t = await server.callTool('list_connections');
        check('list_connections', !t.isError && t.json.connections.length === 1 &&
              t.json.connections[0].name === 'E2E Local', t.text);

        t = await server.callTool('list_collections', { database: DB });
        check('list_collections', !t.isError && t.json.collections.some((c) => c.name === 'items'), t.text);

        t = await server.callTool('find', { database: DB, collection: 'items', filter: { kind: 'even' },
                                            sort: { n: 1 }, limit: 1000 });
        check('find capped at maxDocuments', !t.isError && t.json.returned === 10 && t.json.hasMore === true &&
              t.json.documents[0].n === 0, t.text.slice(0, 300));
        check('find keeps BSON dates', t.text.includes('"$date"'), t.text.slice(0, 300));

        t = await server.callTool('count', { database: DB, collection: 'items', filter: "{ n: { $gte: 20 } }" });
        check('count', !t.isError && t.json.count === 5, t.text);

        t = await server.callTool('aggregate', { database: DB, collection: 'items',
                                                 pipeline: [{ $group: { _id: '$kind', n: { $sum: 1 } } }] });
        check('aggregate', !t.isError && t.json.returned === 2, t.text);

        t = await server.callTool('aggregate', { database: DB, collection: 'items',
                                                 pipeline: [{ $match: {} }, { $out: 'items_copy' }] });
        check('aggregate rejects $out', t.isError && t.text.includes('$out'), t.text);

        t = await server.callTool('find', { database: 'admin', collection: 'system.users' });
        check('find refuses system.users', t.isError && t.text.includes('credentials'), t.text);

        t = await server.callTool('aggregate', { database: DB, collection: 'items',
                                                 pipeline: [{ $unionWith: { coll: 'system.keys', pipeline: [] } }] });
        check('aggregate refuses $unionWith into system.keys', t.isError && t.text.includes('credentials'), t.text);

        t = await server.callTool('collection_schema', { database: DB, collection: 'items' });
        check('collection_schema', !t.isError && t.json.fields.some((f) => f.path === 'kind'), t.text.slice(0, 300));

        t = await server.callTool('explain', { database: DB, collection: 'items', filter: { n: 3 } });
        check('explain', !t.isError && t.text.includes('COLLSCAN'), t.text.slice(0, 300));

        const unknown = await server.request('tools/call', { name: 'drop_database', arguments: {} });
        check('unknown tool is a protocol error', unknown.error && unknown.error.code === -32602, JSON.stringify(unknown));

        const collections = await db.listCollections().toArray();
        check('nothing was written', !collections.some((c) => c.name === 'items_copy'));
        check('stdout carried only JSON-RPC', server.stray.length === 0, server.stray.join('\n'));

        server.child.stdin.end();
        const code = await new Promise((resolve) => server.child.on('exit', resolve));
        check('clean exit on stdin EOF', code === 0, `exit ${code}; stderr: ${server.stderr()}`);
    } finally {
        await db.dropDatabase();
        await mongo.close();
        fs.rmSync(configDir, { recursive: true, force: true });
    }
    console.log(failures ? `${failures} check(s) failed` : 'all checks passed');
    return failures ? 1 : 0;
}

main().then((code) => process.exit(code), (e) => {
    console.error(e);
    process.exit(1);
});
