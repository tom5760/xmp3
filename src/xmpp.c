/**
 * xmp3 - XMPP Proxy
 * xmpp.{c,h} - Implements the server part of a normal XMPP server.
 * Copyright (c) 2011 Drexel University
 * @file
 */

#include "xmpp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <expat.h>

#include "utlist.h"
#include "uthash.h"

#include "log.h"
#include "utils.h"
#include "event.h"

#include "xmpp_common.h"
#include "xmpp_auth.h"
#include "xmpp_core.h"
#include "xmpp_im.h"

#include "xep_muc.h"

#define BUFFER_SIZE 2000

static const int SERVER_BACKLOG = 3;

static char MSG_BUFFER[BUFFER_SIZE];

/** Holds data on how to send a stanza to a particular JID. */
struct stanza_route {
    /** The JID to send to. */
    const struct jid *jid;

    /** The function that will deliver the stanza. */
    xmpp_stanza_callback func;

    /** Arbitrary data. */
    void *data;

    /** @{ These are kept in a doubly-linked list. */
    struct stanza_route *prev;
    struct stanza_route *next;
    /** @} */
};

/**
 * Holds data on how to handle a particular iq stanza.
 *
 * This is determined by the namespace + tag name of the first child element
 * (the spec says there can only be one child).
 */
struct iq_route {
    /** The namespace + name of the tag to match. */
    const char *ns;

    /** The function that will deliver the stanza. */
    xmpp_iq_callback func;

    /** Arbitrary data that callbacks can use. */
    void *data;

    /** These are kept in a hash table. */
    UT_hash_handle hh;
};

/** Holds data for a XMPP server instance. */
struct xmpp_server {
    /** The bound and listening file descriptor. */
    int fd;

    /** The OpenSSL context. */
    SSL_CTX *ssl_context;

    /** Multi-User Chat component. */
    struct xep_muc *muc;

    /** Linked list of connected clients. */
    struct xmpp_client *clients;

    /** Linked list of stanza routes. */
    struct stanza_route *stanza_routes;

    /** Hash table of iq routes. */
    struct iq_route *iq_routes;
};

// Forward declarations
static struct xmpp_server* new_server(
        int fd, const struct xmp3_options *options);
static struct xmpp_client* new_client(struct xmpp_server *server);
static void del_client(struct xmpp_client *client);
static void read_client(struct event_loop *loop, int fd, void *data);

static void add_connection(struct event_loop *loop, int fd, void *data);
static void remove_connection(struct xmpp_server *server,
                              const struct xmpp_client *client);

static struct stanza_route* find_stanza_route(
        const struct xmpp_server *server, const struct jid *jid);

struct xmpp_server* xmpp_init(struct event_loop *loop,
                              const struct xmp3_options *options) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct xmpp_server *server = NULL;
    check(fd != -1, "Error creating XMPP server socket");

    // Allow address reuse when in the TIME_WAIT state.
    static const int on = 1;
    check(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != -1,
          "Error setting SO_REUSEADDR on server socket");

    // Convert to network byte order
    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port = htons(xmp3_options_get_port(options)),
        .sin_addr = xmp3_options_get_addr(options),
    };

    check(bind(fd, (struct sockaddr*)&saddr, sizeof(saddr)) != -1,
          "XMPP server socket bind error");
    check(listen(fd, SERVER_BACKLOG) != -1, "XMPP server socket listen error");

    log_info("Listening for XMPP connections on %s:%d",
             inet_ntoa(xmp3_options_get_addr(options)),
             xmp3_options_get_port(options));

    server = new_server(fd, options);

    // Register initial routes and callbacks
    xmpp_register_stanza_route(server, &SERVER_JID,
                               xmpp_core_stanza_handler, NULL);


    server->muc = xep_muc_new();
    xmpp_register_stanza_route(server, &MUC_JID,
                               xep_muc_stanza_handler, server->muc);

    xmpp_register_iq_namespace(server, XMPP_IQ_SESSION,
                               xmpp_im_iq_session, NULL);
    xmpp_register_iq_namespace(server, XMPP_IQ_QUERY_ROSTER,
                               xmpp_im_iq_roster_query, NULL);
    xmpp_register_iq_namespace(server, XMPP_IQ_DISCO_QUERY_INFO,
                               xmpp_im_iq_disco_query_info, NULL);
    xmpp_register_iq_namespace(server, XMPP_IQ_DISCO_QUERY_ITEMS,
                               xmpp_im_iq_disco_query_items, NULL);

    /* Register the event callback so we can be notified of when the client
     * sends more data. */
    event_register_callback(loop, fd, add_connection, server);
    return server;

error:
    close(fd);
    if (server != NULL) {
        xmpp_shutdown(server);
    }
    return NULL;
}

