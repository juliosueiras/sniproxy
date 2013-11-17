/*
 * Copyright (c) 2011 and 2012, Dustin Lundquist <dustin@null-ptr.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> /* getaddrinfo */
#include <unistd.h> /* close */
#include <fcntl.h>
#include <arpa/inet.h>
#include <ev.h>
#include <assert.h>
#include "connection.h"
#include "address.h"
#include "logger.h"


#define IS_TEMPORARY_SOCKERR(_errno) (_errno == EAGAIN || \
                                      _errno == EWOULDBLOCK || \
                                      _errno == EINTR)


static TAILQ_HEAD(ConnectionHead, Connection) connections;


static inline int client_socket_open(const struct Connection *);
static inline int server_socket_open(const struct Connection *);

static void reactivate_watcher(struct ev_loop *, struct ev_io *,
        const struct Buffer *, const struct Buffer *);

static void connection_cb(struct ev_loop *, struct ev_io *, int);
static void parse_client_request(struct Connection *, struct ev_loop *);
static void resolve_server_address(struct Connection *, struct ev_loop *);
static void initiate_server_connect(struct Connection *, struct ev_loop *);
static void close_connection(struct Connection *, struct ev_loop *);
static void close_client_socket(struct Connection *, struct ev_loop *);
static void close_server_socket(struct Connection *, struct ev_loop *);
static struct Connection *new_connection();
static void free_connection(struct Connection *);
static void print_connection(FILE *, const struct Connection *);


void
init_connections() {
    TAILQ_INIT(&connections);
}

/*
 * Accept a new incoming connection
 */
