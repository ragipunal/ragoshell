#include "ragoshell/ssh/private.h"
#include "ragoshell/ssh/libssh2_config.h"

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <signal.h>
#include <stdio.h>

static int handle_new_client_connections(struct rbm_session *connection, int *fdmax, fd_set *masterset);
static int handle_ssh_connections(struct rbm_session *connection, fd_set *masterset);
static int handle_client_connections(struct rbm_session *connection, rbm_socket_t i, fd_set *masterset);

static void rbm_sleep_ms(int ms);
static void rbm_socket_close(rbm_socket_t socket);
static int rbm_set_socket_nonblocking(rbm_socket_t sock, int nonblocking);
static void rbm_set_stopped(struct rbm_session *session);
static int rbm_is_stopped(struct rbm_session *session);
static void rbm_set_error_code(struct rbm_session *session, enum rbm_ssh_error_code code);

//===----------------------------------------------------------------------===//
// Stop / Wakeup Helpers
//===----------------------------------------------------------------------===//

static void rbm_set_error_code(struct rbm_session *session, enum rbm_ssh_error_code code) {
    if (session && session->publicsession && session->publicsession->errorcode == RBM_SSH_ERR_NONE)
        session->publicsession->errorcode = code;
}

static int rbm_set_socket_nonblocking(rbm_socket_t sock, int nonblocking) {
#if defined(_WIN32) || defined(WIN32)
    u_long mode = nonblocking ? 1UL : 0UL;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return -1;
    int updated = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, updated);
#endif
}

static void rbm_set_cloexec(rbm_socket_t sock) {
#if defined(_WIN32) || defined(WIN32)
    (void)sock;
#else
    if (sock < 0)
        return;
    int flags = fcntl(sock, F_GETFD, 0);
    if (flags >= 0)
        fcntl(sock, F_SETFD, flags | FD_CLOEXEC);
#endif
}

static void rbm_set_stopped(struct rbm_session *session) {
    if (!session)
        return;
#if defined(_WIN32) || defined(WIN32)
    InterlockedExchange(&session->stop_requested, 1);
#else
    __atomic_store_n(&session->stop_requested, 1, __ATOMIC_SEQ_CST);
    if (session->wakefds[1] != -1) {
        const char byte = 1;
        ssize_t ignored = write(session->wakefds[1], &byte, 1);
        (void)ignored;
    }
#endif
}

static int rbm_is_stopped(struct rbm_session *session) {
    if (!session)
        return 1;
#if defined(_WIN32) || defined(WIN32)
    return InterlockedCompareExchange(&session->stop_requested, 0, 0) != 0;
#else
    return __atomic_load_n(&session->stop_requested, __ATOMIC_SEQ_CST) != 0;
#endif
}

#if !defined(_WIN32) && !defined(WIN32)
static void rbm_drain_wakeup(struct rbm_session *session) {
    if (!session || session->wakefds[0] == -1)
        return;
    char buf[32];
    while (read(session->wakefds[0], buf, sizeof(buf)) > 0) {
    }
}
#endif

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/*
 * Initialize SSH submodule, should be called only once
 * Returns 0 if succeeded, or a negative value for error.
 */
int rbm_ssh_init() {
#if !defined(_WIN32) && !defined(WIN32)
    // Ignore SIGPIPE signal. If we will not do that, an attempt to send/write
    // to the socket that do not have readers will force OS to generate SIGPIPE
    // signal. Default handler for this signal is a termination of program.
    // While signal is ignored, send/write will still return error code, that
    // should be handled by the code.
    signal(SIGPIPE, SIG_IGN);
#endif

    int err;

#if defined(_WIN32) || defined(WIN32)
    WSADATA wsadata;
    err = WSAStartup(MAKEWORD(2,0), &wsadata);
    if (err) {
        log_error("WSAStartup failed with error code: (%d)", err);
        return RBM_ERROR;
    }
#endif

    err = libssh2_init(0);
    if (err) {
        log_error("Initialization of LIBSSH2 failed with error code: (%d)", err);
        return RBM_ERROR;
    }

    return RBM_SUCCESS;
}

/*
 * Cleanup SSH submodule, should be called only once
 */
void rbm_ssh_cleanup() {
    libssh2_exit();

#if defined(_WIN32) || defined(WIN32)
    WSACleanup();
#endif
}

/*
 * Returns NULL on error, valid rbm_ssh_session* if no errors
 */
struct rbm_ssh_session *rbm_ssh_session_create(struct rbm_ssh_tunnel_config *config) {
    if (!config)
        return NULL;

    // Check that loglevel is valid
    if (config->loglevel != RBM_SSH_LOG_TYPE_ERROR &&
        config->loglevel != RBM_SSH_LOG_TYPE_WARN &&
        config->loglevel != RBM_SSH_LOG_TYPE_INFO &&
        config->loglevel != RBM_SSH_LOG_TYPE_DEBUG) {
        log_error("Invalid log level for SSH submodule");
        return NULL;
    }

    struct rbm_session *session = (struct rbm_session *) malloc(sizeof(struct rbm_session));
    if (!session)
        return NULL;

    session->localsocket = rbm_socket_invalid;
    session->sshsocket = rbm_socket_invalid;
    session->sshsession = NULL;
    session->config = config;
    session->channels = NULL;
    session->channelssize = 0;
    session->lasterror[0] = '\0';
    session->stop_requested = 0;
#if !defined(_WIN32) && !defined(WIN32)
    session->wakefds[0] = -1;
    session->wakefds[1] = -1;
    if (pipe(session->wakefds) == 0) {
        rbm_set_cloexec(session->wakefds[0]);
        rbm_set_cloexec(session->wakefds[1]);
        rbm_set_socket_nonblocking(session->wakefds[0], 1);
        rbm_set_socket_nonblocking(session->wakefds[1], 1);
    }
#endif

