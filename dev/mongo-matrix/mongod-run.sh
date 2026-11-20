#!/bin/sh
# Starts mongod or mongos inside a matrix container with the same arguments on every MongoDB version.
#
#   mongod-run.sh mongod|mongos [args ...]
#
# - copies the server certificate and the cluster keyfile out of the read-only /fixtures mount (mongod refuses
#   a keyfile that other users can read, and the mount keeps host permissions);
# - MongoDB 4.0 knows only the old --ssl* option names; when the binary has no --tlsMode, every --tls* option
#   is rewritten to its --ssl* twin (4.2+ accept both, so newer versions get the arguments unchanged);
# - hands over to the image's docker-entrypoint.sh, which drops root before starting the server.
set -eu

bin="${1:?usage: mongod-run.sh mongod|mongos [args ...]}"
shift

if [ -f /fixtures/server.pem ]; then
    cp /fixtures/server.pem /tmp/server.pem
    chmod 600 /tmp/server.pem
fi
if [ -f /fixtures/mongo-keyfile ]; then
    cp /fixtures/mongo-keyfile /tmp/mongo-keyfile
    chmod 400 /tmp/mongo-keyfile
fi
chown mongodb:mongodb /tmp/server.pem /tmp/mongo-keyfile 2>/dev/null || true

legacy_ssl=0
if ! "$bin" --help 2>&1 | grep -q -- '--tlsMode'; then
    legacy_ssl=1
fi

if [ "$legacy_ssl" -eq 1 ]; then
    # rebuild "$@" with the 4.0 option names and mode values
    for arg do
        shift
        case "$arg" in
            --tlsMode) arg=--sslMode ;;
            allowTLS) arg=allowSSL ;;
            preferTLS) arg=preferSSL ;;
            requireTLS) arg=requireSSL ;;
            --tlsCertificateKeyFile) arg=--sslPEMKeyFile ;;
            --tlsCertificateKeyFilePassword) arg=--sslPEMKeyPassword ;;
            --tlsCAFile) arg=--sslCAFile ;;
            --tlsClusterFile) arg=--sslClusterFile ;;
            --tlsAllowConnectionsWithoutCertificates) arg=--sslAllowConnectionsWithoutCertificates ;;
            --tlsAllowInvalidHostnames) arg=--sslAllowInvalidHostnames ;;
            --tlsAllowInvalidCertificates) arg=--sslAllowInvalidCertificates ;;
        esac
        set -- "$@" "$arg"
    done
fi

if command -v docker-entrypoint.sh >/dev/null 2>&1; then
    exec docker-entrypoint.sh "$bin" "$@"
fi
exec "$bin" "$@"