void xmpp_shutdown(struct xmpp_server *server) {
    SSL_CTX_free(server->ssl_context);

    xep_muc_del(server->muc);

    struct xmpp_client *client;
    struct xmpp_client *client_tmp;
    DL_FOREACH_SAFE(server->clients, client, client_tmp) {
        DL_DELETE(server->clients, client);
        del_client(client);
    }

    struct stanza_route *route;
    struct stanza_route *route_tmp;
    DL_FOREACH_SAFE(server->stanza_routes, route, route_tmp) {
        DL_DELETE(server->stanza_routes, route);
        free(route);
    }

    struct iq_route *iq_route;
    struct iq_route *iq_route_tmp;
    HASH_ITER(hh, server->iq_routes, iq_route, iq_route_tmp) {
        HASH_DEL(server->iq_routes, iq_route);
        free(iq_route);
    }

    free(server);
}

void xmpp_new_ssl_connection(struct xmpp_client *client) {
    struct xmpp_server *server = client->server;

    struct client_socket *ssl_socket = client_socket_ssl_new(
            server->ssl_context, client->socket);

    client_socket_del(client->socket);
    client->socket = ssl_socket;
}

void xmpp_register_stanza_route(struct xmpp_server *server,
                                const struct jid *jid,
                                xmpp_stanza_callback func, void *data) {
    struct stanza_route *route = find_stanza_route(server, jid);
    if (route != NULL) {
        log_warn("Attempted to insert duplicate stanza route");
        return;
    }

    route = calloc(1, sizeof(*route));
    check_mem(route);
    route->jid = jid;
    route->func = func;
    route->data = data;

    DL_APPEND(server->stanza_routes, route);
}

void xmpp_deregister_stanza_route(struct xmpp_server *server,
                                  const struct jid *jid) {
    struct stanza_route *route = find_stanza_route(server, jid);
    if (route == NULL) {
        log_warn("Attempted to remove non-existent stanza route");
        return;
    }
    DL_DELETE(server->stanza_routes, route);
    free(route);
}

bool xmpp_route_stanza(struct xmpp_stanza *stanza) {
    struct xmpp_server *server = stanza->from_client->server;
    struct stanza_route *route = find_stanza_route(server, &stanza->to_jid);
    if (route == NULL) {
        log_info("No route for destination");
        return false;
    }
    return route->func(stanza, route->data);
}

void xmpp_register_iq_namespace(struct xmpp_server *server, const char *ns,
                                xmpp_iq_callback func, void *data) {
    struct iq_route *route = NULL;
    HASH_FIND_STR(server->iq_routes, ns, route);
    if (route != NULL) {
        log_warn("Attempted to insert duplicate iq route");
        return;
    }

    route = calloc(1, sizeof(*route));
    check_mem(route);
    route->ns = ns;
    route->func = func;
    route->data = data;
    HASH_ADD_KEYPTR(hh, server->iq_routes, route->ns, strlen(route->ns),
                    route);
}

void xmpp_deregister_iq_namespace(struct xmpp_server *server, const char *ns) {
    struct iq_route *route = NULL;
    HASH_FIND_STR(server->iq_routes, ns, route);
    if (route == NULL) {
        log_warn("Attempted to remove non-existent iq route");
        return;
    }
    HASH_DEL(server->iq_routes, route);
    free(route);
}

bool xmpp_route_iq(const char *ns, struct xmpp_stanza *stanza) {
    struct xmpp_server *server = stanza->from_client->server;
    struct iq_route *route = NULL;
    HASH_FIND_STR(server->iq_routes, ns, route);
    if (route == NULL) {
        log_info("No iq route for destination");
        return false;
    }
    return route->func(stanza, route->data);
}

