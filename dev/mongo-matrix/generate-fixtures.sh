#!/usr/bin/env bash
# Generates ephemeral TLS/mTLS/X.509 certificates, MongoDB replica-set keyfile,
# and SSH host/client keys under dev/mongo-matrix/.fixtures/ (git-ignored).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${1:-${SCRIPT_DIR}/.fixtures}"

mkdir -p "${OUT_DIR}/ssh"
chmod 700 "${OUT_DIR}/ssh"

echo "[fixtures] Generating Root CA, Server TLS, and Client mTLS/X.509 certificates in ${OUT_DIR} ..."

cat > "${OUT_DIR}/openssl-ca.cnf" <<'EOF'
[ req ]
default_bits       = 2048
distinguished_name = req_dn
x509_extensions    = v3_ca
prompt             = no

[ req_dn ]
C  = US
ST = CA
O  = RagoShellTestCA
CN = RagoShell Matrix Root CA

[ v3_ca ]
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints       = critical, CA:true
keyUsage               = critical, digitalSignature, cRLSign, keyCertSign
EOF

openssl req -x509 -nodes -newkey rsa:2048 -days 3650 -sha256 \
    -keyout "${OUT_DIR}/ca.key" \
    -out "${OUT_DIR}/ca.crt" \
    -config "${OUT_DIR}/openssl-ca.cnf" >/dev/null 2>&1

cat > "${OUT_DIR}/openssl-server.cnf" <<'EOF'
[ req ]
default_bits       = 2048
distinguished_name = req_dn
req_extensions     = v3_server
prompt             = no

[ req_dn ]
C  = US
ST = CA
O  = RagoShellServer
OU = Cluster
CN = localhost

[ v3_server ]
basicConstraints       = critical, CA:FALSE
keyUsage               = critical, digitalSignature, keyEncipherment
extendedKeyUsage       = serverAuth, clientAuth
subjectAltName         = @alt_names

[ alt_names ]
DNS.1  = localhost
DNS.2  = mongo-40
DNS.3  = mongo-42
DNS.4  = mongo-44
DNS.5  = mongo-50
DNS.6  = mongo-60
DNS.7  = mongo-70
DNS.8  = mongo-80
DNS.9  = mongo-mtls
DNS.10 = mongo-rs1
DNS.11 = mongo-rs2
DNS.12 = mongo-rs3
DNS.13 = mongo-cfg
DNS.14 = mongo-shard
DNS.15 = mongos
DNS.16 = mongo-std
DNS.17 = mongo-noauth
IP.1   = 127.0.0.1
EOF

openssl req -nodes -newkey rsa:2048 -sha256 \
    -keyout "${OUT_DIR}/server.key" \
    -out "${OUT_DIR}/server.csr" \
    -config "${OUT_DIR}/openssl-server.cnf" >/dev/null 2>&1

openssl x509 -req -days 825 -sha256 \
    -in "${OUT_DIR}/server.csr" \
    -CA "${OUT_DIR}/ca.crt" \
    -CAkey "${OUT_DIR}/ca.key" \
    -CAcreateserial \
    -out "${OUT_DIR}/server.crt" \
    -extensions v3_server \
    -extfile "${OUT_DIR}/openssl-server.cnf" >/dev/null 2>&1

cat "${OUT_DIR}/server.crt" "${OUT_DIR}/server.key" > "${OUT_DIR}/server.pem"
chmod 600 "${OUT_DIR}/server.key" "${OUT_DIR}/server.pem"
chmod 644 "${OUT_DIR}/ca.crt" "${OUT_DIR}/server.crt"

# Client X.509 / mTLS certificate (distinct O/OU from server so MongoDB treats it as $external user, not __system)
cat > "${OUT_DIR}/openssl-client.cnf" <<'EOF'
[ req ]
default_bits       = 2048
distinguished_name = req_dn
req_extensions     = v3_client
prompt             = no

[ req_dn ]
C  = US
ST = CA
O  = RagoShellClient
OU = AppUsers
CN = rago-x509-client

[ v3_client ]
basicConstraints = critical, CA:FALSE
keyUsage         = critical, digitalSignature, keyEncipherment
extendedKeyUsage = clientAuth
EOF