    struct rbm_ssh_session *publicsession = (struct rbm_ssh_session *) malloc(sizeof(struct rbm_ssh_session));
    if (!publicsession) {
#if !defined(_WIN32) && !defined(WIN32)
        if (session->wakefds[0] != -1) close(session->wakefds[0]);
        if (session->wakefds[1] != -1) close(session->wakefds[1]);
#endif
        free(session);
        return NULL;
    }

    publicsession->lasterror = session->lasterror;
    publicsession->errorcode = RBM_SSH_ERR_NONE;

    // Point to each other
    publicsession->handle = session;
    session->publicsession = publicsession;

    return publicsession;
}

void rbm_ssh_session_stop(struct rbm_ssh_session *sshsession) {
    if (!sshsession || !sshsession->handle)
        return;
    rbm_set_stopped((struct rbm_session *)sshsession->handle);
}

void rbm_ssh_session_close(struct rbm_ssh_session *sshsession) {
    if (!sshsession)
        return;

    struct rbm_session *session = (struct rbm_session*)sshsession->handle;
    if (session) {
        if (session->localsocket != rbm_socket_invalid) {
            ssh_log_debug(session, "Closing local accept socket");
            rbm_socket_close(session->localsocket);
            session->localsocket = rbm_socket_invalid;
        }

        rbm_session_cleanup(session);

#if !defined(_WIN32) && !defined(WIN32)
        if (session->wakefds[0] != -1) {
            close(session->wakefds[0]);
            session->wakefds[0] = -1;
        }
        if (session->wakefds[1] != -1) {
            close(session->wakefds[1]);
            session->wakefds[1] = -1;
        }
#endif

        ssh_log_debug(session, "SSH tunnel successfully closed.");
        free(session);
    }
    free(sshsession);
}

int rbm_ssh_open_tunnel(struct rbm_ssh_session *sshsession) {
    if (!sshsession || !sshsession->handle)
        return RBM_ERROR;

    struct rbm_session *connection = (struct rbm_session*)sshsession->handle;
    int rc = rbm_open_tunnel(connection);
    if (rc == RBM_SUCCESS || rc == RBM_STOPPED)
        return RBM_SUCCESS;

    return RBM_ERROR;
}

int rbm_ssh_session_listen_local(struct rbm_ssh_session *sshsession) {
    if (!sshsession || !sshsession->handle)
        return RBM_ERROR;

    struct rbm_session *session = (struct rbm_session*)sshsession->handle;
    struct rbm_ssh_tunnel_config *config = session->config;

    session->localsocket = socket_listen(session, config->localip, (int *) &config->localport);
    if (session->localsocket == rbm_socket_invalid) {
        rbm_set_error_code(session, RBM_SSH_ERR_LOCAL_BIND);
        return RBM_ERROR; // errors are already logged by socket_listen
    }

    ssh_log_debug(session, "Waiting for TCP connection on %s:%d...", config->localip, config->localport);
    return RBM_SUCCESS;
}

// Returns -1 on error, 0 when otherwise
int rbm_ssh_session_setup(struct rbm_ssh_session *sshsession) {
    if (!sshsession || !sshsession->handle)
        return RBM_ERROR;

    struct rbm_session *session = (struct rbm_session*)sshsession->handle;

    if (rbm_ssh_setup(session) == -1)
        return RBM_ERROR;

    if (rbm_ssh_session_listen_local(sshsession) == RBM_ERROR) {
        rbm_session_cleanup(session);
        return RBM_ERROR;
    }

    return RBM_SUCCESS;
}


//===----------------------------------------------------------------------===//
// Private API
//===----------------------------------------------------------------------===//


// Returns NULL on error, or valid rbm_ssh_channel otherwise.
struct rbm_channel *rbm_channel_create(struct rbm_session* session, rbm_socket_t socket, LIBSSH2_CHANNEL *lchannel) {
    struct rbm_channel *channel = (struct rbm_channel *)malloc(sizeof(struct rbm_channel));
    if (!channel)
        return NULL;

    char *inbuf = (char *)malloc(sizeof(char) * RBM_BUFSIZE);
    if (!inbuf) {
        free(channel);
        return NULL;
    }

    char *outbuf = (char *)malloc(sizeof(char) * RBM_BUFSIZE);
    if (!outbuf) {
        free(inbuf);
        free(channel);
        return NULL;
    }

    channel->session = session;
    channel->channel = lchannel;
    channel->socket = socket;
    channel->inbuf = inbuf;
    channel->outbuf = outbuf;
    channel->bufmaxsize = RBM_BUFSIZE;

    if (rbm_array_add((void ***)&session->channels, &session->channelssize, channel)) {
        free(outbuf);
        free(inbuf);
        free(channel);
        return NULL;
    }

    return channel;
}

void rbm_channel_close(struct rbm_channel *channel) {
    struct rbm_session *session = channel->session;
    if (rbm_array_remove((void ***)&session->channels, &session->channelssize, channel))
        return;

    // 1. Close local client socket
    if (channel->socket != rbm_socket_invalid) {
        rbm_socket_close(channel->socket);
        channel->socket = rbm_socket_invalid;
    }

    // 2. Free libssh2 channel
    if (channel->channel) {
        libssh2_channel_free(channel->channel);
        channel->channel = NULL;
    }

    // 3. Free input/output buffers
    free(channel->inbuf);
    free(channel->outbuf);
    channel->inbuf = NULL;
    channel->outbuf = NULL;

    // 4. Free channel struct
    free(channel);

    ssh_log_debug(session, "Channel closed");
}

