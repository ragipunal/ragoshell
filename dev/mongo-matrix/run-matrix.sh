#!/usr/bin/env bash
# RagoShell live MongoDB compatibility matrix: every connection type on every MongoDB version.
#
# For each version the same services are started from ${MONGO_IMAGE} (docker-compose.yml) and the real app is run
# with --matrix-selftest=<suite> (src/ragoshell-app/MatrixSelfTest*.cpp), which drives QmlBackend, ShellController,
# ServerMonitorController and SshTunnelWorker against them.
#
# Usage:
#   dev/mongo-matrix/run-matrix.sh                            # all versions x all suites
#   dev/mongo-matrix/run-matrix.sh --versions 4.0,9.0          # some versions
#   dev/mongo-matrix/run-matrix.sh --suites version,ssh        # some suites
#   dev/mongo-matrix/run-matrix.sh --exe /path/to/ragoshell    # app binary (else RAGO_EXE, else the build dirs)
#   dev/mongo-matrix/run-matrix.sh --keep-up                   # leave the last containers running (debugging)
#
# Suites: version  standalone --auth + allowTLS: SCRAM-SHA-256/SHA-1, authSource, restricted user, TLS, X.509,
#                  user management, collections, indexes, shell, monitor, export/import
#         noauth   standalone without authentication
#         mtls     requireTLS + mandatory client certificate, encrypted client key, X.509
#         sharded  config RS + shard RS + mongos (TLS, SCRAM, X.509, user management, listShards)
#         ssh      SSH tunnel: password, Ed25519, encrypted Ed25519, RSA + TLS, SSH + mTLS + X.509, SSH + mongos
#         rs       3-member replica set: discovery from a secondary seed, secondaryPreferred + TLS, restricted user
#
# Writes a Markdown report to dev/mongo-matrix/.results/ (git-ignored) and exits non-zero when any suite fails.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/.fixtures"
RESULTS_DIR="${SCRIPT_DIR}/.results"
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

# Every MongoDB release line with a Docker Official Image, oldest supported first.
ALL_VERSIONS="4.0,4.2,4.4,5.0,6.0,7.0,8.0,8.2,8.3,9.0"
ALL_SUITES="version,noauth,mtls,sharded,ssh,rs"
VERSIONS_CSV="${ALL_VERSIONS}"
SUITES_CSV="${ALL_SUITES}"
EXE="${RAGO_EXE:-}"
KEEP_UP=0
SUITE_TIMEOUT="${RAGO_MATRIX_SUITE_TIMEOUT:-900}"

while [ $# -gt 0 ]; do
    case "$1" in
        --versions) VERSIONS_CSV="${2:?--versions needs a list, e.g. 4.0,8.0,9.0}"; shift 2 ;;
        --suites)   SUITES_CSV="${2:?--suites needs a list, e.g. version,ssh}"; shift 2 ;;
        --exe)      EXE="${2:?--exe needs a path}"; shift 2 ;;
        --keep-up)  KEEP_UP=1; shift ;;
        # kept for older notes: --core used to mean "topologies on mongo:7 only"
        --core)     VERSIONS_CSV="8.0"; shift ;;
        -h|--help)  sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "[matrix] unknown option: $1" >&2; exit 2 ;;
    esac
done

# --- the app binary -----------------------------------------------------------------------------------------------
if [ -z "${EXE}" ]; then
    for candidate in \
        "${REPO_ROOT}/build/linux-release/src/ragoshell-app/ragoshell" \
        "${REPO_ROOT}/build/linux-debug/src/ragoshell-app/ragoshell" \
        "${REPO_ROOT}/build/macos-release/src/ragoshell-app/RagoShell.app/Contents/MacOS/RagoShell" \
        "${REPO_ROOT}/build/macos-debug/src/ragoshell-app/RagoShell.app/Contents/MacOS/RagoShell" \
        "${REPO_ROOT}/dist/ragoshell-"*"-windows-x64/ragoshell.exe" \
        "${REPO_ROOT}/build/windows/src/ragoshell-app/Release/ragoshell.exe"; do
        if [ -x "${candidate}" ]; then EXE="${candidate}"; break; fi
    done
fi
if [ -z "${EXE}" ] || [ ! -x "${EXE}" ]; then
    echo "[matrix] ERROR: no RagoShell binary found; build one or pass --exe / RAGO_EXE" >&2
    exit 1
fi

# headless: "offscreen" on Linux/macOS; a windeployqt bundle ships only the "windows" plugin, which needs no display
case "${EXE}" in
    *.exe) export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-windows}" ;;
    *)     export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" ;;
esac
export RAGOSHELL_KEYSTORE="${RAGOSHELL_KEYSTORE:-file}"
# the SSH suite's main target: the authenticated standalone of the version under test
export RAGO_SSH_TARGET_HOST="${RAGO_SSH_TARGET_HOST:-mongo-std}"
if [ -z "${QT_PLUGIN_PATH:-}" ] && [ -d "${HOME}/Qt/6.12.0/macos/plugins" ]; then
    export QT_PLUGIN_PATH="${HOME}/Qt/6.12.0/macos/plugins"
