#!/usr/bin/env bash
# Bootstraps users, roles, X.509 principals, Replica Set election, or Sharded Cluster
# routing inside a running rago-test-matrix service container.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/.fixtures"
PROJECT="rago-test-matrix"
# Git Bash on Windows: docker, git and the app are native programs that need C:/... paths, and MSYS must not
# rewrite the container paths (/fixtures/...) given to docker exec.
native_path() { printf '%s' "$1"; }
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        export MSYS_NO_PATHCONV=1
        native_path() { cygpath -m "$1"; }
        ;;
esac
COMPOSE_FILE="$(native_path "${SCRIPT_DIR}/docker-compose.yml")"

MODE="${1:?Usage: bootstrap-service.sh <standalone|mtls|rs|sharded> [service] [port]}"
SERVICE="${2:-mongo-std}"
INTERNAL_PORT="${3:-27017}"

CLIENT_SUBJECT="$(cat "${FIXTURES_DIR}/client_subject.txt")"

dc_exec() {
    local svc="$1"
    shift
    docker compose -p "${PROJECT}" -f "${COMPOSE_FILE}" exec -T "${svc}" "$@"
}

run_mongo_js() {
    local svc="$1"
    local port="$2"
    local tls_mode="$3" # "none" or "mtls"
    local js="$4"

    dc_exec "${svc}" sh -ec '
        PORT="$1"
        TLS_MODE="$2"
        JS_CODE="$3"
        if command -v mongosh >/dev/null 2>&1; then
            if [ "$TLS_MODE" = "mtls" ]; then
                mongosh --quiet \
                    --tls --tlsCAFile /fixtures/ca.crt --tlsCertificateKeyFile /fixtures/client.pem \
                    "mongodb://127.0.0.1:${PORT}/admin" --eval "$JS_CODE"
            else
                mongosh --quiet "mongodb://127.0.0.1:${PORT}/admin" --eval "$JS_CODE"
            fi
        else
            if [ "$TLS_MODE" = "mtls" ]; then
                # the legacy shell (OpenSSL 1.0.2 on 4.0) does not match IP SANs; "localhost" is a DNS SAN
                mongo --quiet \
                    --ssl --sslCAFile /fixtures/ca.crt --sslPEMKeyFile /fixtures/client.pem \
                    --host localhost --port "$PORT" admin --eval "$JS_CODE"
            else
                mongo --quiet --host 127.0.0.1 --port "$PORT" admin --eval "$JS_CODE"
            fi
        fi
    ' -- "${port}" "${tls_mode}" "${js}"
}

wait_for_ping() {
    local svc="$1"
    local port="$2"
    local tls_mode="$3"
    for i in $(seq 1 60); do
        if run_mongo_js "${svc}" "${port}" "${tls_mode}" "db.runCommand({ping: 1}).ok" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "[bootstrap] ERROR: Timed out waiting for ${svc}:${port} to accept connections." >&2
    return 1
}