/*
 * Returns channel for specified socket, or NULL otherwise
 */
struct rbm_channel *rbm_channel_find_by_socket(struct rbm_session *session, rbm_socket_t socket) {
    for (int i = 0; i < session->channelssize; i++)
        if (session->channels[i]->socket == socket)
            return session->channels[i];

    return NULL;
}

// Returns -1 on error, 0 when otherwise
int rbm_ssh_setup(struct rbm_session *session) {
    struct rbm_ssh_tunnel_config *config = session->config;

    if (rbm_is_stopped(session)) {
        rbm_set_error_code(session, RBM_SSH_ERR_STOPPED);
        return RBM_ERROR;
    }

    ssh_log_debug(session, "Connecting to SSH server (%s:%d)...", config->sshserverhost, config->sshserverport);

    session->sshsocket = socket_connect(session, config->sshserverhost, config->sshserverport);
    if (session->sshsocket == rbm_socket_invalid) {
        return RBM_ERROR; // errors and errorcode are already set by socket_connect
    }

    session->sshsession = ssh_connect(session, session->sshsocket, config->authtype, config->username, config->password,
                                      config->publickeyfile, config->privatekeyfile, config->passphrase);
    if (session->sshsession == 0) {
        rbm_socket_close(session->sshsocket);
        session->sshsocket = rbm_socket_invalid;
        return RBM_ERROR; // errors and errorcode are already set by ssh_connect
    }

    // Reset blocking handshake timeout and switch to non-blocking IO for multiplexing.
    libssh2_session_set_timeout(session->sshsession, 0);
    libssh2_session_set_blocking(session->sshsession, 0);

    return RBM_SUCCESS;
}

void rbm_session_cleanup(struct rbm_session *session) {
    if (session == NULL)
        return;

    // Best-effort polite SSH disconnect while sshsocket is still writable, then shut down
    // sshsocket before freeing non-blocking libssh2 channels/session so a full OS send buffer
    // returns hard error instead of LIBSSH2_ERROR_EAGAIN (preventing leaked LIBSSH2_SESSION/CHANNELs).
    if (session->sshsession) {
        libssh2_session_disconnect(session->sshsession, "Client disconnecting normally");
    }
    if (session->sshsocket != rbm_socket_invalid) {
#if defined(_WIN32) || defined(WIN32)
        shutdown(session->sshsocket, SD_BOTH);
#else
        shutdown(session->sshsocket, SHUT_RDWR);
#endif
    }

    // 1. Free libssh2 channels and close accepted client sockets.
    ssh_log_debug(session, "Closing channels");
    while (session->channelssize > 0) {
        rbm_channel_close(session->channels[0]);
    }

    // 2. Free libssh2 session.
    if (session->sshsession) {
        ssh_log_debug(session, "Closing SSH session");
        libssh2_session_free(session->sshsession);
        session->sshsession = NULL;
    }

    // 3. Close ssh socket FD last so the descriptor cannot be recycled during libssh2 teardown.
    if (session->sshsocket != rbm_socket_invalid) {
        ssh_log_debug(session, "Closing SSH socket");
        rbm_socket_close(session->sshsocket);
        session->sshsocket = rbm_socket_invalid;
    }
}