void
accept_connection(const struct Listener *listener, struct ev_loop *loop) {
    struct Connection *con = new_connection();
    if (con == NULL) {
        err("new_connection failed");
        return;
    }

    int sockfd = accept(listener->watcher.fd,
                    (struct sockaddr *)&con->client.addr,
                    &con->client.addr_len);
    if (sockfd < 0) {
        warn("accept failed: %s", strerror(errno));
        free_connection(con);
        return;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    /* Avoiding type-punned pointer warning */
    struct ev_io *client_watcher = &con->client.watcher;
    ev_io_init(client_watcher, connection_cb, sockfd, EV_READ);
    con->client.watcher.data = con;
    con->state = ACCEPTED;
    con->listener = listener;

    TAILQ_INSERT_HEAD(&connections, con, entries);

    ev_io_start(loop, client_watcher);
}

/*
 * Close and free all connections
 */
void
free_connections(struct ev_loop *loop) {
    struct Connection *iter;
    while ((iter = TAILQ_FIRST(&connections)) != NULL) {
        TAILQ_REMOVE(&connections, iter, entries);
        close_connection(iter, loop);
        free_connection(iter);
    }
}

/* dumps a list of all connections for debugging */
void
print_connections() {
    char filename[] = "/tmp/sniproxy-connections-XXXXXX";

    int fd = mkstemp(filename);
    if (fd < 0) {
        warn("mkstemp failed: %s", strerror(errno));
        return;
    }

    FILE *temp = fdopen(fd, "w");
    if (temp == NULL) {
        warn("fdopen failed: %s", strerror(errno));
        return;
    }

    fprintf(temp, "Running connections:\n");
    struct Connection *iter;
    TAILQ_FOREACH(iter, &connections, entries)
        print_connection(temp, iter);

    if (fclose(temp) < 0)
        warn("fclose failed: %s", strerror(errno));

    notice("Dumped connections to %s", filename);
}

/*
 * Test is client socket is open
 *
 * Returns true iff the client socket is opened based on connection state.
 */
static inline int
client_socket_open(const struct Connection *con) {
    return con->state == ACCEPTED ||
        con->state == PARSED ||
        con->state == RESOLVED ||
        con->state == CONNECTED ||
        con->state == SERVER_CLOSED;
}

/*
 * Test is server socket is open
 *
 * Returns true iff the server socket is opened based on connection state.
 */
static inline int
server_socket_open(const struct Connection *con) {
    return con->state == CONNECTED ||
        con->state == CLIENT_CLOSED;
}

/*
 * Main client callback: this is used by both the client and server watchers
 *
 * The logic is almost the same except for:
 *  + input buffer
 *  + output buffer
 *  + how to close the socket
 *
 */
static void
connection_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
    struct Connection *con = (struct Connection *)w->data;
    int is_client = &con->client.watcher == w;
    struct Buffer *input_buffer =
        is_client ? con->client.buffer : con->server.buffer;
    struct Buffer *output_buffer =
        is_client ? con->server.buffer : con->client.buffer;
    void (*close_socket)(struct Connection *, struct ev_loop *) =
        is_client ? close_client_socket : close_server_socket;

    /* Receive first in case the socket was closed */
    if (revents & EV_READ && buffer_room(input_buffer)) {
        ssize_t bytes_received = buffer_recv(input_buffer, w->fd, 0);
        if (bytes_received < 0 && !IS_TEMPORARY_SOCKERR(errno)) {
            warn("recv(): %s, closing connection",
                    strerror(errno));

            close_socket(con, loop);
            revents = 0; /* Clear revents so we don't try to send */
        } else if (bytes_received == 0) { /* peer closed socket */
            close_socket(con, loop);
            revents = 0;
        }
    }

    /* Transmit */
    if (revents & EV_WRITE && buffer_len(output_buffer)) {
        ssize_t bytes_transmitted = buffer_send(output_buffer, w->fd, 0);
        if (bytes_transmitted < 0 && !IS_TEMPORARY_SOCKERR(errno)) {
            warn("send(): %s, closing connection",
                    strerror(errno));

            close_socket(con, loop);
        }
    }

    /* Handle any state specific logic */
    if (is_client && con->state == ACCEPTED)
        parse_client_request(con, loop);
    if (is_client && con->state == PARSED)
        resolve_server_address(con, loop);
    if (is_client && con->state == RESOLVED)
        initiate_server_connect(con, loop);

    /* Close other socket if we have flushed corresponding buffer */
    if (con->state == SERVER_CLOSED && buffer_len(con->server.buffer) == 0)
        close_client_socket(con, loop);
    if (con->state == CLIENT_CLOSED && buffer_len(con->client.buffer) == 0)
        close_server_socket(con, loop);

    if (con->state == CLOSED) {
        TAILQ_REMOVE(&connections, con, entries);
        /* TODO log connection */
        free_connection(con);
        return;
    }

    struct ev_io *client_watcher = &con->client.watcher;
    struct ev_io *server_watcher = &con->server.watcher;

    /* Reactivate watchers */
    if (client_socket_open(con))
        reactivate_watcher(loop, client_watcher,
                con->client.buffer, con->server.buffer);

    if (server_socket_open(con))
        reactivate_watcher(loop, server_watcher,
                con->server.buffer, con->client.buffer);

    /* Neither watcher is active when the corresponding socket is closed */
    assert(client_socket_open(con) || !ev_is_active(client_watcher));
    assert(server_socket_open(con) || !ev_is_active(server_watcher));

    /* At least one watcher is still active for this connection */
    assert((ev_is_active(client_watcher) && con->client.watcher.events) ||
           (ev_is_active(server_watcher) && con->server.watcher.events));

    ev_verify(loop);

    /* Move to head of queue, so we can find inactive connections */
    TAILQ_REMOVE(&connections, con, entries);
    TAILQ_INSERT_HEAD(&connections, con, entries);
}

static void
reactivate_watcher(struct ev_loop *loop, struct ev_io *w,
        const struct Buffer *input_buffer,
        const struct Buffer *output_buffer) {
    int events = 0;

    if (buffer_room(input_buffer))
        events |= EV_READ;

    if (buffer_len(output_buffer))
        events |= EV_WRITE;

    if (ev_is_active(w)) {
        if (events == 0)
            ev_io_stop(loop, w);
        else if (events != w->events) {
            ev_io_stop(loop, w);
            ev_io_set(w, w->fd, events);
            ev_io_start(loop, w);
        }
    } else if (events != 0) {
        ev_io_set(w, w->fd, events);
        ev_io_start(loop, w);
    }
}