fi

# The app gets the fixtures path inside its spec; a Windows binary needs a Windows path.
FIXTURES_ARG="$(native_path "${FIXTURES_DIR}")"

HOST_ARCH="$(uname -m)"
TIMEOUT_CMD=""
command -v timeout >/dev/null 2>&1 && TIMEOUT_CMD="timeout ${SUITE_TIMEOUT}"

dc() { docker compose -p "${PROJECT}" -f "${COMPOSE_FILE}" "$@"; }

all_down() {
    dc --profile standalone --profile rs --profile sharded --profile ssh down -v --remove-orphans >/dev/null 2>&1 || true
}
cleanup() {
    if [ "${KEEP_UP}" -eq 1 ]; then
        echo "[matrix] --keep-up: containers of project '${PROJECT}' left running"
    else
        all_down
    fi
}
trap cleanup EXIT

wait_port() { # host port seconds
    local i
    for i in $(seq 1 $(( $3 * 2 ))); do
        (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

has() { case ",$1," in *",$2,"*) return 0 ;; esac; return 1; }

mkdir -p "${RESULTS_DIR}/logs"
STAMP="$(date -u +%Y%m%d-%H%M%S)"
REPORT="${RESULTS_DIR}/matrix-${STAMP}.md"
declare -A RESULT

# openssl and ssh-keygen are native programs on Windows: let MSYS convert their path arguments
if ! env -u MSYS_NO_PATHCONV "${SCRIPT_DIR}/generate-fixtures.sh" "${FIXTURES_DIR}" >"${RESULTS_DIR}/logs/${STAMP}-fixtures.log" 2>&1         || [ ! -s "${FIXTURES_DIR}/client_subject.txt" ]; then
    echo "[matrix] ERROR: generating certificates and keys failed (log: ${RESULTS_DIR}/logs/${STAMP}-fixtures.log)" >&2
    exit 1
fi
all_down

TOTAL=0
FAILED=0
run_suite() { # version suite spec
    local ver="$1" suite="$2" spec="$3"
    local log="${RESULTS_DIR}/logs/${STAMP}-${ver}-${suite}.log"
    TOTAL=$((TOTAL + 1))
    echo "[matrix] ${ver} ${suite}: --matrix-selftest=${spec}"
    if ${TIMEOUT_CMD} "${EXE}" "--matrix-selftest=${spec}" >"${log}" 2>&1; then
        RESULT["${ver}|${suite}"]="ok"
        echo "[matrix] ${ver} ${suite}: OK"
    else
        RESULT["${ver}|${suite}"]="FAIL"
        FAILED=$((FAILED + 1))
        echo "[matrix] ${ver} ${suite}: FAILED (log: ${log})" >&2
        grep -E "FAIL|not ok|error" "${log}" | head -15 >&2 || true
    fi
}
mark_all() { # version suites-csv status
    local s
    IFS=',' read -r -a _ss <<< "$2"
    for s in "${_ss[@]}"; do
        if has "${SUITES_CSV}" "${s}"; then RESULT["$1|${s}"]="$3"; TOTAL=$((TOTAL + 1)); FAILED=$((FAILED + 1)); fi
    done
}

IFS=',' read -r -a VERSIONS <<< "${VERSIONS_CSV}"
for ver in "${VERSIONS[@]}"; do
    [ -n "${ver}" ] || continue
    export MONGO_IMAGE="${MONGO_IMAGE_OVERRIDE:-mongo:${ver}}"
    # 4.0 and 4.2 images exist for amd64 only; newer ones run natively on arm64 hosts
    export MONGO_PLATFORM=linux/amd64
    case "${HOST_ARCH}" in
        arm64|aarch64) case "${ver}" in 4.0|4.2) ;; *) export MONGO_PLATFORM=linux/arm64 ;; esac ;;
    esac
    echo ""
    echo "==================================================================="
    echo "[matrix] MongoDB ${ver} (${MONGO_IMAGE}, ${MONGO_PLATFORM})"
    echo "==================================================================="

    # --- group 1: standalone servers, sharded cluster and the SSH bastion together (the SSH suite reaches
    #     mongo-noauth, mongo-std, mongo-mtls and mongos through the bastion) ---------------------------------
    need_std=0; need_sharded=0; need_ssh=0
    for s in version noauth mtls ssh; do has "${SUITES_CSV}" "$s" && need_std=1; done
    for s in sharded ssh; do has "${SUITES_CSV}" "$s" && need_sharded=1; done
    has "${SUITES_CSV}" ssh && need_ssh=1
    if [ $((need_std + need_sharded + need_ssh)) -gt 0 ]; then
        profiles=()
        [ "${need_std}" -eq 1 ] && profiles+=(--profile standalone)
        [ "${need_sharded}" -eq 1 ] && profiles+=(--profile sharded)
        [ "${need_ssh}" -eq 1 ] && profiles+=(--profile ssh)
        if ! dc "${profiles[@]}" up -d --quiet-pull >"${RESULTS_DIR}/logs/${STAMP}-${ver}-up.log" 2>&1; then
            echo "[matrix] ${ver}: containers did not start (log: ${RESULTS_DIR}/logs/${STAMP}-${ver}-up.log)" >&2
            mark_all "${ver}" "version,noauth,mtls,sharded,ssh" "FAIL (start)"
        else
            boot_ok=1
            if [ "${need_std}" -eq 1 ]; then
                "${SCRIPT_DIR}/bootstrap-service.sh" standalone mongo-std 27017 >/dev/null || boot_ok=0
                "${SCRIPT_DIR}/bootstrap-service.sh" mtls mongo-mtls 27017 >/dev/null || boot_ok=0
            fi
            if [ "${need_sharded}" -eq 1 ]; then
                "${SCRIPT_DIR}/bootstrap-service.sh" sharded >/dev/null || boot_ok=0
            fi
            if [ "${boot_ok}" -eq 0 ]; then
                echo "[matrix] ${ver}: bootstrap failed" >&2
                dc "${profiles[@]}" logs --tail 40 >"${RESULTS_DIR}/logs/${STAMP}-${ver}-bootstrap.log" 2>&1 || true
                mark_all "${ver}" "version,noauth,mtls,sharded,ssh" "FAIL (bootstrap)"
            else
                has "${SUITES_CSV}" version && run_suite "${ver}" version "version:37040:${ver}:${FIXTURES_ARG}"
                has "${SUITES_CSV}" noauth  && run_suite "${ver}" noauth "noauth:37039:${FIXTURES_ARG}"
                has "${SUITES_CSV}" mtls    && run_suite "${ver}" mtls "mtls:37090:${FIXTURES_ARG}"
                has "${SUITES_CSV}" sharded && run_suite "${ver}" sharded "sharded:37217:${FIXTURES_ARG}"
                if has "${SUITES_CSV}" ssh; then
                    if wait_port 127.0.0.1 32222 60; then
                        run_suite "${ver}" ssh "ssh:32222:${FIXTURES_ARG}"
                    else
                        echo "[matrix] ${ver}: ssh-bastion did not open 127.0.0.1:32222" >&2
                        mark_all "${ver}" "ssh" "FAIL (bastion)"
                    fi
                fi
            fi
        fi
        if [ "${KEEP_UP}" -eq 0 ] || has "${SUITES_CSV}" rs; then all_down; fi
    fi

    # --- group 2: replica set (needs its own ports and a fresh election) -------------------------------------
    if has "${SUITES_CSV}" rs; then
        if ! dc --profile rs up -d --quiet-pull >"${RESULTS_DIR}/logs/${STAMP}-${ver}-rs-up.log" 2>&1; then
            mark_all "${ver}" "rs" "FAIL (start)"
        elif ! "${SCRIPT_DIR}/bootstrap-service.sh" rs >/dev/null; then
            dc --profile rs logs --tail 40 >"${RESULTS_DIR}/logs/${STAMP}-${ver}-rs-bootstrap.log" 2>&1 || true
            mark_all "${ver}" "rs" "FAIL (bootstrap)"
        else
            run_suite "${ver}" rs "rs:37117,37118,37119:${FIXTURES_ARG}"
        fi
        [ "${KEEP_UP}" -eq 1 ] || all_down
    fi