int rbm_open_tunnel(struct rbm_session *connection) {
    rbm_socket_t local_socket = connection->localsocket;
    rbm_socket_t ssh_socket = connection->sshsocket;

#if !defined(_WIN32) && !defined(WIN32)
    if (local_socket < 0 || local_socket >= FD_SETSIZE ||
        ssh_socket < 0 || ssh_socket >= FD_SETSIZE) {
        ssh_log_error(connection, "Socket descriptor exceeds FD_SETSIZE");
        rbm_set_error_code(connection, RBM_SSH_ERR_NETWORK);
        return RBM_ERROR;
    }
#endif

    const int maxerrors = 100;  // number of serial errors, when we probably should stop the loop
    int errors = 0;             // counter for serial errors
    int rc = 0;

    int fdmax;       // maximum socket (file descriptor) number
    int isocket;     // index for traversing sockets
    fd_set masterset, readset;
    FD_ZERO(&masterset);

    // Add the listener to the master set. Only select on ssh_socket while at
    // least one forwarded channel is active so post-auth OpenSSH global request
    // packets (e.g. hostkeys-00@openssh.com) arriving before MongoWorker opens
    // its first TCP socket do not prematurely close the tunnel.
    FD_SET(local_socket, &masterset);

    fdmax = (int)(local_socket > ssh_socket ? local_socket : ssh_socket);
#if !defined(_WIN32) && !defined(WIN32)
    if (connection->wakefds[0] >= 0 && connection->wakefds[0] < FD_SETSIZE) {
        FD_SET(connection->wakefds[0], &masterset);
        if (connection->wakefds[0] > fdmax)
            fdmax = connection->wakefds[0];
    }
#endif

    while (errors < maxerrors) {
        if (rbm_is_stopped(connection)) {
            rbm_set_error_code(connection, RBM_SSH_ERR_STOPPED);
            return RBM_STOPPED;
        }

        readset = masterset; // copy set

        if (!FD_ISSET(local_socket, &readset))
            break;

        if (connection->channelssize > 0)
            FD_SET(ssh_socket, &readset);
        else
            FD_CLR(ssh_socket, &readset);

        struct timeval tv;
#if defined(_WIN32) || defined(WIN32)
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000; // 100 ms poll for stop_requested on Windows
#else
        tv.tv_sec = 0;
        tv.tv_usec = 250 * 1000; // wakeup pipe is instant; 250 ms fallback
#endif

        ssh_log_debug(connection, "* Okay, we are ready to select.");
        int sel = select(fdmax + 1, &readset, NULL, NULL, &tv);
        if (sel == -1) {
#if !defined(_WIN32) && !defined(WIN32)
            if (errno == EINTR)
                continue;
#endif
            if (rbm_is_stopped(connection)) {
                rbm_set_error_code(connection, RBM_SSH_ERR_STOPPED);
                return RBM_STOPPED;
            }
            ssh_log_error(connection, "Error on select()");
            rbm_set_error_code(connection, RBM_SSH_ERR_NETWORK);
            return RBM_ERROR;
        }

        if (rbm_is_stopped(connection)) {
            rbm_set_error_code(connection, RBM_SSH_ERR_STOPPED);
            return RBM_STOPPED;
        }

        if (sel == 0)
            continue;

#if !defined(_WIN32) && !defined(WIN32)
        if (connection->wakefds[0] >= 0 && FD_ISSET(connection->wakefds[0], &readset)) {
            rbm_drain_wakeup(connection);
            if (rbm_is_stopped(connection)) {
                rbm_set_error_code(connection, RBM_SSH_ERR_STOPPED);
                return RBM_STOPPED;
            }
        }
#endif

        ssh_log_debug(connection, "* Selected!");

        // Run through the existing connections looking for data to read
#if defined(_WIN32) || defined(WIN32)
        for (u_int idx = 0; idx < readset.fd_count; ++idx) {
            rbm_socket_t candidate = readset.fd_array[idx];
#else
        for (isocket = 0; isocket <= fdmax; isocket++) {
            rbm_socket_t candidate = (rbm_socket_t)isocket;

            if (isocket == connection->wakefds[0])
                continue;

            // Skip connections that are not available for reading
            if (!FD_ISSET(candidate, &readset))
                continue;
#endif

            if (candidate == local_socket) {
                rc = handle_new_client_connections(connection, &fdmax, &masterset);
                goto next;
            }

            if (candidate == ssh_socket) {
                rc = handle_ssh_connections(connection, &masterset);
                goto next;
            }

            rc = handle_client_connections(connection, candidate, &masterset);

            next:
            if (rc == RBM_STOPPED) {
                rbm_set_error_code(connection, RBM_SSH_ERR_STOPPED);
                return RBM_STOPPED;
            }

            // Increment "errors" counter, if we found an EAGAIN condition,
            // or zero it, if not.
            errors = (rc == -2) ? errors + 1 : 0;
            if (errors > 0) {
                ssh_log_warn(connection, "*** COLLECTED %d AGAIN ***", errors);
                rbm_sleep_ms(5);
            }

            if (rc == RBM_ERROR || rc == RBM_CHANNEL_CREATION_ERROR) {
                ssh_log_warn(connection, "SSH tunnel shutdown because of error");
                rbm_set_error_code(connection, RBM_SSH_ERR_NETWORK);
                return RBM_ERROR;
            }
        }
    }

    if (errors >= maxerrors) {
        ssh_log_warn(connection, "SSH tunnel shutdown because of series of successive EAGAIN errors");
        rbm_set_error_code(connection, RBM_SSH_ERR_NETWORK);
        return RBM_ERROR;
    }

    return RBM_SUCCESS;
}

// Return -1 on error, 0 otherwise.
static int handle_new_client_connections(struct rbm_session *connection, int *fdmax, fd_set *masterset) {
    rbm_socket_t local_socket = connection->localsocket;
    LIBSSH2_SESSION* session = connection->sshsession;
    struct rbm_ssh_tunnel_config* config = connection->config;
    rbm_socket_t newfd = rbm_socket_invalid;

    ssh_log_debug(connection, "Data on accept socket is available");

    struct sockaddr_in remoteaddr;
    socklen_t slen = sizeof(remoteaddr);

    // handle new connections
    newfd = accept(local_socket, (struct sockaddr *) &remoteaddr, &slen);
    if (newfd == rbm_socket_invalid) {
        if (rbm_is_stopped(connection))
            return RBM_STOPPED;
        ssh_log_error(connection, "Error on accept()");
        return RBM_ERROR;
    }
    rbm_set_cloexec(newfd);

#if !defined(_WIN32) && !defined(WIN32)
    if (newfd < 0 || newfd >= FD_SETSIZE) {
        rbm_socket_close(newfd);
        ssh_log_warn(connection, "Accepted socket exceeds FD_SETSIZE; closing client connection");
        return RBM_SUCCESS;
    }
#endif

    ssh_log_debug(connection, "New connection from %s on socket %d", inet_ntoa(remoteaddr.sin_addr), (int)newfd);

    LIBSSH2_CHANNEL *channel = NULL;
    int maxattempts = 500;
    int attempts = 0;
    int last_ssh_err = 0;
    while (attempts < maxattempts) {
        if (rbm_is_stopped(connection)) {
            rbm_socket_close(newfd);
            return RBM_STOPPED;
        }
        ++attempts;
        channel = libssh2_channel_direct_tcpip_ex(session, config->remotehost, config->remoteport,
                                                  config->localip, config->localport);

        if (!channel) {
            last_ssh_err = libssh2_session_last_errno(connection->sshsession);

            // Error -37: LIBSSH2_ERROR_EAGAIN while sshd opens remote TCP socket.
            if (last_ssh_err == LIBSSH2_ERROR_EAGAIN) {
                rbm_sleep_ms(10);
                continue;
            }

            ssh_log_warn(connection, "Could not open the direct TCP/IP channel (%d)", last_ssh_err);
            break;
        }

        ssh_log_debug(connection, "Channel successfully created!");
        break;
    }

    if (!channel) {
        rbm_socket_close(newfd);
        // LIBSSH2_ERROR_CHANNEL_FAILURE means sshd itself is alive and authenticated,
        // and explicitly rejected forwarding to remotehost:remoteport (e.g. mongod restarting).
        // Close only this client socket and keep the SSH session open.
        if (last_ssh_err == LIBSSH2_ERROR_CHANNEL_FAILURE) {
            ssh_log_warn(connection, "SSH server refused forwarding to %s:%u (channel error %d)",
                         config->remotehost, config->remoteport, last_ssh_err);
            return RBM_SUCCESS;
        }
        ssh_log_error(connection, "Failed to create SSH channel (transport error %d)", last_ssh_err);
        return RBM_CHANNEL_CREATION_ERROR;
    }

    if (rbm_channel_create(connection, newfd, channel) == NULL) {
        libssh2_channel_free(channel);
        rbm_socket_close(newfd);
        return RBM_ERROR;
    }

    FD_SET(newfd, masterset);
    if ((int)newfd > *fdmax) {
        *fdmax = (int)newfd;
    }

    return RBM_SUCCESS;
}

static int handle_ssh_connections(struct rbm_session *connection, fd_set *masterset) {
    const int AGAIN = -2;
    struct rbm_ssh_tunnel_config* config = connection->config;

    ssh_log_debug(connection, "Data on SSH socket is available");
    ssh_log_debug(connection, "[%d]  <-  Number of channels", connection->channelssize);

    if (connection->channelssize == 0) {
        return RBM_SUCCESS;
    }

    int s = 0;
    int result = RBM_SUCCESS;
    int eagain = 0;
    while (s < connection->channelssize) {
        struct rbm_channel *context = connection->channels[s];
        ++s;

        int firstflag = 1;
        while (1) {
            if (rbm_is_stopped(connection))
                return RBM_STOPPED;

            int len = (int)libssh2_channel_read(context->channel, context->outbuf, context->bufmaxsize);
            if (len == LIBSSH2_ERROR_EAGAIN) {
                if (firstflag) {
                    ++eagain;
                    if (eagain == connection->channelssize) {
                        result = AGAIN;
                        ssh_log_warn(connection, "All channels are in a non ready state (EAGAIN)");
                    }
                }
                break;
            } else if (len < 0) {
                ssh_log_error(connection, "libssh2_channel_read: %d", len);
                return RBM_ERROR;
            }

            firstflag = 0;
            ssh_log_debug(connection, "Received %d bytes from tunnel", len);

            int wr = 0;
            int client_failed = 0;
            while (wr < len) {
                int rc = (int)send(context->socket, context->outbuf + wr, len - wr, 0);
                if (rc <= 0) {
                    // Only this local client disconnected; close the channel, keep the SSH tunnel open.
                    ssh_log_debug(connection, "Local client closed socket while receiving tunnel data");
                    client_failed = 1;
                    break;
                }
                wr += rc;
            }
            if (client_failed || libssh2_channel_eof(context->channel)) {
                if (!client_failed) {
                    ssh_log_debug(connection, "The server at %s:%d disconnected!\n",
                                  config->remotehost, config->remoteport);
                }
                FD_CLR(context->socket, masterset);
                rbm_channel_close(context);
                --s;
                break;
            }
        }
    }

    return result;
}

static int handle_client_connections(struct rbm_session *connection, rbm_socket_t i, fd_set *masterset) {
    ssh_log_debug(connection, "Data on client socket is available");

    struct rbm_channel *context = rbm_channel_find_by_socket(connection, i);
    if (!context) {
        rbm_socket_close(i);
        FD_CLR(i, masterset);
        return RBM_SUCCESS;
    }

    // Read data from a local client
    int nbytes = (int)recv(context->socket, context->inbuf, context->bufmaxsize, 0);
    if (nbytes <= 0) {
        if (nbytes == 0) {
            ssh_log_debug(connection, "Client disconnected");
        } else {
            ssh_log_debug(connection, "Client socket closed with reset");
        }

        // Local client closed its connection: close only this channel, keep listening on localsocket.
        FD_CLR(context->socket, masterset);
        rbm_channel_close(context);
        return RBM_SUCCESS;
    }
    ssh_log_debug(connection, "Received %d bytes from client", nbytes);

    // Write data to ssh tunnel
    const int againmax = 400;
    int again = 0;
    int wr = 0;
    while (wr < nbytes) {
        if (rbm_is_stopped(connection))
            return RBM_STOPPED;

        int rc = (int)libssh2_channel_write(context->channel, context->inbuf + wr, nbytes - wr);
        if (LIBSSH2_ERROR_EAGAIN == rc) {
            ++again;

            if (again > againmax) {
                ssh_log_warn(connection, "Number of attempts to libssh2_channel_write exceeded max value");
                return RBM_ERROR;
            }

            rbm_sleep_ms(5);
            continue;
        }
        if (rc < 0) {
            ssh_log_error(connection, "Failed to write to SSH channel (%d)", rc);
            return RBM_ERROR;
        }
        wr += rc;
    }
    ssh_log_debug(connection, "Written %d bytes to tunnel", wr);

    return RBM_SUCCESS;
}

/*
 * Returns connected socket if succeeded, otherwise rbm_socket_invalid on error.
 * Uses non-blocking connect() with bounded timeout and stop_requested checks.
 */
static rbm_socket_t socket_connect(struct rbm_session* session, char *ip, int port) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    int err;
    char cport[20];

    if (rbm_is_stopped(session)) {
        rbm_set_error_code(session, RBM_SSH_ERR_STOPPED);
        return rbm_socket_invalid;
    }

    snprintf(cport, sizeof(cport), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(ip, cport, &hints, &res);
    if (err || !res) {
        ssh_log_error(session, "Failed to get address info for %s:%d", ip ? ip : "(null)", port);
        rbm_set_error_code(session, RBM_SSH_ERR_NETWORK);
        return rbm_socket_invalid;
    }

    rbm_socket_t connected_sock = rbm_socket_invalid;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (rbm_is_stopped(session)) {
            freeaddrinfo(res);
            rbm_set_error_code(session, RBM_SSH_ERR_STOPPED);
            return rbm_socket_invalid;
        }

        rbm_socket_t sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == rbm_socket_invalid)
            continue;
        rbm_set_cloexec(sock);

#if !defined(_WIN32) && !defined(WIN32)
        if (sock < 0 || sock >= FD_SETSIZE) {
            rbm_socket_close(sock);
            continue;
        }
#endif

        rbm_set_socket_nonblocking(sock, 1);

        int rc = connect(sock, rp->ai_addr, (int)rp->ai_addrlen);
        if (rc == 0) {
            rbm_set_socket_nonblocking(sock, 0);
            connected_sock = sock;
            break;
        }

#if defined(_WIN32) || defined(WIN32)
        int in_progress = (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS);
#else
        int in_progress = (errno == EINPROGRESS);
#endif
        if (!in_progress) {
            rbm_socket_close(sock);
            continue;
        }

        // Wait up to 10 seconds in 200 ms slices so stop() wakes promptly.
        int elapsed_ms = 0;
        const int timeout_ms = 10000;
        const int slice_ms = 200;
        int ok = 0;

        while (elapsed_ms < timeout_ms) {
            if (rbm_is_stopped(session)) {
                rbm_socket_close(sock);
                freeaddrinfo(res);
                rbm_set_error_code(session, RBM_SSH_ERR_STOPPED);
                return rbm_socket_invalid;
            }

            fd_set readset, writeset, exceptset;
            FD_ZERO(&readset);
            FD_ZERO(&writeset);
            FD_ZERO(&exceptset);
            FD_SET(sock, &writeset);
            FD_SET(sock, &exceptset);
            int fdmax = (int)sock;

#if !defined(_WIN32) && !defined(WIN32)
            if (session->wakefds[0] >= 0 && session->wakefds[0] < FD_SETSIZE) {
                FD_SET(session->wakefds[0], &readset);
                if (session->wakefds[0] > fdmax)
                    fdmax = session->wakefds[0];
            }
#endif
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = slice_ms * 1000;

            int sel = select(fdmax + 1, &readset, &writeset, &exceptset, &tv);
            if (sel < 0) {
#if !defined(_WIN32) && !defined(WIN32)
                if (errno == EINTR)
                    continue;
#endif
                break;
            }
#if !defined(_WIN32) && !defined(WIN32)
            if (session->wakefds[0] >= 0 && FD_ISSET(session->wakefds[0], &readset)) {
                rbm_drain_wakeup(session);
                if (rbm_is_stopped(session)) {
                    rbm_socket_close(sock);
                    freeaddrinfo(res);
                    rbm_set_error_code(session, RBM_SSH_ERR_STOPPED);
                    return rbm_socket_invalid;
                }
            }
#endif
            if (sel == 0) {
                elapsed_ms += slice_ms;
                continue;
            }

            if (FD_ISSET(sock, &writeset) || FD_ISSET(sock, &exceptset)) {
                int so_error = 0;
                socklen_t slen = sizeof(so_error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &slen) == 0 && so_error == 0) {
                    ok = 1;
                }
                break;
            }
            elapsed_ms += slice_ms;
        }

        if (ok) {
            rbm_set_socket_nonblocking(sock, 0);
            connected_sock = sock;
            break;
        }

        rbm_socket_close(sock);
    }

    freeaddrinfo(res);

    if (connected_sock == rbm_socket_invalid) {
        ssh_log_error(session, "Failed to connect to %s:%d", ip, port);
        rbm_set_error_code(session, RBM_SSH_ERR_NETWORK);
    }
    return connected_sock;
}

