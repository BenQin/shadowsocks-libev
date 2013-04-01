#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#include "utils.h"
#include "server.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#define min(a,b) (((a)<(b))?(a):(b))

static int verbose = 0;
static int remote_conn = 0;
static int server_conn = 0;

int setnonblocking(int fd) {
    int flags;
    if (-1 ==(flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_and_bind(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, listen_sock;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC; /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */

    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        LOGE("getaddrinfo: %s", gai_strerror(s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listen_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_sock == -1)
            continue;

        int opt = 1;
        int err = setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (err) {
            perror("setsocket");
        }

        s = bind(listen_sock, rp->ai_addr, rp->ai_addrlen);
        if (s == 0) {
            /* We managed to bind successfully! */
            break;
        } else {
            perror("bind");
        }

        close(listen_sock);
    }

    if (rp == NULL) {
        LOGE("Could not bind");
        return -1;
    }

    freeaddrinfo(result);

    return listen_sock;
}

struct remote *connect_to_remote(char *remote_host, char *remote_port, int timeout) {
    struct addrinfo hints, *res;
    int sockfd;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(remote_host, remote_port, &hints, &res);
    if (err) {
        perror("getaddrinfo");
        return NULL;
    }

    // initilize remote socks
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        perror("socket");
        close(sockfd);
        freeaddrinfo(res);
        return NULL;
    }

    struct remote *remote = new_remote(sockfd, timeout);

    // setup remote socks
    setnonblocking(sockfd);
    connect(sockfd, res->ai_addr, res->ai_addrlen);

    // release addrinfo
    freeaddrinfo(res);

    return remote;
}


static void server_recv_cb (EV_P_ ev_io *w, int revents) {
    struct server_ctx *server_recv_ctx = (struct server_ctx *)w;
    struct server *server = server_recv_ctx->server;
    struct remote *remote = NULL;

    char *buf = server->buf;

    if (server->stage != 0) {
        remote = server->remote;
        buf = remote->buf;
    }

    ssize_t r = recv(server->fd, buf, BUF_SIZE, 0);

    if (r == 0) {
        // connection closed
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data
            // continue to wait for recv
            return;
        } else {
            perror("server recv");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    decrypt_ctx(buf, r, server->d_ctx);

    // handshake and transmit data
    if (server->stage == 5) {
        int s = send(remote->fd, remote->buf, r, 0);
        if (s == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no data, wait for send
                remote->buf_len = r;
                ev_io_stop(EV_A_ &server_recv_ctx->io);
                ev_io_start(EV_A_ &remote->send_ctx->io);
            } else {
                perror("server_recv_send");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
        } else if (s < r) {
            char *pt = remote->buf;
            char *et = pt + r;
            while (pt + s < et) {
                *pt = *(pt + s);
                pt++;
            }
            remote->buf_len = r - s;
            assert(remote->buf_len >= 0);
            ev_io_stop(EV_A_ &server_recv_ctx->io);
            ev_io_start(EV_A_ &remote->send_ctx->io);
        }
        return;
    } else if (server->stage == 0) {

        /*
         * Shadowsocks Protocol:
         *
         *    +------+----------+----------+
         *    | ATYP | DST.ADDR | DST.PORT |
         *    +------+----------+----------+
         *    |  1   | Variable |    2     |
         *    +------+----------+----------+
         */

        int offset = 0;
        char atyp = server->buf[offset++];
        char host[256];
        char port[64];
        memset(host, 0, 256);
        int p = 0;

        // get remote addr and port
        if (atyp == 1) {
            // IP V4
            size_t in_addr_len = sizeof(struct in_addr);
            char *a = inet_ntoa(*(struct in_addr*)(server->buf + offset));
            memcpy(host, a, strlen(a));
            offset += in_addr_len;

        } else if (atyp == 3) {
            // Domain name
            uint8_t name_len = *(uint8_t *)(server->buf + offset);
            memcpy(host, server->buf + offset + 1, name_len);
            offset += name_len + 1;

        } else {
            LOGE("unsupported addrtype: %d", atyp);
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }

        p += *(uint8_t *)(server->buf + offset++) << 8;
        p += *(uint8_t *)(server->buf + offset++);

        sprintf(port, "%d", p);

        if (verbose) {
            LOGD("connect to: %s:%s", host, port);
        }

        struct remote *remote = connect_to_remote(host, port, server->timeout);
        if (remote == NULL) {
            close_and_free_server(EV_A_ server);
            return;
        }

        server->remote = remote;
        remote->server = server;

        // listen to remote connected event
        ev_io_stop(EV_A_ &server_recv_ctx->io);
        ev_io_start(EV_A_ &remote->send_ctx->io);
        ev_timer_start(EV_A_ &remote->send_ctx->watcher);

        // XXX: should handel buffer carefully
        if (r > offset) {
            remote->buf_len = r - offset;
            memcpy(remote->buf, server->buf + offset, remote->buf_len);
        }

        server->stage = 4;

        return;
    }
    // should not reach here
    FATAL("server context error.");
}

static void server_send_cb (EV_P_ ev_io *w, int revents) {
    struct server_ctx *server_send_ctx = (struct server_ctx *)w;
    struct server *server = server_send_ctx->server;
    struct remote *remote = server->remote;

    if (remote == NULL) {
        LOGE("invalid server.");
        close_and_free_server(EV_A_ server);
        return;
    }

    if (server->buf_len == 0) {
        // close and free
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(server->fd, server->buf,
                server->buf_len, 0);
        if (s < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("server_send_send");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
            return;
        } else if (s < server->buf_len) {
            // partly sent, move memory, wait for the next time to send
            char *pt = server->buf;
            char *et = pt + server->buf_len;
            while (pt + s < et) {
                *pt = *(pt + s);
                pt++;
            }
            server->buf_len -= s;
            assert(server->buf_len >= 0);
            return;
        } else {
            // all sent out, wait for reading
            server->buf_len = 0;
            ev_io_stop(EV_A_ &server_send_ctx->io);
            if (remote != NULL) {
                ev_io_start(EV_A_ &remote->recv_ctx->io);
                return;
            } else {
                LOGE("invalid remote.");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
                return;
            }
        }
    }
}

static void remote_timeout_cb(EV_P_ ev_timer *watcher, int revents) {
    struct remote_ctx *remote_ctx = (struct remote_ctx *) (((void*)watcher)
            - sizeof(ev_io));
    struct remote *remote = remote_ctx->remote;
    struct server *server = remote->server;

    LOGE("remote timeout");

    ev_timer_stop(EV_A_ watcher);

    close_and_free_remote(EV_A_ remote);
    close_and_free_server(EV_A_ server);
}

static void remote_recv_cb (EV_P_ ev_io *w, int revents) {
    struct remote_ctx *remote_recv_ctx = (struct remote_ctx *)w;
    struct remote *remote = remote_recv_ctx->remote;
    struct server *server = remote->server;

    if (server == NULL) {
        LOGE("invalid server.");
        close_and_free_remote(EV_A_ remote);
        return;
    }

    ssize_t r = recv(remote->fd, server->buf, BUF_SIZE, 0);

    if (verbose) {
        LOGD("remote recv: %d byte", (int)r);
    }

    if (r == 0) {
        // connection closed
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data
            // continue to wait for recv
            return;
        } else {
            perror("remote recv");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    encrypt_ctx(server->buf, r, server->e_ctx);
    int s = send(server->fd, server->buf, r, 0);

    if (s == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data, wait for send
            server->buf_len = r;
            ev_io_stop(EV_A_ &remote_recv_ctx->io);
            ev_io_start(EV_A_ &server->send_ctx->io);
        } else {
            perror("remote_recv_send");
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
        }
        return;
    } else if (s < r) {
        char *pt = server->buf;
        char *et = pt + r;
        while (pt + s < et) {
            *pt = *(pt + s);
            pt++;
        }
        server->buf_len = r - s;
        assert(server->buf_len >= 0);
        ev_io_stop(EV_A_ &remote_recv_ctx->io);
        ev_io_start(EV_A_ &server->send_ctx->io);
        return;
    }
}

static void remote_send_cb (EV_P_ ev_io *w, int revents) {
    struct remote_ctx *remote_send_ctx = (struct remote_ctx *)w;
    struct remote *remote = remote_send_ctx->remote;
    struct server *server = remote->server;

    if (server == NULL) {
        LOGE("invalid server.");
        close_and_free_remote(EV_A_ remote);
        return;
    }

    if (!remote_send_ctx->connected) {

        struct sockaddr_storage addr;
        socklen_t len = sizeof addr;
        int r = getpeername(remote->fd, (struct sockaddr*)&addr, &len);
        if (r == 0) {
            if (verbose) {
                LOGD("remote connected.");
            }
            remote_send_ctx->connected = 1;
            ev_timer_stop(EV_A_ &remote_send_ctx->watcher);

            ev_io_start(EV_A_ &remote->recv_ctx->io);
            server->stage = 5;

            if (remote->buf_len == 0) {
                ev_io_stop(EV_A_ &remote_send_ctx->io);
                ev_io_start(EV_A_ &server->recv_ctx->io);
                return;
            }

        } else {
            perror("getpeername");
            // not connected
            close_and_free_remote(EV_A_ remote);
            close_and_free_server(EV_A_ server);
            return;
        }
    }

    if (remote->buf_len == 0) {
        // close and free
        close_and_free_remote(EV_A_ remote);
        close_and_free_server(EV_A_ server);
        return;
    } else {
        // has data to send
        ssize_t s = send(remote->fd, remote->buf,
                remote->buf_len, 0);
        if (s < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("remote_send_send");
                // close and free
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
            return;
        } else if (s < remote->buf_len) {
            // partly sent, move memory, wait for the next time to send
            char *pt = remote->buf;
            char *et = pt + remote->buf_len;
            while (pt + s < et) {
                *pt = *(pt + s);
                pt++;
            }
            remote->buf_len -= s;
            assert(remote->buf_len >= 0);
            return;
        } else {
            // all sent out, wait for reading
            remote->buf_len = 0;
            ev_io_stop(EV_A_ &remote_send_ctx->io);
            if (server != NULL) {
                ev_io_start(EV_A_ &server->recv_ctx->io);
            } else {
                LOGE("invalid server.");
                close_and_free_remote(EV_A_ remote);
                close_and_free_server(EV_A_ server);
            }
            return;
        }
    }
}

struct remote* new_remote(int fd, int timeout) {
    remote_conn++;
    struct remote *remote;
    remote = malloc(sizeof(struct remote));
    remote->recv_ctx = malloc(sizeof(struct remote_ctx));
    remote->send_ctx = malloc(sizeof(struct remote_ctx));
    remote->fd = fd;
    ev_io_init(&remote->recv_ctx->io, remote_recv_cb, fd, EV_READ);
    ev_io_init(&remote->send_ctx->io, remote_send_cb, fd, EV_WRITE);
    ev_timer_init(&remote->send_ctx->watcher, remote_timeout_cb, timeout, 0);
    remote->recv_ctx->remote = remote;
    remote->recv_ctx->connected = 0;
    remote->send_ctx->remote = remote;
    remote->send_ctx->connected = 0;
    remote->buf_len = 0;
    remote->server = NULL;
    return remote;
}

void free_remote(struct remote *remote) {
    remote_conn--;
    if (remote != NULL) {
        if (remote->server != NULL) {
            remote->server->remote = NULL;
        }
        free(remote->recv_ctx);
        free(remote->send_ctx);
        free(remote);
    }
}

void close_and_free_remote(EV_P_ struct remote *remote) {
    if (remote != NULL) {
        ev_timer_stop(EV_A_ &remote->send_ctx->watcher);
        ev_io_stop(EV_A_ &remote->send_ctx->io);
        ev_io_stop(EV_A_ &remote->recv_ctx->io);
        close(remote->fd);
        free_remote(remote);
    }
    if (verbose) {
        LOGD("current remote connection: %d", remote_conn);
    }
}

struct server* new_server(int fd) {
    server_conn++;
    struct server *server;
    server = malloc(sizeof(struct server));
    server->recv_ctx = malloc(sizeof(struct server_ctx));
    server->send_ctx = malloc(sizeof(struct server_ctx));
    server->fd = fd;
    ev_io_init(&server->recv_ctx->io, server_recv_cb, fd, EV_READ);
    ev_io_init(&server->send_ctx->io, server_send_cb, fd, EV_WRITE);
    server->recv_ctx->server = server;
    server->recv_ctx->connected = 0;
    server->send_ctx->server = server;
    server->send_ctx->connected = 0;
    server->stage = 0;
    if (enc_conf.method == RC4) {
        server->e_ctx = malloc(sizeof(struct rc4_state));
        server->d_ctx = malloc(sizeof(struct rc4_state));
        enc_ctx_init(server->e_ctx, 1);
        enc_ctx_init(server->d_ctx, 0);
    } else {
        server->e_ctx = NULL;
        server->d_ctx = NULL;
    }
    server->buf_len = 0;
    server->remote = NULL;
    return server;
}

void free_server(struct server *server) {
    server_conn--;
    if (server != NULL) {
        if (server->remote != NULL) {
            server->remote->server = NULL;
        }
        if (enc_conf.method == RC4) {
            free(server->e_ctx);
            free(server->d_ctx);
        }
        free(server->recv_ctx);
        free(server->send_ctx);
        free(server);
    }
}

void close_and_free_server(EV_P_ struct server *server) {
    if (server != NULL) {
        ev_io_stop(EV_A_ &server->send_ctx->io);
        ev_io_stop(EV_A_ &server->recv_ctx->io);
        close(server->fd);
        free_server(server);
    }
    if (verbose) {
        LOGD("current server connection: %d", server_conn);
    }
}

static void accept_cb (EV_P_ ev_io *w, int revents) {
    struct listen_ctx *listener = (struct listen_ctx *)w;
    int serverfd = accept(listener->fd, NULL, NULL);
    if (serverfd == -1) {
        perror("accept");
        return;
    }
    setnonblocking(serverfd);

    if (verbose) {
        LOGD("accept a connection.");
    }

    struct server *server = new_server(serverfd);
    server->timeout = listener->timeout;
    ev_io_start(EV_A_ &server->recv_ctx->io);
}

int main (int argc, char **argv) {

    int i, c;
    int pid_flags = 0;
    char *password = NULL;
    char *timeout = NULL;
    char *method = NULL;
    char *pid_path = NULL;
    char *conf_path = NULL;

    int server_num = 0;
    char *server_host[MAX_REMOTE_NUM];
    char *server_port = NULL;

    opterr = 0;

    while ((c = getopt (argc, argv, "f:s:p:l:k:t:m:c:v")) != -1) {
        switch (c) {
            case 's':
                server_host[server_num++] = optarg;
                break;
            case 'p':
                server_port = optarg;
                break;
            case 'k':
                password = optarg;
                break;
            case 'f':
                pid_flags = 1;
                pid_path = optarg;
                break;
            case 't':
                timeout = optarg;
                break;
            case 'm':
                method = optarg;
                break;
            case 'c':
                conf_path = optarg;
                break;
            case 'v':
                verbose = 1;
        }
    }

    if (opterr) {
        usage();
        exit(EXIT_FAILURE);
    }

    if (conf_path != NULL) {
        jconf_t *conf = read_jconf(conf_path);
        if (server_num == 0) {
            server_num = conf->remote_num;
            for (i = 0; i < server_num; i++) {
                server_host[i] = conf->remote_host[i];
            }
        }
        if (server_port == NULL) server_port = conf->remote_port;
        if (password == NULL) password = conf->password;
        if (method == NULL) method = conf->method;
        if (timeout == NULL) timeout = conf->timeout;
    }

    if (server_num == 0 || server_port == NULL || password == NULL) {
        usage();
        exit(EXIT_FAILURE);
    }

    if (timeout == NULL) timeout = "60";

    if (pid_flags) {
        demonize(pid_path);
    }

    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    // Setup keys
    LOGD("calculating ciphers...");
    enc_conf_init(password, method);

    // Inilitialize ev loop
    struct ev_loop *loop = EV_DEFAULT;

    // bind to each interface
    while (server_num > 0) {
        int index = --server_num;
        const char* host = server_host[index];

        // Bind to port
        int listenfd;
        listenfd = create_and_bind(host, server_port);
        if (listenfd < 0) {
            FATAL("bind() error..");
        }
        if (listen(listenfd, SOMAXCONN) == -1) {
            FATAL("listen() error.");
        }
        setnonblocking(listenfd);
        LOGD("server listening at port %s.", server_port);

        // Setup proxy context
        struct listen_ctx listen_ctx;
        listen_ctx.timeout = timeout;
        listen_ctx.fd = listenfd;

        ev_io_init (&listen_ctx.io, accept_cb, listenfd, EV_READ);
        ev_io_start (loop, &listen_ctx.io);
    }

    // start ev loop
    ev_run (loop, 0);
    return 0;
}