static struct xmpp_server* new_server(
        int fd, const struct xmp3_options *options) {
    struct xmpp_server *server = calloc(1, sizeof(*server));
    check_mem(server);

    server->fd = fd;

    // Initialize OpenSSL context
    server->ssl_context = SSL_CTX_new(SSLv23_server_method());
    if (server->ssl_context == NULL) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    if (SSL_CTX_use_certificate_chain_file(server->ssl_context,
                xmp3_options_get_certificate(options)) != 1) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    if (SSL_CTX_use_PrivateKey_file(server->ssl_context,
                xmp3_options_get_keyfile(options), SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    if (SSL_CTX_check_private_key(server->ssl_context) != 1) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    return server;
}

static struct xmpp_client* new_client(struct xmpp_server *server) {
    struct xmpp_client *client = calloc(1, sizeof(*client));
    check_mem(client);

    client->authenticated = false;
    client->connected = true;
    client->server = server;

    // Create the XML parser we'll use to parse stanzas from the client.
    client->parser = XML_ParserCreateNS(NULL, *XMPP_NS_SEPARATOR);
    check(client->parser != NULL, "Error creating XML parser");

    return client;

error:
    del_client(client);
    return NULL;
}

static void del_client(struct xmpp_client *client) {
    if (client == NULL) {
        return;
    }
    xmpp_deregister_stanza_route(client->server, &client->jid);
    client_socket_close(client->socket);
    client_socket_del(client->socket);
    XML_ParserFree(client->parser);
    free(client->jid.local);
    free(client->jid.domain);
    free(client->jid.resource);
    free(client);
}

/**
 * Event loop callback when we receive new data from a client.
 *
 * @param loop Event loop instance.
 * @param fd   The client file descriptor.
 * @param data A struct xmpp_client instance.
 */
static void read_client(struct event_loop *loop, int fd, void *data) {
    struct xmpp_client *client = (struct xmpp_client*)data;
    struct xmpp_server *server = client->server;

    ssize_t numrecv = client_socket_recv(client->socket, MSG_BUFFER,
                                         sizeof(MSG_BUFFER));

    if (numrecv == 0 || numrecv == -1) {
        switch (numrecv) {
            case 0:
                log_info("%s:%d disconnected",
                         inet_ntoa(client->caddr.sin_addr),
                                   client->caddr.sin_port);
                break;
            case -1:
                log_err("Error reading from %s:%d: %s",
                        inet_ntoa(client->caddr.sin_addr),
                        client->caddr.sin_port, strerror(errno));
                break;
        }
        goto error;
    }

    log_info("%s:%d - Read %zd bytes", inet_ntoa(client->caddr.sin_addr),
             client->caddr.sin_port, numrecv);
    xmpp_print_data(MSG_BUFFER, numrecv);
    enum XML_Status status = XML_Parse(client->parser, MSG_BUFFER,
                                       numrecv, false);
    check(status != XML_STATUS_ERROR, "Error parsing XML: %s",
          XML_ErrorString(XML_GetErrorCode(client->parser)));

    /* If an error occurred which caused the client to disconnect, clean up
     * after it. */
    if (!client->connected) {
        goto error;
    }

    return;

error:
    event_deregister_callback(loop, fd);
    remove_connection(server, client);
}

static void add_connection(struct event_loop *loop, int fd, void *data) {
    struct xmpp_server *server = (struct xmpp_server*)data;
    struct xmpp_client *client = new_client(server);

    socklen_t addrlen = sizeof(client->caddr);
    int client_fd = accept(fd, (struct sockaddr*)&client->caddr, &addrlen);
    check(client_fd != -1, "Error accepting client connection");

    client->socket = client_socket_new(client_fd);

    /* The first Expat callback should be to handle the start of the XML
     * stream, and begin authentication. */
    XML_SetElementHandler(client->parser, xmpp_auth_stream_start,
                           xmpp_error_end);
    XML_SetCharacterDataHandler(client->parser, xmpp_error_data);
    XML_SetUserData(client->parser, client);

    log_info("New connection from %s:%d",
             inet_ntoa(client->caddr.sin_addr), client->caddr.sin_port);

    DL_APPEND(server->clients, client);
    event_register_callback(loop, client_fd, read_client, client);
    return;

error:
    del_client(client);
}

static void remove_connection(struct xmpp_server *server,
                              const struct xmpp_client *client) {
    struct xmpp_client *item;
    DL_FOREACH(server->clients, item) {
        if (client == item) {
            DL_DELETE(server->clients, item);
            del_client(item);
            return;
        }
    }
}

/**
 * Searches for a matching JID route given a full or bare JID.
 *
 * If given a bare JID, matches the first full or bare JID found.
 *
 * @param server XMPP server instance to search on.
 * @param jid    Full or bare JID to search for.
 * @return The route to send this stanza to.
 */
static struct stanza_route* find_stanza_route(
        const struct xmpp_server *server, const struct jid *jid) {
    struct stanza_route *route;

    if (jid->domain == NULL) {
        log_err("Tried to find a route for a jid with no domain");
        return NULL;
    }

    char *strjid = jid_to_str(jid);
    debug("Looking for route to \"%s\"", strjid);
    free(strjid);

    DL_FOREACH(server->stanza_routes, route) {
        strjid = jid_to_str(route->jid);
        debug("Checking \"%s\"", strjid);
        free(strjid);

        // First, match the domain parts
        if (strcmp(route->jid->domain, "*") != 0 &&
            strcmp(jid->domain, route->jid->domain) != 0) {
            debug("A");
            continue;
        }
        // Next, match the local part
        if (jid->local != NULL && (route->jid->local == NULL
            || (strcmp(route->jid->local, "*") != 0
                && strcmp(jid->local, route->jid->local) != 0))) {
            debug("B");
            continue;
        }
        if (jid->resource != NULL && (route->jid->resource == NULL
            || (strcmp(route->jid->resource, "*") != 0
                && strcmp(jid->resource, route->jid->resource) != 0))) {
            debug("C");
            continue;
        }
        return route;
    }
    return NULL;
}