/*
 * Returns socket (bound exclusively to 127.0.0.1 and in listen state) if succeeded,
 * otherwise rbm_socket_invalid on error.
 * Security invariants:
 *   - Refuses any non-loopback address (0.0.0.0 / LAN IP rejected).
 *   - Never sets SO_REUSEPORT.
 *   - Never sets SO_REUSEADDR on Windows (where Winsock SO_REUSEADDR allows port hijacking).
 */
rbm_socket_t socket_listen(struct rbm_session *rsession, char *ip, int *port) {
    rbm_socket_t listensock;
    struct sockaddr_in sin;
    socklen_t sinlen;

    if (!ip || strcmp(ip, "127.0.0.1") != 0 || !port) {
        errno = 0;
        ssh_log_error(rsession, "Refusing to bind SSH tunnel listener to non-loopback address");
        rbm_set_error_code(rsession, RBM_SSH_ERR_LOCAL_BIND);
        return rbm_socket_invalid;
    }

    int requested_port = (*port > 0 && *port <= 65535) ? *port : 0;

    listensock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listensock == rbm_socket_invalid) {
        ssh_log_error(rsession, "Failed to open local listener socket");
        rbm_set_error_code(rsession, RBM_SSH_ERR_LOCAL_BIND);
        return rbm_socket_invalid;
    }
    rbm_set_cloexec(listensock);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)requested_port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