static void
parse_client_request(struct Connection *con, struct ev_loop *loop) {
    char buffer[1460]; /* TCP MSS over standard Ethernet and IPv4 */
    ssize_t len = buffer_peek(con->client.buffer, buffer, sizeof(buffer));

    char *hostname = NULL;
    int result = con->listener->parse_packet(buffer, len, &hostname);
    if (result == -1) {
        return;  /* incomplete request: try again */
    } else if (result == -2) {
        char client[INET6_ADDRSTRLEN + 8];
        info("Request from %s did not include a hostname",
                display_sockaddr(&con->client.addr, client, sizeof(client)));

        if (con->listener->fallback_address == NULL) {
            close_client_socket(con, loop);
            return;
        }
    } else if (result < -2) {
        char client[INET6_ADDRSTRLEN + 8];
        warn("Unable to parse request from %s",
                display_sockaddr(&con->client.addr, client, sizeof(client)));
        debug("parse() returned %d", result);
        /* TODO optionally dump request to file */

        if (con->listener->fallback_address == NULL) {
            close_client_socket(con, loop);
            return;
        }
    }

    con->hostname = hostname;
    con->state = PARSED;
}

static void
resolve_server_address(struct Connection *con, struct ev_loop *loop) {
    /* TODO avoid extra malloc in listener_lookup_server_address() */
    struct Address *server_address =
        listener_lookup_server_address(con->listener, con->hostname);

    if (address_is_hostname(server_address)) {
        warn("DNS lookups not supported at this time");
        close_client_socket(con, loop);
        return;
    }

    assert(address_is_sockaddr(server_address));
    con->server.addr_len = address_sa_len(server_address);
    assert(con->server.addr_len <= sizeof(con->server.addr));
    memcpy(&con->server.addr, address_sa(server_address), con->server.addr_len);

    free(server_address);

    con->state = RESOLVED;
}