openssl req -nodes -newkey rsa:2048 -sha256 \
    -keyout "${OUT_DIR}/client.key" \
    -out "${OUT_DIR}/client.csr" \
    -config "${OUT_DIR}/openssl-client.cnf" >/dev/null 2>&1

openssl x509 -req -days 825 -sha256 \
    -in "${OUT_DIR}/client.csr" \
    -CA "${OUT_DIR}/ca.crt" \
    -CAkey "${OUT_DIR}/ca.key" \
    -CAcreateserial \
    -out "${OUT_DIR}/client.crt" \
    -extensions v3_client \
    -extfile "${OUT_DIR}/openssl-client.cnf" >/dev/null 2>&1

cat "${OUT_DIR}/client.crt" "${OUT_DIR}/client.key" > "${OUT_DIR}/client.pem"

# Encrypted client key (passphrase: ragoPemPass2026)
openssl pkcs8 -topk8 -v2 aes-256-cbc \
    -in "${OUT_DIR}/client.key" \
    -out "${OUT_DIR}/client-enc.key" \
    -passout pass:ragoPemPass2026 >/dev/null 2>&1
cat "${OUT_DIR}/client.crt" "${OUT_DIR}/client-enc.key" > "${OUT_DIR}/client-enc.pem"
chmod 600 "${OUT_DIR}/client.key" "${OUT_DIR}/client.pem" "${OUT_DIR}/client-enc.key" "${OUT_DIR}/client-enc.pem"

# Exact RFC2253 subject expected by MongoDB $external X.509 user creation
openssl x509 -in "${OUT_DIR}/client.crt" -noout -subject -nameopt RFC2253 \
    | sed -E 's/^subject= ?//' | tr -d '\r\n' > "${OUT_DIR}/client_subject.txt"

# Replica Set / Sharded Cluster keyfile
openssl rand -base64 756 | tr -d '\r\n ' | head -c 756 > "${OUT_DIR}/mongo-keyfile"
echo "" >> "${OUT_DIR}/mongo-keyfile"
chmod 600 "${OUT_DIR}/mongo-keyfile"

echo "[fixtures] Generating SSH host and client keys ..."
rm -f "${OUT_DIR}/ssh/ssh_host_ed25519_key"* \
      "${OUT_DIR}/ssh/ssh_host_rsa_key"* \
      "${OUT_DIR}/ssh/id_ed25519"* \
      "${OUT_DIR}/ssh/id_rsa"* \
      "${OUT_DIR}/ssh/authorized_keys"

ssh-keygen -q -t ed25519 -N "" -C "rago-sshd-host-ed25519" -f "${OUT_DIR}/ssh/ssh_host_ed25519_key"
ssh-keygen -q -t rsa -b 3072 -N "" -C "rago-sshd-host-rsa" -f "${OUT_DIR}/ssh/ssh_host_rsa_key"

ssh-keygen -q -t ed25519 -N "" -C "rago-client-ed25519" -f "${OUT_DIR}/ssh/id_ed25519"
ssh-keygen -q -t ed25519 -N "ragoSshPass2026" -C "rago-client-ed25519-enc" -f "${OUT_DIR}/ssh/id_ed25519_enc"
ssh-keygen -q -t rsa -b 3072 -N "" -C "rago-client-rsa" -f "${OUT_DIR}/ssh/id_rsa"

cat "${OUT_DIR}/ssh/id_ed25519.pub" \
    "${OUT_DIR}/ssh/id_ed25519_enc.pub" \
    "${OUT_DIR}/ssh/id_rsa.pub" > "${OUT_DIR}/ssh/authorized_keys"

chmod 600 "${OUT_DIR}/ssh/ssh_host_ed25519_key" \
          "${OUT_DIR}/ssh/ssh_host_rsa_key" \
          "${OUT_DIR}/ssh/id_ed25519" \
          "${OUT_DIR}/ssh/id_ed25519_enc" \
          "${OUT_DIR}/ssh/id_rsa" \
          "${OUT_DIR}/ssh/authorized_keys"

echo "[fixtures] Done. X.509 client subject: $(cat "${OUT_DIR}/client_subject.txt")"