#if !defined(_WIN32) && !defined(WIN32)
    // On POSIX, SO_REUSEADDR permits rebinding 127.0.0.1:<port> immediately after closing
    // the previous listener when former accepted client sockets are in TIME_WAIT, while still
    // failing with EADDRINUSE if any active listener owns the port. SO_REUSEPORT is never used.
    int sockopt = 1;
    setsockopt(listensock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));
#endif

    sinlen = sizeof(sin);
    if (-1 == bind(listensock, (struct sockaddr *)&sin, sinlen)) {
        ssh_log_error(rsession, "Cannot bind local SSH listener to 127.0.0.1:%d", requested_port);
        rbm_socket_close(listensock);
        rbm_set_error_code(rsession, RBM_SSH_ERR_LOCAL_BIND);
        return rbm_socket_invalid;
    }

    if (-1 == listen(listensock, 64)) {
        ssh_log_error(rsession, "Failed to listen on local SSH socket");
        rbm_socket_close(listensock);
        rbm_set_error_code(rsession, RBM_SSH_ERR_LOCAL_BIND);
        return rbm_socket_invalid;
    }

    if (getsockname(listensock, (struct sockaddr *)&sin, &sinlen) == -1) {
        ssh_log_error(rsession, "Failed to get local SSH socket address");
        rbm_socket_close(listensock);
        rbm_set_error_code(rsession, RBM_SSH_ERR_LOCAL_BIND);
        return rbm_socket_invalid;
    }

    *port = ntohs(sin.sin_port);
    return listensock;
}