done

# --- report ---------------------------------------------------------------------------------------------------------
IFS=',' read -r -a SUITES <<< "${SUITES_CSV}"
{
    echo "# RagoShell MongoDB matrix — ${STAMP} UTC"
    echo ""
    echo "- App: \`${EXE}\` (repository $(git -C "$(native_path "${REPO_ROOT}")" describe --always --dirty 2>/dev/null || echo unknown))"
    echo "- Host: $(uname -srm)"
    echo "- Logs: \`dev/mongo-matrix/.results/logs/${STAMP}-*\`"
    echo ""
    printf "| MongoDB |"; for s in "${SUITES[@]}"; do printf " %s |" "$s"; done; echo ""
    printf "|---|"; for s in "${SUITES[@]}"; do printf -- "---|"; done; echo ""
    for ver in "${VERSIONS[@]}"; do
        [ -n "${ver}" ] || continue
        printf "| %s |" "${ver}"
        for s in "${SUITES[@]}"; do printf " %s |" "${RESULT["${ver}|${s}"]:--}"; done
        echo ""
    done
    echo ""
    echo "Suites run: ${TOTAL}, failed: ${FAILED}"
} > "${REPORT}"

echo ""
cat "${REPORT}"
echo ""
echo "[matrix] report: ${REPORT}"
[ "${FAILED}" -eq 0 ]