seed_users_and_data() {
    local svc="$1"
    local port="$2"
    local tls_mode="$3"

    local seed_js
    seed_js=$(cat <<EOF
const adminDb = db.getSiblingDB("admin");
let authed = false;
try {
    authed = !!adminDb.auth("rago_root", "ragoRootSecret2026");
} catch (e) {
    authed = false;
}
if (!authed) {
    try {
        adminDb.createUser({
            user: "rago_root",
            pwd: "ragoRootSecret2026",
            roles: [ { role: "root", db: "admin" } ]
        });
    } catch (e) {
        if (String(e).indexOf("already exists") < 0 &&
            String(e).indexOf("not authorized") < 0 &&
            String(e).indexOf("requires authentication") < 0) {
            throw e;
        }
    }
    if (!adminDb.auth("rago_root", "ragoRootSecret2026")) {
        throw new Error("Failed to authenticate as rago_root");
    }
}

function upsertUser(targetDbName, spec) {
    const tdb = db.getSiblingDB(targetDbName);
    try { tdb.dropUser(spec.user); } catch (e) {}
    tdb.createUser(spec);
}

upsertUser("admin", {
    user: "rago_sha256",
    pwd: "ragoSha256Pass!",
    mechanisms: ["SCRAM-SHA-256"],
    roles: [
        { role: "readWriteAnyDatabase", db: "admin" },
        { role: "userAdminAnyDatabase", db: "admin" },
        { role: "dbAdminAnyDatabase", db: "admin" },
        { role: "clusterMonitor", db: "admin" }
    ]
});

upsertUser("admin", {
    user: "rago_sha1",
    pwd: "ragoSha1Pass!",
    mechanisms: ["SCRAM-SHA-1"],
    roles: [
        { role: "readWriteAnyDatabase", db: "admin" },
        { role: "clusterMonitor", db: "admin" }
    ]
});

upsertUser("appdb", {
    user: "rago_app_owner",
    pwd: "ragoAppPass!",
    roles: [ { role: "dbOwner", db: "appdb" } ]
});

upsertUser("tenant_a", {
    user: "rago_restricted",
    pwd: "ragoRestrictedPass!",
    roles: [ { role: "readWrite", db: "tenant_a" } ]
});

const extDb = db.getSiblingDB("\$external");
try { extDb.dropUser("${CLIENT_SUBJECT}"); } catch (e) {}
extDb.createUser({
    user: "${CLIENT_SUBJECT}",
    roles: [
        { role: "readWrite", db: "appdb" },
        { role: "userAdminAnyDatabase", db: "admin" },
        { role: "clusterMonitor", db: "admin" }
    ]
});

const appDb = db.getSiblingDB("appdb");
appDb.items.deleteMany({});
appDb.items.insertMany([
    { _id: 1, sku: "A-100", title: "Alpha Widget", qty: 42, tags: ["featured", "active"] },
    { _id: 2, sku: "B-200", title: "Beta Gadget", qty: 17, tags: ["active"] },
    { _id: 3, sku: "C-300", title: "Gamma Sensor", qty: 99, tags: ["iot", "featured"] }
]);

const tenantDb = db.getSiblingDB("tenant_a");
tenantDb.orders.deleteMany({});
tenantDb.orders.insertMany([
    { _id: 101, customer: "Ada", total: 125.50, status: "paid" },
    { _id: 102, customer: "Grace", total: 240.00, status: "shipped" }
]);

const secretDb = db.getSiblingDB("tenant_secret");
secretDb.records.deleteMany({});
secretDb.records.insertOne({ _id: 999, classified: true });

print("BOOTSTRAP_OK:" + db.version());
EOF
)
    run_mongo_js "${svc}" "${port}" "${tls_mode}" "${seed_js}"
}

