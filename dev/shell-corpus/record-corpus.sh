#!/usr/bin/env bash
# Deterministic seeder, recorder, and verifier for src/ragoshell-unit-tests/shell-corpus/*.json
# (see docs/ShellCorpusRecorder.md and dev/shell-corpus/README.md).
#
# Never touches dev/mongo-test/ or binds fixed port 27017 in --ephemeral-docker mode.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SEED_JS="${SCRIPT_DIR}/seed-samples.js"
DEFAULT_CORPUS_DIR="${REPO_ROOT}/src/ragoshell-unit-tests/shell-corpus"

find_default_test_bin() {
    local candidates=(
        "${REPO_ROOT}/build/macos-release/src/ragoshell-unit-tests/rago_unit_tests"
        "${REPO_ROOT}/build/macos-debug/src/ragoshell-unit-tests/rago_unit_tests"
        "${REPO_ROOT}/build/linux/src/ragoshell-unit-tests/rago_unit_tests"
        "${REPO_ROOT}/build/linux-release/src/ragoshell-unit-tests/rago_unit_tests"
    )
    local c
    for c in "${candidates[@]}"; do
        if [ -x "${c}" ]; then
            printf '%s\n' "${c}"
            return 0
        fi
    done
    printf '%s\n' "${candidates[0]}"
}

usage() {
    cat <<'EOF'
Usage:
  ./dev/shell-corpus/record-corpus.sh --ephemeral-docker [image] [options]
  ./dev/shell-corpus/record-corpus.sh --mongo <host[:port]> [options]

Targets (one required):
  --ephemeral-docker [image]  Start an isolated tmpfs MongoDB container on 127.0.0.1:<random>
                              (default image: mongo:7), seed `samples`, run, and remove on exit.
  --mongo <host[:port]>       Connect to an existing test MongoDB endpoint. Loopback is required
                              unless --allow-remote is also passed.

Options:
  --seed                      Drop and deterministically re-seed `samples` on --mongo target
                              (always enabled for --ephemeral-docker).
  --allow-remote              Permit non-loopback --mongo endpoints.
  --verify-only               Verify corpus without writing any JSON files.
  --corpus-dir <dir>          Corpus directory to verify in --verify-only mode
                              (default: src/ragoshell-unit-tests/shell-corpus).
  --out <dir>                 Output directory when recording corpus JSON files
                              (default: src/ragoshell-unit-tests/shell-corpus).
  --bin <path>                Path to rago_unit_tests binary.
  -h, --help                  Show this help text.
EOF
}

MODE=""
DOCKER_IMAGE="mongo:7"
MONGO_ENDPOINT=""
DO_SEED=0
ALLOW_REMOTE=0
VERIFY_ONLY=0
CORPUS_DIR="${DEFAULT_CORPUS_DIR}"
OUT_DIR="${DEFAULT_CORPUS_DIR}"
TEST_BIN="${RAGO_UNIT_TESTS_BIN:-$(find_default_test_bin)}"

while [ $# -gt 0 ]; do
    case "$1" in
        --ephemeral-docker)
            MODE="docker"
            if [ $# -ge 2 ] && [[ "$2" != -* ]]; then
                DOCKER_IMAGE="$2"
                shift 2
            else
                shift
            fi
            ;;
        --mongo)
            MODE="mongo"
            MONGO_ENDPOINT="${2:?--mongo requires <host[:port]>}"
            shift 2
            ;;
        --seed)
            DO_SEED=1
            shift
            ;;
        --allow-remote)
            ALLOW_REMOTE=1
            shift
            ;;
        --verify-only)
            VERIFY_ONLY=1
            shift
            ;;
        --corpus-dir)
            CORPUS_DIR="${2:?--corpus-dir requires a directory path}"
            shift 2
            ;;
        --out)
            OUT_DIR="${2:?--out requires a directory path}"
            shift 2
            ;;
        --bin)
            TEST_BIN="${2:?--bin requires path to rago_unit_tests}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[shell-corpus] ERROR: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "${MODE}" ]; then
    echo "[shell-corpus] ERROR: specify either --ephemeral-docker [image] or --mongo <host[:port]>." >&2
    usage >&2
    exit 2
fi

if [ ! -x "${TEST_BIN}" ]; then
    echo "[shell-corpus] ERROR: rago_unit_tests binary not found or not executable: ${TEST_BIN}" >&2
    echo "[shell-corpus] Build it first, e.g.: cmake --build --preset macos-release --target rago_unit_tests" >&2
    exit 1
fi

if [ ! -f "${SEED_JS}" ]; then
    echo "[shell-corpus] ERROR: seed script missing: ${SEED_JS}" >&2
    exit 1
fi

CONTAINER_NAME=""
cleanup() {
    if [ -n "${CONTAINER_NAME}" ]; then
        docker rm -f -v "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

is_loopback_host() {
    local endpoint="$1"
    local host_part=""
    if [[ "${endpoint}" == \[* ]]; then
        host_part="${endpoint%%\]*}]"
    else
        host_part="${endpoint%%:*}"
    fi
    case "${host_part}" in
        localhost|127.0.0.1|"[::1]") return 0 ;;
        *) return 1 ;;
    esac
}