static const char *hostkey_type_name(int type, int *knownhostbits) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       *knownhostbits = LIBSSH2_KNOWNHOST_KEY_SSHRSA;    return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_DSS:       *knownhostbits = LIBSSH2_KNOWNHOST_KEY_SSHDSS;    return "ssh-dss";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: *knownhostbits = LIBSSH2_KNOWNHOST_KEY_ECDSA_256; return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: *knownhostbits = LIBSSH2_KNOWNHOST_KEY_ECDSA_384; return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: *knownhostbits = LIBSSH2_KNOWNHOST_KEY_ECDSA_521; return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   *knownhostbits = LIBSSH2_KNOWNHOST_KEY_ED25519;   return "ssh-ed25519";
    default:                             *knownhostbits = 0;                               return NULL;
    }
}

/*
 * Checks the server's host key against the known hosts file, asking
 * hostkeycallback about a new or changed key. Returns 1 when the connection
 * may go on, 0 (with rsession->lasterror and RBM_SSH_ERR_HOSTKEY set) when it must stop.
 */
static int verify_hostkey(struct rbm_session *rsession, LIBSSH2_SESSION *session) {
    struct rbm_ssh_tunnel_config *config = rsession->config;
    if (!config->knownhostsfile)
        return 1;

    size_t keylen = 0;
    int keytype = 0;
    int keybits = 0;
    const char *key = libssh2_session_hostkey(session, &keylen, &keytype);
    const char *typename_ = key ? hostkey_type_name(keytype, &keybits) : NULL;
    if (!key || !typename_) {
        ssh_log_error(rsession, "The SSH server's host key has an unsupported type; refusing to connect");
        rbm_set_error_code(rsession, RBM_SSH_ERR_HOSTKEY);
        return 0;
    }

    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(session);
    if (!hosts) {
        ssh_log_error(rsession, "Could not check the SSH server's host key (known hosts)");
        rbm_set_error_code(rsession, RBM_SSH_ERR_HOSTKEY);
        return 0;
    }
    /* A missing file is an empty list. */
    libssh2_knownhost_readfile(hosts, config->knownhostsfile, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    errno = 0;   /* the file may not exist yet; do not let that leak into error messages */

    const char *host = config->sshserverhost;
    int port = (int) config->sshserverport;
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | keybits;
    struct libssh2_knownhost *known = NULL;
    int check = libssh2_knownhost_checkp(hosts, host, port, key, keylen, typemask, &known);

    int allowed = 0;
    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        allowed = 1;
    } else if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND || check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        const char *sha256 = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
        struct rbm_hostkey_info info;
        info.host = host;
        info.port = config->sshserverport;
        info.keytype = typename_;
        info.sha256 = (const unsigned char *) sha256;
        info.status = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) ? RBM_HOSTKEY_CHANGED : RBM_HOSTKEY_NEW;

        if (config->hostkeycallback && config->hostkeycallback(config->hostkeycontext, &info)) {
            char hostentry[512];
            if (port == 22)
                snprintf(hostentry, sizeof(hostentry), "%s", host);
            else
                snprintf(hostentry, sizeof(hostentry), "[%s]:%d", host, port);
            if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH && known)
                libssh2_knownhost_del(hosts, known);
            if (libssh2_knownhost_addc(hosts, hostentry, NULL, key, keylen, NULL, 0, typemask, NULL) == 0 &&
                libssh2_knownhost_writefile(hosts, config->knownhostsfile, LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0)
                ssh_log_msg(rsession, "SSH host key of %s trusted and saved", hostentry);
            else
                ssh_log_warn(rsession, "SSH host key of %s trusted for this connection, but could not be saved", hostentry);
            allowed = 1;
        } else if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
            ssh_log_error(rsession, "The SSH server's host key has CHANGED (%s:%d). Connection refused: "
                                    "someone may be intercepting the connection", host, port);
            rbm_set_error_code(rsession, RBM_SSH_ERR_HOSTKEY);
        } else {
            ssh_log_error(rsession, "The SSH server's host key (%s:%d) was not trusted. Connection refused", host, port);
            rbm_set_error_code(rsession, RBM_SSH_ERR_HOSTKEY);
        }
    } else {
        ssh_log_error(rsession, "Could not check the SSH server's host key (known hosts error %d)", check);
        rbm_set_error_code(rsession, RBM_SSH_ERR_HOSTKEY);
    }

    libssh2_knownhost_free(hosts);
    return allowed;
}