case "${MODE}" in
    standalone)
        wait_for_ping "${SERVICE}" "${INTERNAL_PORT}" "none"
        seed_users_and_data "${SERVICE}" "${INTERNAL_PORT}" "none"
        ;;
    mtls)
        wait_for_ping "${SERVICE}" "${INTERNAL_PORT}" "mtls"
        seed_users_and_data "${SERVICE}" "${INTERNAL_PORT}" "mtls"
        ;;
    rs)
        wait_for_ping "mongo-rs1" "37117" "none"
        wait_for_ping "mongo-rs1" "37118" "none"
        wait_for_ping "mongo-rs1" "37119" "none"
        run_mongo_js "mongo-rs1" "37117" "none" '
            try {
                rs.initiate({
                    _id: "rs0",
                    members: [
                        { _id: 0, host: "127.0.0.1:37117", priority: 3 },
                        { _id: 1, host: "127.0.0.1:37118", priority: 1 },
                        { _id: 2, host: "127.0.0.1:37119", priority: 1 }
                    ]
                });
            } catch (e) {
                if (String(e).indexOf("already initialized") < 0) throw e;
            }
        ' >/dev/null
        for i in $(seq 1 60); do
            state="$(run_mongo_js "mongo-rs1" "37117" "none" '
                print((function(){ var h = db.runCommand({hello: 1}); if (!h.ok) { h = db.runCommand({isMaster: 1}); return h.ismaster; } return h.isWritablePrimary; })() ? "PRIMARY" : "WAIT");
            ' 2>/dev/null || true)"
            if printf '%s' "${state}" | grep -q "PRIMARY"; then
                break
            fi
            sleep 0.5
        done
        seed_users_and_data "mongo-rs1" "37117" "none"
        # The suite opens shells directly on secondaries: wait until every member has replicated the users.
        # A write acknowledged by all 3 members implies all earlier oplog entries reached them too.
        run_mongo_js "mongo-rs1" "37117" "none" '
            const adminDb = db.getSiblingDB("admin");
            adminDb.auth("rago_root", "ragoRootSecret2026");
            adminDb.matrix_sync.insertOne({ at: new Date() }, { writeConcern: { w: 3, wtimeout: 60000 } });
        ' >/dev/null
        ;;
    sharded)
        wait_for_ping "mongo-cfg" "27019" "none"
        run_mongo_js "mongo-cfg" "27019" "none" '
            try {
                rs.initiate({
                    _id: "cfg0",
                    configsvr: true,
                    members: [ { _id: 0, host: "mongo-cfg:27019" } ]
                });
            } catch (e) {
                if (String(e).indexOf("already initialized") < 0) throw e;
            }
        ' >/dev/null
        wait_for_ping "mongo-shard" "27018" "none"
        run_mongo_js "mongo-shard" "27018" "none" '
            try {
                rs.initiate({
                    _id: "shard0",
                    members: [ { _id: 0, host: "mongo-shard:27018" } ]
                });
            } catch (e) {
                if (String(e).indexOf("already initialized") < 0) throw e;
            }
        ' >/dev/null
        for i in $(seq 1 60); do
            cstate="$(run_mongo_js "mongo-cfg" "27019" "none" 'print((function(){ var h = db.runCommand({hello: 1}); if (!h.ok) { h = db.runCommand({isMaster: 1}); return h.ismaster; } return h.isWritablePrimary; })() ? "PRIMARY" : "WAIT")' 2>/dev/null || true)"
            sstate="$(run_mongo_js "mongo-shard" "27018" "none" 'print((function(){ var h = db.runCommand({hello: 1}); if (!h.ok) { h = db.runCommand({isMaster: 1}); return h.ismaster; } return h.isWritablePrimary; })() ? "PRIMARY" : "WAIT")' 2>/dev/null || true)"
            if printf '%s' "${cstate}" | grep -q "PRIMARY" && printf '%s' "${sstate}" | grep -q "PRIMARY"; then
                break
            fi
            sleep 0.5
        done
        wait_for_ping "mongos" "27017" "none"
        run_mongo_js "mongos" "27017" "none" '
            const adminDb = db.getSiblingDB("admin");
            try {
                adminDb.createUser({
                    user: "rago_root",
                    pwd: "ragoRootSecret2026",
                    roles: [ { role: "root", db: "admin" } ]
                });
            } catch (e) {
                if (String(e).indexOf("already exists") < 0 && String(e).indexOf("not authorized") < 0) throw e;
            }
            adminDb.auth("rago_root", "ragoRootSecret2026");
            const shards = adminDb.runCommand({ listShards: 1 }).shards || [];
            if (shards.length === 0) {
                sh.addShard("shard0/mongo-shard:27018");
            }
        ' >/dev/null
        seed_users_and_data "mongos" "27017" "none"
        run_mongo_js "mongos" "27017" "none" '
            const adminDb = db.getSiblingDB("admin");
            adminDb.auth("rago_root", "ragoRootSecret2026");
            sh.enableSharding("appdb");
            print("SHARDED_OK");
        '
        ;;
    *)
        echo "Unknown mode: ${MODE}" >&2
        exit 2
        ;;
esac