wait_for_container_mongo() {
    local cname="$1"
    local attempt
    for attempt in $(seq 1 60); do
        if docker exec "${cname}" sh -c '
            if command -v mongosh >/dev/null 2>&1; then
                mongosh --quiet --norc --eval "db.adminCommand({ping: 1}).ok"
            else
                mongo --quiet --norc --eval "db.adminCommand({ping: 1}).ok"
            fi
        ' >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

if [ "${MODE}" = "docker" ]; then
    if [[ ! "${DOCKER_IMAGE}" =~ ^[A-Za-z0-9._/:@-]+$ ]]; then
        echo "[shell-corpus] ERROR: invalid Docker image reference: ${DOCKER_IMAGE}" >&2
        exit 2
    fi
    CONTAINER_NAME="rago-shell-corpus-$$-${RANDOM}"
    echo "[shell-corpus] Starting ephemeral container '${CONTAINER_NAME}' (${DOCKER_IMAGE}) on 127.0.0.1:<ephemeral> ..."
    docker run -d --rm \
        --name "${CONTAINER_NAME}" \
        --tmpfs /data/db \
        -p "127.0.0.1::27017" \
        "${DOCKER_IMAGE}" \
        --bind_ip_all >/dev/null

    PORT_LINE="$(docker port "${CONTAINER_NAME}" 27017/tcp | head -n 1)"
    HOST_PORT="${PORT_LINE##*:}"
    if [[ ! "${HOST_PORT}" =~ ^[0-9]+$ ]]; then
        echo "[shell-corpus] ERROR: failed to resolve ephemeral host port from '${PORT_LINE}'" >&2
        exit 1
    fi
    MONGO_ENDPOINT="127.0.0.1:${HOST_PORT}"
    DO_SEED=1

    echo "[shell-corpus] Waiting for MongoDB on ${MONGO_ENDPOINT} ..."
    if ! wait_for_container_mongo "${CONTAINER_NAME}"; then
        echo "[shell-corpus] ERROR: MongoDB in '${CONTAINER_NAME}' did not become ready within 30s" >&2
        docker logs "${CONTAINER_NAME}" >&2 || true
        exit 1
    fi
else
    if [[ ! "${MONGO_ENDPOINT}" =~ ^(\[[0-9a-fA-F:]+\]|[A-Za-z0-9._-]+)(:[0-9]{1,5})?$ ]]; then
        echo "[shell-corpus] ERROR: invalid --mongo endpoint: ${MONGO_ENDPOINT}" >&2
        exit 2
    fi
    if [ "${ALLOW_REMOTE}" -ne 1 ] && ! is_loopback_host "${MONGO_ENDPOINT}"; then
        echo "[shell-corpus] ERROR: refusing non-loopback endpoint '${MONGO_ENDPOINT}' without --allow-remote" >&2
        exit 2
    fi
fi

if [ -z "${QT_PLUGIN_PATH:-}" ] && [ -d "${HOME}/Qt/6.12.0/macos/plugins" ]; then
    export QT_PLUGIN_PATH="${HOME}/Qt/6.12.0/macos/plugins"
fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export RAGOSHELL_KEYSTORE="${RAGOSHELL_KEYSTORE:-file}"

SEED_ENV=""
if [ "${DO_SEED}" -eq 1 ]; then
    SEED_ENV="${SEED_JS}"
    echo "[shell-corpus] Seeding deterministic 'samples' dataset on ${MONGO_ENDPOINT} ..."
fi

if [ "${VERIFY_ONLY}" -eq 1 ]; then
    echo "[shell-corpus] Verifying corpus at '${CORPUS_DIR}' against ${MONGO_ENDPOINT} ..."
    RAGOSHELL_TEST_MONGO="${MONGO_ENDPOINT}" \
    RAGOSHELL_SHELL_CORPUS_DIR="${CORPUS_DIR}" \
    RAGOSHELL_SEED_SHELL_CORPUS="${SEED_ENV}" \
    RAGOSHELL_RECORD_SHELL_CORPUS="" \
        "${TEST_BIN}" --gtest_filter="ShellCorpusCompareTest.*:ShellCorpusTest.*"
    echo "[shell-corpus] Verification succeeded."
else
    mkdir -p "${OUT_DIR}"
    OUT_ABS="$(cd "${OUT_DIR}" && pwd)"
    echo "[shell-corpus] Recording canonicalized corpus into '${OUT_ABS}' from ${MONGO_ENDPOINT} ..."
    RAGOSHELL_TEST_MONGO="${MONGO_ENDPOINT}" \
    RAGOSHELL_SHELL_CORPUS_DIR="${OUT_ABS}" \
    RAGOSHELL_SEED_SHELL_CORPUS="${SEED_ENV}" \
    RAGOSHELL_RECORD_SHELL_CORPUS="${OUT_ABS}" \
        "${TEST_BIN}" --gtest_filter="ShellCorpusTest.*"

    echo "[shell-corpus] Re-verifying recorded corpus in '${OUT_ABS}' ..."
    RAGOSHELL_TEST_MONGO="${MONGO_ENDPOINT}" \
    RAGOSHELL_SHELL_CORPUS_DIR="${OUT_ABS}" \
    RAGOSHELL_SEED_SHELL_CORPUS="" \
    RAGOSHELL_RECORD_SHELL_CORPUS="" \
        "${TEST_BIN}" --gtest_filter="ShellCorpusCompareTest.*:ShellCorpusTest.*"
    echo "[shell-corpus] Recording and round-trip verification succeeded: ${OUT_ABS}"
fi