static void
initiate_server_connect(struct Connection *con, struct ev_loop *loop) {
    int sockfd = socket(con->server.addr.ss_family, SOCK_STREAM, 0);
    if (sockfd < 0) {
        warn("socket failed");
        return;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    int result = connect(sockfd,
            (struct sockaddr *)&con->server.addr,
            con->server.addr_len);
    if (result < 0 && errno != EINPROGRESS) {
        close(sockfd);
        char server[INET6_ADDRSTRLEN + 8];
        warn("Failed to open connection to %s: %s",
                display_sockaddr(&con->server.addr, server, sizeof(server)),
                strerror(errno));

        con->state = SERVER_CLOSED;
        return;
    }

    struct ev_io *server_watcher = &con->server.watcher;
    ev_io_init(server_watcher, connection_cb, sockfd, EV_WRITE);
    con->server.watcher.data = con;
    con->state = CONNECTED;

    ev_io_start(loop, server_watcher);
}

/* Close client socket.
 * Caller must ensure that it has not been closed before.
 */
static void
close_client_socket(struct Connection *con, struct ev_loop *loop) {
    assert(con->state != CLOSED
            && con->state != CLIENT_CLOSED);

    ev_io_stop(loop, &con->client.watcher);

    if (close(con->client.watcher.fd) < 0)
        warn("close failed: %s", strerror(errno));

    /* next state depends on previous state */
    if (con->state == SERVER_CLOSED
            || con->state == ACCEPTED
            || con->state == PARSED
            || con->state == RESOLVED)
        con->state = CLOSED;
    else
        con->state = CLIENT_CLOSED;
}

/* Close server socket.
 * Caller must ensure that it has not been closed before.
 */
static void
close_server_socket(struct Connection *con, struct ev_loop *loop) {
    assert(con->state != CLOSED
            && con->state != SERVER_CLOSED);

    ev_io_stop(loop, &con->server.watcher);

    if (close(con->server.watcher.fd) < 0)
        warn("close failed: %s", strerror(errno));

    /* next state depends on previous state */
    if (con->state == CLIENT_CLOSED)
        con->state = CLOSED;
    else
        con->state = SERVER_CLOSED;
}

static void
close_connection(struct Connection *con, struct ev_loop *loop) {
    assert(con->state != NEW); /* only used during initialization */

    if (con->state == CONNECTED
            || con->state == CLIENT_CLOSED)
        close_server_socket(con, loop);

    assert(con->state == ACCEPTED
            || con->state == PARSED
            || con->state == RESOLVED
            || con->state == SERVER_CLOSED
            || con->state == CLOSED);

    if (con->state == ACCEPTED
            || con->state == PARSED
            || con->state == RESOLVED
            || con->state == SERVER_CLOSED)
        close_client_socket(con, loop);

    assert(con->state == CLOSED);
}

/*
 * Allocate and initialize a new connection
 */
static struct Connection *
new_connection() {
    struct Connection *con = calloc(1, sizeof(struct Connection));
    if (con == NULL)
        return NULL;

    con->state = NEW;
    con->client.addr_len = sizeof(con->client.addr);
    con->server.addr_len = sizeof(con->server.addr);
    con->hostname = NULL;

    con->client.buffer = new_buffer();
    if (con->client.buffer == NULL) {
        free_connection(con);
        return NULL;
    }

    con->server.buffer = new_buffer();
    if (con->server.buffer == NULL) {
        free_connection(con);
        return NULL;
    }

    return con;
}

/*
 * Free a connection and associated data
 *
 * Requires that no watchers remain active
 */
static void
free_connection(struct Connection *con) {
    if (con == NULL)
        return;

    free_buffer(con->client.buffer);
    free_buffer(con->server.buffer);
    free((void *)con->hostname); /* cast away const'ness */
    free(con);
}

static void
print_connection(FILE *file, const struct Connection *con) {
    char client[INET6_ADDRSTRLEN + 8];
    char server[INET6_ADDRSTRLEN + 8];

    switch (con->state) {
        case NEW:
            fprintf(file, "NEW           -\t-\n");
            break;
        case ACCEPTED:
            fprintf(file, "ACCEPTED      %s %zu/%zu\t-\n",
                    display_sockaddr(&con->client.addr, client, sizeof(client)),
                    con->client.buffer->len, con->client.buffer->size);
            break;
        case PARSED:
            fprintf(file, "PARSED        %s %zu/%zu\t-\n",
                    display_sockaddr(&con->client.addr, client, sizeof(client)),
                    con->client.buffer->len, con->client.buffer->size);
            break;
        case RESOLVED:
            fprintf(file, "RESOLVED      %s %zu/%zu\t-\n",
                    display_sockaddr(&con->client.addr, client, sizeof(client)),
                    con->client.buffer->len, con->client.buffer->size);
            break;
        case CONNECTED:
            fprintf(file, "CONNECTED     %s %zu/%zu\t%s %zu/%zu\n",
                    display_sockaddr(&con->client.addr, client, sizeof(client)),
                    con->client.buffer->len, con->client.buffer->size,
                    display_sockaddr(&con->server.addr, server, sizeof(server)),
                    con->server.buffer->len, con->server.buffer->size);
            break;
        case SERVER_CLOSED:
            fprintf(file, "SERVER_CLOSED %s %zu/%zu\t-\n",
                    display_sockaddr(&con->client.addr, client, sizeof(client)),
                    con->client.buffer->len, con->client.buffer->size);
            break;
        case CLIENT_CLOSED:
            fprintf(file, "CLIENT_CLOSED -\t%s %zu/%zu\n",
                    display_sockaddr(&con->server.addr, server, sizeof(server)),
                    con->server.buffer->len, con->server.buffer->size);
            break;
        case CLOSED:
            fprintf(file, "CLOSED        -\t-\n");
            break;
    }
}