static int is_ssh_transport_errno(int err) {
    return err == LIBSSH2_ERROR_SOCKET_SEND ||
           err == LIBSSH2_ERROR_SOCKET_RECV ||
           err == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
           err == LIBSSH2_ERROR_BAD_SOCKET ||
           err == LIBSSH2_ERROR_TIMEOUT ||
           err == LIBSSH2_ERROR_PROTO;
}

/*
 * Returns 0 if error.
 */
LIBSSH2_SESSION *ssh_connect(struct rbm_session *rsession, rbm_socket_t sock, enum rbm_ssh_auth_type type, char *username, char *password,
                             char *publickeypath, char *privatekeypath, char *passphrase) {
    int rc, auth = RBM_SSH_AUTH_TYPE_NONE;
    LIBSSH2_SESSION *session;
    char *userauthlist;

    ssh_log_debug(rsession, "ssh_connect: username: %s", username);
    ssh_log_debug(rsession, "ssh_connect: privatekeyfile: %s", privatekeypath);
    ssh_log_debug(rsession, "ssh_connect: publickeyfile: %s", publickeypath);

    /* Create a session instance */
    session = libssh2_session_init();
    if (!session) {
        ssh_log_error(rsession, "Could not initialize SSH session");
        rbm_set_error_code(rsession, RBM_SSH_ERR_NETWORK);
        return 0;
    }

    /* Bound blocking handshake and authentication operations to 10 seconds. */
    libssh2_session_set_timeout(session, 10000);

    rc = libssh2_session_handshake(session, sock);
    if (rc) {
        ssh_log_error(rsession, "Error when starting up SSH session: %d", rc);
        libssh2_session_free(session);
        rbm_set_error_code(rsession, RBM_SSH_ERR_NETWORK);
        return 0;
    }

    if (rbm_is_stopped(rsession)) {
        libssh2_session_disconnect(session, "Stopped");
        libssh2_session_free(session);
        rbm_set_error_code(rsession, RBM_SSH_ERR_STOPPED);
        return 0;
    }

    /* Check the server's host key against known_hosts BEFORE any credential is sent. */
    if (!verify_hostkey(rsession, session)) {
        libssh2_session_disconnect(session, "Host key not trusted");
        libssh2_session_free(session);
        return 0;
    }

    if (rbm_is_stopped(rsession)) {
        libssh2_session_disconnect(session, "Stopped");
        libssh2_session_free(session);
        rbm_set_error_code(rsession, RBM_SSH_ERR_STOPPED);
        return 0;
    }

    /* Check what authentication methods are available */
    userauthlist = libssh2_userauth_list(session, username, (unsigned int)strlen(username));
    if (!userauthlist) {
        if (libssh2_userauth_authenticated(session))
            return session;
        int lerr = libssh2_session_last_errno(session);
        ssh_log_error(rsession, "Failed to query SSH authentication methods for user '%s'", username);
        libssh2_session_free(session);
        rbm_set_error_code(rsession, is_ssh_transport_errno(lerr) ? RBM_SSH_ERR_NETWORK : RBM_SSH_ERR_AUTH);
        return 0;
    }
    ssh_log_debug(rsession, "Authentication methods: %s", userauthlist);
    if (strstr(userauthlist, "password"))
        auth |= RBM_SSH_AUTH_TYPE_PASSWORD;
    if (strstr(userauthlist, "publickey"))
        auth |= RBM_SSH_AUTH_TYPE_PUBLICKEY;

    if (auth & RBM_SSH_AUTH_TYPE_PASSWORD && type == RBM_SSH_AUTH_TYPE_PASSWORD) {
        rc = libssh2_userauth_password(session, username, password);
        if (rc) {
            int lerr = libssh2_session_last_errno(session);
            errno = 0;
            ssh_log_error(rsession, "Authentication by password failed");
            libssh2_session_free(session);
            rbm_set_error_code(rsession, is_ssh_transport_errno(lerr) ? RBM_SSH_ERR_NETWORK : RBM_SSH_ERR_AUTH);
            return 0;
        }
        ssh_log_debug(rsession, "Authentication by password succeeded.");
    } else if (auth & RBM_SSH_AUTH_TYPE_PUBLICKEY && type == RBM_SSH_AUTH_TYPE_PUBLICKEY) {
        rc = libssh2_userauth_publickey_fromfile(session, username, publickeypath, privatekeypath, passphrase);
        if (rc) {
            int lerr = libssh2_session_last_errno(session);
            errno = 0;
            ssh_log_error(rsession, "Authentication by key (%s) failed (Error %d)", privatekeypath, rc);
            libssh2_session_free(session);
            rbm_set_error_code(rsession, is_ssh_transport_errno(lerr) ? RBM_SSH_ERR_NETWORK : RBM_SSH_ERR_AUTH);
            return 0;
        }
        ssh_log_debug(rsession, "Authentication by key succeeded.");
    } else {
        errno = 0;
        ssh_log_error(rsession, "No supported authentication methods found");
        libssh2_session_free(session);
        rbm_set_error_code(rsession, RBM_SSH_ERR_AUTH);
        return 0;
    }

    return session;
}

static void rbm_sleep_ms(int ms) {
#if defined(_WIN32) || defined(WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void rbm_socket_close(rbm_socket_t socket) {
#if defined(_WIN32) || defined(WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}
