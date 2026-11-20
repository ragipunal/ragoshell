#ifndef RAGOSHELL_SSH_H
#define RAGOSHELL_SSH_H

#ifdef __cplusplus
extern "C" {
#endif

enum rbm_ssh_log_type {
    RBM_SSH_LOG_TYPE_ERROR  = 1,
    RBM_SSH_LOG_TYPE_WARN   = 2,
    RBM_SSH_LOG_TYPE_INFO   = 3,
    RBM_SSH_LOG_TYPE_DEBUG  = 100 // log as much as possible
};

enum rbm_ssh_auth_type {
    RBM_SSH_AUTH_TYPE_NONE       = 0,
    RBM_SSH_AUTH_TYPE_PASSWORD   = 1,
    RBM_SSH_AUTH_TYPE_PUBLICKEY  = 2
};

enum rbm_ssh_error_code {
    RBM_SSH_ERR_NONE       = 0,
    RBM_SSH_ERR_NETWORK    = 1,
    RBM_SSH_ERR_HOSTKEY    = 2,
    RBM_SSH_ERR_AUTH       = 3,
    RBM_SSH_ERR_LOCAL_BIND = 4,
    RBM_SSH_ERR_STOPPED    = 5
};

/*
 * What the tunnel knows about a server's host key when it is not (or no
 * longer) the one in the known hosts file.
 */
enum rbm_hostkey_status {
    RBM_HOSTKEY_NEW     = 0,   // the host is not in the known hosts file
    RBM_HOSTKEY_CHANGED = 1    // the host is there with a different key
};

struct rbm_hostkey_info {
    const char *host;
    unsigned int port;
    const char *keytype;                 // "ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256", ...
    const unsigned char *sha256;         // 32 bytes (SHA-256 of the host key); NULL when not available
    enum rbm_hostkey_status status;
};

/*
 * SSH Tunnel configuration
 */
struct rbm_ssh_tunnel_config {
    enum rbm_ssh_auth_type authtype;

    // Keys and optional passphrase
    char *privatekeyfile;
    char *publickeyfile;
    char *passphrase;   // May be NULL or ""

    // Local IP and port to bind and listen to
    char *localip;
    unsigned int localport;

    // Remote host (domain or IPv4/v6) and port to connect to
    char *remotehost;   // Resolved by the remote server
    unsigned int remoteport;

    // Username and password of remote user
    char *username;
    char *password;     // May be NULL or ""

    // Remote host (domain or IPv4/v6)
    char *sshserverhost;
    unsigned int sshserverport;

    // Host key verification. With a known hosts file (OpenSSH format) the
    // server's key is checked right after the handshake, before any
    // credential is sent: a known key passes; a new or changed key goes to
    // hostkeycallback, which returns 1 to trust it (it is then written to the
    // file) or 0 to refuse. Without a callback an unknown key is refused.
    // NULL knownhostsfile skips the check.
    char *knownhostsfile;
    void *hostkeycontext;
    int (*hostkeycallback)(void *hostkeycontext, const struct rbm_hostkey_info *info);

    // Logging facilities
    enum rbm_ssh_log_type loglevel;
    void *logcontext;   // Pointer to user-defined data (can be NULL)
    void (*logcallback)(void *logcontext, char *message, int level);
};

struct rbm_ssh_session {
    char *lasterror;
    void *handle;    // opaque pointer to rbm_session
    enum rbm_ssh_error_code errorcode;
};

int rbm_ssh_init();
void rbm_ssh_cleanup();

struct rbm_ssh_session* rbm_ssh_session_create(struct rbm_ssh_tunnel_config *config);
int rbm_ssh_open_tunnel(struct rbm_ssh_session *connection);
int rbm_ssh_session_setup(struct rbm_ssh_session *session);
int rbm_ssh_session_listen_local(struct rbm_ssh_session *session);
void rbm_ssh_session_stop(struct rbm_ssh_session *session);
void rbm_ssh_session_close(struct rbm_ssh_session *session);


#ifdef __cplusplus
}
#endif
#endif // RAGOSHELL_SSH_H
