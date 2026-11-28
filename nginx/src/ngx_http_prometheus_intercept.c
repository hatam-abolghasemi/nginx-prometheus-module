/*
 * HTTPS interception for the outbound proxy: see inside a CONNECT tunnel.
 *
 *   prometheus_intercept_connect [tls=<addr>] [h2c=<addr>];
 *                                          in the CONNECT location of :3128
 *   prometheus_intercept_ca <crt> <key>;   http{}: the CA leaf certs are minted from
 *   $prometheus_intercept_cert / _key      the leaf for this connection's name,
 *                                          for ssl_certificate data:$...
 *   prometheus_intercept_trusted <bundle>; http{}: the CAs destinations are
 *                                          verified against, read on demand
 *
 * prometheus_intercept_connect answers "200 Connection Established" first and
 * reads the bytes the client sends into the tunnel. Their first bytes say what
 * the tunnel carries: a TLS ClientHello goes to the tls= terminator
 * (127.0.0.1:3129), the HTTP/2 preface (plaintext gRPC) to the h2c= one
 * (127.0.0.1:3130), both behind a PROXY protocol line whose destination port
 * is the CONNECT's port (and whose destination address is the CONNECT's host
 * when that is an IP literal). Anything else is relayed to the real target,
 * resolved with the location's resolver; IPv4 answers are preferred. A
 * terminator is any address nginx can connect to: host:port, [v6]:port,
 * unix:/path.
 * Clients speak first in TLS, HTTP/1 and HTTP/2, so the read adds no wait.
 *
 * A tunnel handed to a terminator is not recorded as a CONNECT: each request
 * inside it is recorded there, as mode 1 with its real method, route and
 * status. A tunnel relayed as is is recorded as CONNECT host:port, protocol
 * "tunnel", like tunnel_pass.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_HTTP_SSL)
#include <openssl/x509v3.h>
#endif

#include "ngx_http_prometheus.h"


#define NGX_PROM_TUNNEL_BUF      16384
#define NGX_PROM_MINT_CACHE      512


typedef struct ngx_prom_trust_s  ngx_prom_trust_t;

typedef struct {
    ngx_str_t                  ca_cert;
    ngx_str_t                  ca_key;
    ngx_prom_trust_t          *trust;
} ngx_prom_intercept_main_conf_t;


typedef struct {
    ngx_flag_t                 connect;
    ngx_flag_t                 resolve;
    ngx_msec_t                 timeout;
    ngx_msec_t                 connect_timeout;
    ngx_addr_t                *tls;         /* the terminators */
    ngx_addr_t                *h2c;
} ngx_prom_intercept_loc_conf_t;


typedef struct {
    ngx_http_request_t        *request;
    ngx_peer_connection_t      peer;
    ngx_buf_t                 *up;          /* client -> target */
    ngx_buf_t                 *down;        /* target -> client */
    ngx_str_t                  host;
    in_port_t                  port;
    ngx_uint_t                 intercepted; /* handed to a terminator */
    ngx_uint_t                 done;
    ngx_uint_t                 connected;
    ngx_uint_t                 inet6;       /* host is an IPv6 literal */
    ngx_sockaddr_t             sa;
    socklen_t                  socklen;
} ngx_prom_tunnel_t;


static ngx_int_t ngx_prom_intercept_add_variables(ngx_conf_t *cf);
static void *ngx_prom_intercept_create_main_conf(ngx_conf_t *cf);
static void *ngx_prom_intercept_create_loc_conf(ngx_conf_t *cf);
static char *ngx_prom_intercept_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_prom_intercept_connect(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_prom_intercept_ca(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_prom_intercept_origin(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_prom_intercept_trusted(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_prom_intercept_postconf(ngx_conf_t *cf);
static ngx_int_t ngx_prom_intercept_init_worker(ngx_cycle_t *cycle);
static void ngx_prom_intercept_exit_worker(ngx_cycle_t *cycle);


static ngx_command_t  ngx_prom_intercept_commands[] = {

    { ngx_string("prometheus_intercept_connect"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_NOARGS|NGX_CONF_TAKE12,
      ngx_prom_intercept_connect,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_intercept_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_prom_intercept_loc_conf_t, timeout),
      NULL },

    { ngx_string("prometheus_intercept_connect_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_prom_intercept_loc_conf_t, connect_timeout),
      NULL },

    { ngx_string("prometheus_intercept_resolve"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_prom_intercept_loc_conf_t, resolve),
      NULL },

    { ngx_string("prometheus_intercept_origin"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_prom_intercept_origin,
      0,
      0,
      NULL },

    { ngx_string("prometheus_intercept_ca"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_prom_intercept_ca,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_intercept_trusted"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_prom_intercept_trusted,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_prom_intercept_module_ctx = {
    ngx_prom_intercept_add_variables,      /* preconfiguration */
    ngx_prom_intercept_postconf,           /* postconfiguration */
    ngx_prom_intercept_create_main_conf,   /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
    ngx_prom_intercept_create_loc_conf,    /* create location configuration */
    ngx_prom_intercept_merge_loc_conf      /* merge location configuration */
};


ngx_module_t  ngx_http_prometheus_intercept_module = {
    NGX_MODULE_V1,
    &ngx_prom_intercept_module_ctx,        /* module context */
    ngx_prom_intercept_commands,           /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_prom_intercept_init_worker,        /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_prom_intercept_exit_worker,        /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


ngx_uint_t
ngx_prom_intercept_skip_log(ngx_http_request_t *r)
{
    ngx_prom_tunnel_t  *t;

    if (r->method != NGX_HTTP_CONNECT) {
        return 0;
    }

    t = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);

    return t != NULL && t->intercepted;
}


/* ---------------------------------------------------------------- the relay */

static void ngx_prom_tunnel_finish(ngx_prom_tunnel_t *t, ngx_uint_t status);


static ngx_int_t
ngx_prom_tunnel_pump(ngx_connection_t *src, ngx_connection_t *dst,
    ngx_buf_t *b)
{
    ssize_t  n;

    for ( ;; ) {

        if (b->pos < b->last && dst->write->ready) {
            n = dst->send(dst, b->pos, b->last - b->pos);

            if (n == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (n > 0) {
                b->pos += n;

                if (b->pos == b->last) {
                    b->pos = b->start;
                    b->last = b->start;
                }

                continue;
            }
        }

        if (b->last < b->end && src->read->ready && !src->read->eof) {
            n = src->recv(src, b->last, b->end - b->last);

            if (n == NGX_AGAIN) {
                break;
            }

            if (n == 0 || n == NGX_ERROR) {
                src->read->eof = 1;
                break;
            }

            b->last += n;
            continue;
        }

        break;
    }

    return NGX_OK;
}


static void
ngx_prom_tunnel_relay(ngx_prom_tunnel_t *t)
{
    ngx_connection_t               *c, *pc;
    ngx_prom_intercept_loc_conf_t  *ilcf;

    if (t->done || !t->connected) {
        return;                 /* the client's bytes wait until it is up */
    }

    c = t->request->connection;
    pc = t->peer.connection;

    if (ngx_prom_tunnel_pump(c, pc, t->up) != NGX_OK
        || ngx_prom_tunnel_pump(pc, c, t->down) != NGX_OK)
    {
        ngx_prom_tunnel_finish(t, 0);
        return;
    }

    /* either side closed and what it sent has been delivered */
    if ((c->read->eof && t->up->pos == t->up->last)
        || (pc->read->eof && t->down->pos == t->down->last))
    {
        ngx_prom_tunnel_finish(t, 0);
        return;
    }

    if (ngx_handle_read_event(c->read, 0) != NGX_OK
        || ngx_handle_read_event(pc->read, 0) != NGX_OK
        || ngx_handle_write_event(c->write, 0) != NGX_OK
        || ngx_handle_write_event(pc->write, 0) != NGX_OK)
    {
        ngx_prom_tunnel_finish(t, 0);
        return;
    }

    ilcf = ngx_http_get_module_loc_conf(t->request,
                                        ngx_http_prometheus_intercept_module);

    ngx_add_timer(c->read, ilcf->timeout);
}


static void
ngx_prom_tunnel_client_read(ngx_http_request_t *r)
{
    ngx_prom_tunnel_t  *t;

    t = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);

    if (r->connection->read->timedout) {
        ngx_prom_tunnel_finish(t, 0);
        return;
    }

    ngx_prom_tunnel_relay(t);
}


static void
ngx_prom_tunnel_client_write(ngx_http_request_t *r)
{
    ngx_prom_tunnel_t  *t;

    t = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);
    ngx_prom_tunnel_relay(t);
}


static void
ngx_prom_tunnel_peer_handler(ngx_event_t *ev)
{
    ngx_connection_t   *pc;
    ngx_prom_tunnel_t  *t;

    pc = ev->data;
    t = pc->data;

    if (ev->timedout) {
        ngx_prom_tunnel_finish(t, 0);
        return;
    }

    ngx_prom_tunnel_relay(t);
}


static void
ngx_prom_tunnel_finish(ngx_prom_tunnel_t *t, ngx_uint_t status)
{
    ngx_http_request_t  *r;

    if (t->done) {
        return;
    }

    t->done = 1;
    r = t->request;

    if (status) {
        r->headers_out.status = status;
    }

    if (t->peer.connection) {
        ngx_close_connection(t->peer.connection);
        t->peer.connection = NULL;
    }

    ngx_http_finalize_request(r, NGX_HTTP_CLOSE);
}


/* ------------------------------------------------------------- the connect */

static ngx_int_t
ngx_prom_connect_check(ngx_connection_t *c)
{
    int        err;
    socklen_t  len;

    err = 0;
    len = sizeof(int);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) == -1) {
        err = ngx_socket_errno;
    }

    if (err) {
        ngx_log_error(NGX_LOG_ERR, c->log, err, "prometheus intercept: connect failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_prom_tunnel_connected(ngx_event_t *ev)
{
    ngx_connection_t   *pc;
    ngx_prom_tunnel_t  *t;

    pc = ev->data;
    t = pc->data;

    if (ev->timedout) {
        ngx_prom_tunnel_finish(t, NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    if (ngx_prom_connect_check(pc) != NGX_OK) {
        ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        return;
    }

    if (pc->write->timer_set) {
        ngx_del_timer(pc->write);
    }

    pc->read->handler = ngx_prom_tunnel_peer_handler;
    pc->write->handler = ngx_prom_tunnel_peer_handler;
    pc->write->ready = 1;
    t->connected = 1;

    ngx_prom_tunnel_relay(t);
}


static void
ngx_prom_tunnel_connect(ngx_prom_tunnel_t *t, struct sockaddr *sa,
    socklen_t socklen)
{
    ngx_int_t                       rc;
    ngx_connection_t               *pc;
    ngx_prom_intercept_loc_conf_t  *ilcf;

    ngx_memcpy(&t->sa, sa, socklen);
    t->socklen = socklen;
    t->peer.sockaddr = &t->sa.sockaddr;
    t->peer.socklen = socklen;
    t->peer.name = &t->host;
    t->peer.get = ngx_event_get_peer;
    t->peer.log = t->request->connection->log;
    t->peer.log_error = NGX_ERROR_ERR;

    rc = ngx_event_connect_peer(&t->peer);

    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        return;
    }

    pc = t->peer.connection;
    pc->data = t;
    pc->pool = t->request->pool;
    pc->read->handler = ngx_prom_tunnel_connected;
    pc->write->handler = ngx_prom_tunnel_connected;

    if (rc == NGX_AGAIN) {
        ilcf = ngx_http_get_module_loc_conf(t->request,
                                            ngx_http_prometheus_intercept_module);
        ngx_add_timer(pc->write, ilcf->connect_timeout);
        return;
    }

    ngx_prom_tunnel_connected(pc->write);
}


/* the first IPv4 answer, else the first IPv6 one */
static ngx_resolver_addr_t *
ngx_prom_pick_addr(ngx_resolver_ctx_t *ctx)
{
    ngx_uint_t  i;

    if (ctx->state || ctx->naddrs == 0) {
        return NULL;
    }

    for (i = 0; i < ctx->naddrs; i++) {
        if (ctx->addrs[i].sockaddr->sa_family == AF_INET) {
            return &ctx->addrs[i];
        }
    }

#if (NGX_HAVE_INET6)
    for (i = 0; i < ctx->naddrs; i++) {
        if (ctx->addrs[i].sockaddr->sa_family == AF_INET6) {
            return &ctx->addrs[i];
        }
    }
#endif

    return NULL;
}


/*
 * An IP literal (IPv4, or IPv6 without brackets) as a sockaddr with port.
 * Returns the length, or 0 when s is a name.
 */
static socklen_t
ngx_prom_literal(u_char *s, size_t len, in_port_t port, ngx_sockaddr_t *sa)
{
    in_addr_t  addr;

    ngx_memzero(sa, sizeof(ngx_sockaddr_t));

    addr = ngx_inet_addr(s, len);

    if (addr != INADDR_NONE) {
        sa->sockaddr_in.sin_family = AF_INET;
        sa->sockaddr_in.sin_addr.s_addr = addr;
        sa->sockaddr_in.sin_port = htons(port);
        return sizeof(struct sockaddr_in);
    }

#if (NGX_HAVE_INET6)
    if (ngx_inet6_addr(s, len, sa->sockaddr_in6.sin6_addr.s6_addr) == NGX_OK) {
        sa->sockaddr_in6.sin6_family = AF_INET6;
        sa->sockaddr_in6.sin6_port = htons(port);
        return sizeof(struct sockaddr_in6);
    }
#endif

    return 0;
}


static void
ngx_prom_tunnel_resolved(ngx_resolver_ctx_t *ctx)
{
    socklen_t             len;
    ngx_sockaddr_t        sa;
    ngx_prom_tunnel_t    *t;
    ngx_resolver_addr_t  *a;

    t = ctx->data;
    a = ngx_prom_pick_addr(ctx);

    if (a == NULL) {
        ngx_resolve_name_done(ctx);
        ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        return;
    }

    len = a->socklen;
    ngx_memcpy(&sa, a->sockaddr, len);
    ngx_inet_set_port(&sa.sockaddr, t->port);
    ngx_resolve_name_done(ctx);

    ngx_prom_tunnel_connect(t, &sa.sockaddr, len);
}


static void
ngx_prom_tunnel_direct(ngx_prom_tunnel_t *t)
{
    socklen_t                  len;
    ngx_sockaddr_t             sa;
    ngx_resolver_ctx_t        *ctx, temp;
    ngx_http_core_loc_conf_t  *clcf;

    len = ngx_prom_literal(t->host.data, t->host.len, t->port, &sa);

    if (len) {
        ngx_prom_tunnel_connect(t, &sa.sockaddr, len);
        return;
    }

    if (t->inet6) {
        ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        return;
    }

    clcf = ngx_http_get_module_loc_conf(t->request, ngx_http_core_module);

    temp.name = t->host;
    ctx = ngx_resolve_start(clcf->resolver, &temp);

    if (ctx == NULL || ctx == NGX_NO_RESOLVER) {
        ngx_log_error(NGX_LOG_ERR, t->request->connection->log, 0,
                      "prometheus intercept: no resolver for \"%V\"",
                      &t->host);
        ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        return;
    }

    ctx->name = t->host;
    ctx->handler = ngx_prom_tunnel_resolved;
    ctx->data = t;
    ctx->timeout = clcf->resolver_timeout;

    if (ngx_resolve_name(ctx) != NGX_OK) {
        ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
    }
}


/* the PROXY line in front of what the client sent, for a terminator */
static ngx_int_t
ngx_prom_tunnel_local(ngx_prom_tunnel_t *t, ngx_addr_t *to)
{
    u_char      line[NGX_SOCKADDR_STRLEN + 64], *p;
    size_t      n, have;
    ngx_buf_t  *b;

    if (t->inet6) {
        /* the CONNECT's own address is the destination */
        p = ngx_snprintf(line, sizeof(line), "PROXY TCP6 ::1 %V 1 %d\r\n",
                         &t->host, (int) t->port);

    } else if (ngx_inet_addr(t->host.data, t->host.len) != INADDR_NONE) {
        p = ngx_snprintf(line, sizeof(line), "PROXY TCP4 127.0.0.1 %V 1 %d\r\n",
                         &t->host, (int) t->port);
    } else {
        p = ngx_snprintf(line, sizeof(line),
                         "PROXY TCP4 127.0.0.1 127.0.0.1 1 %d\r\n",
                         (int) t->port);
    }

    n = p - line;
    b = t->up;
    have = b->last - b->pos;

    if (have + n > (size_t) (b->end - b->start)) {
        return NGX_ERROR;
    }

    ngx_memmove(b->start + n, b->pos, have);
    ngx_memcpy(b->start, line, n);
    b->pos = b->start;
    b->last = b->start + n + have;

    t->intercepted = 1;
    ngx_prom_tunnel_connect(t, to->sockaddr, to->socklen);

    return NGX_OK;
}


static void
ngx_prom_tunnel_first_bytes(ngx_http_request_t *r)
{
    ssize_t             n;
    ngx_buf_t                      *b;
    ngx_connection_t               *c;
    ngx_prom_tunnel_t              *t;
    ngx_prom_intercept_loc_conf_t  *ilcf;

    static const u_char  h2[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    c = r->connection;
    t = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);
    ilcf = ngx_http_get_module_loc_conf(r, ngx_http_prometheus_intercept_module);
    b = t->up;

    if (c->read->timedout) {
        ngx_prom_tunnel_finish(t, 0);
        return;
    }

    if (b->last - b->pos < 3) {
        n = c->recv(c, b->last, b->end - b->last);

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_prom_tunnel_finish(t, 0);
            }
            return;
        }

        if (n == 0 || n == NGX_ERROR) {
            ngx_prom_tunnel_finish(t, 0);
            return;
        }

        b->last += n;
    }

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    r->read_event_handler = ngx_prom_tunnel_client_read;
    r->write_event_handler = ngx_prom_tunnel_client_write;

    n = b->last - b->pos;

    if (n >= 2 && b->pos[0] == 0x16 && b->pos[1] == 0x03) {
        if (ngx_prom_tunnel_local(t, ilcf->tls) != NGX_OK) {
            ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        }
        return;
    }

    if (ngx_strncmp(b->pos, h2, ngx_min((size_t) n, sizeof(h2) - 1)) == 0) {
        if (ngx_prom_tunnel_local(t, ilcf->h2c) != NGX_OK) {
            ngx_prom_tunnel_finish(t, NGX_HTTP_BAD_GATEWAY);
        }
        return;
    }

    ngx_prom_tunnel_direct(t);
}


static ngx_int_t
ngx_prom_intercept_handler(ngx_http_request_t *r)
{
    u_char                         *p, *end, *target, *colon;
    ssize_t                         n;
    ngx_int_t                       port;
    ngx_connection_t               *c;
    ngx_prom_tunnel_t              *t;
    ngx_prom_intercept_loc_conf_t  *ilcf;

    static const u_char  established[] =
        "HTTP/1.1 200 Connection Established\r\n\r\n";

    if (r != r->main || r->method != NGX_HTTP_CONNECT) {
        return NGX_DECLINED;
    }

    /* CONNECT host:port HTTP/1.1 */
    p = r->request_line.data;
    end = p + r->request_line.len;

    while (p < end && *p != ' ') { p++; }
    target = ++p;
    while (p < end && *p != ' ') { p++; }

    for (colon = p; colon > target && colon[-1] != ':'; colon--) { }

    if (colon <= target + 1 || colon == p) {
        return NGX_HTTP_BAD_REQUEST;
    }

    port = ngx_atoi(colon, p - colon);

    if (port <= 0 || port > 65535) {
        return NGX_HTTP_BAD_REQUEST;
    }

    t = ngx_pcalloc(r->pool, sizeof(ngx_prom_tunnel_t));
    if (t == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    t->request = r;
    t->port = (in_port_t) port;
    t->host.data = target;
    t->host.len = colon - 1 - target;

    /* [v6]:port: the address without its brackets */
    if (t->host.len > 2 && target[0] == '[') {
        if (target[t->host.len - 1] != ']') {
            return NGX_HTTP_BAD_REQUEST;
        }

        t->host.data++;
        t->host.len -= 2;
        t->inet6 = 1;

#if (NGX_HAVE_INET6)
        {
        u_char  a6[16];

        if (ngx_inet6_addr(t->host.data, t->host.len, a6) != NGX_OK) {
            return NGX_HTTP_BAD_REQUEST;
        }
        }
#else
        return NGX_HTTP_BAD_REQUEST;
#endif
    }

    t->up = ngx_create_temp_buf(r->pool, NGX_PROM_TUNNEL_BUF);
    t->down = ngx_create_temp_buf(r->pool, NGX_PROM_TUNNEL_BUF);
    if (t->up == NULL || t->down == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, t, ngx_http_prometheus_intercept_module);

    c = r->connection;

    /* bytes the client sent after the request, before the answer */
    if (r->header_in->pos < r->header_in->last) {
        n = ngx_min(r->header_in->last - r->header_in->pos,
                    t->up->end - t->up->last);
        t->up->last = ngx_cpymem(t->up->last, r->header_in->pos, n);
        r->header_in->pos += n;
    }

    n = c->send(c, (u_char *) established, sizeof(established) - 1);

    if (n != (ssize_t) sizeof(established) - 1) {
        return NGX_HTTP_CLOSE;
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->header_sent = 1;
    r->keepalive = 0;
    r->main->count++;

    ilcf = ngx_http_get_module_loc_conf(r, ngx_http_prometheus_intercept_module);

    r->read_event_handler = ngx_prom_tunnel_first_bytes;
    r->write_event_handler = ngx_http_request_empty_handler;

    if (t->up->last - t->up->pos >= 3 || c->read->ready) {
        ngx_prom_tunnel_first_bytes(r);
        return NGX_DONE;
    }

    ngx_add_timer(c->read, ilcf->timeout);

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_prom_tunnel_finish(t, 0);
    }

    return NGX_DONE;
}



/* ------------------------------------------------- the destination's pool */

/*
 * On a terminator: prometheus_intercept_resolve on; resolves the request's
 * destination ($host, port from the PROXY line) before the content phase, and
 *
 *   upstream prometheus_intercept_https { prometheus_intercept_origin 256; }
 *
 * connects there, keeping idle connections per destination name and port, not
 * per address: a TLS connection verified for one name is never handed to
 * another name on the same IP.
 */

typedef struct {
    ngx_queue_t                queue;
    ngx_connection_t          *connection;
    ngx_queue_t               *free;
    u_char                     key[NGX_MAXHOSTNAMELEN + 8];
    size_t                     key_len;
} ngx_prom_origin_item_t;


typedef struct {
    ngx_uint_t                 max;
    ngx_msec_t                 timeout;
    ngx_queue_t                cache;
    ngx_queue_t                free;
    ngx_flag_t                 ready;
} ngx_prom_origin_conf_t;


typedef struct {
    ngx_uint_t                 resolved;
    ngx_uint_t                 failed;
    ngx_resolver_ctx_t        *pending;
    in_port_t                  port;
    ngx_sockaddr_t             sa;
    socklen_t                  socklen;
    ngx_str_t                  name;
    u_char                     key[NGX_MAXHOSTNAMELEN + 8];
    size_t                     key_len;
} ngx_prom_dest_t;


typedef struct {
    ngx_prom_origin_conf_t    *conf;
    ngx_prom_dest_t           *dest;
    ngx_http_upstream_t       *upstream;
} ngx_prom_origin_peer_t;


static void
ngx_prom_origin_close(ngx_connection_t *c)
{
#if (NGX_HTTP_SSL)
    if (c->ssl) {
        c->ssl->no_wait_shutdown = 1;
        c->ssl->no_send_shutdown = 1;

        if (ngx_ssl_shutdown(c) == NGX_AGAIN) {
            c->ssl->handler = ngx_prom_origin_close;
            return;
        }
    }
#endif

    ngx_destroy_pool(c->pool);
    ngx_close_connection(c);
}


static void
ngx_prom_origin_dummy_handler(ngx_event_t *ev)
{
}


static void
ngx_prom_origin_close_handler(ngx_event_t *ev)
{
    int                      n;
    char                     buf[1];
    ngx_connection_t        *c;
    ngx_prom_origin_item_t  *item;

    c = ev->data;

    if (!c->close && !c->read->timedout) {
        n = recv(c->fd, buf, 1, MSG_PEEK);

        if (n == -1 && ngx_socket_errno == NGX_EAGAIN) {
            ev->ready = 0;

            if (ngx_handle_read_event(c->read, 0) == NGX_OK) {
                return;
            }
        }
    }

    item = c->data;
    ngx_prom_origin_close(c);
    ngx_queue_remove(&item->queue);
    ngx_queue_insert_head(item->free, &item->queue);
}


static ngx_int_t
ngx_prom_origin_get(ngx_peer_connection_t *pc, void *data)
{
    ngx_prom_origin_peer_t  *op = data;
    ngx_queue_t             *q;
    ngx_connection_t        *c;
    ngx_prom_dest_t         *d;
    ngx_prom_origin_item_t  *item;

    d = op->dest;

    pc->sockaddr = &d->sa.sockaddr;
    pc->socklen = d->socklen;
    pc->name = &d->name;
    pc->cached = 0;
    pc->connection = NULL;

    for (q = ngx_queue_head(&op->conf->cache);
         q != ngx_queue_sentinel(&op->conf->cache);
         q = ngx_queue_next(q))
    {
        item = ngx_queue_data(q, ngx_prom_origin_item_t, queue);

        if (item->key_len == d->key_len
            && ngx_memcmp(item->key, d->key, d->key_len) == 0)
        {
            ngx_queue_remove(q);
            ngx_queue_insert_head(&op->conf->free, q);

            c = item->connection;
            c->idle = 0;
            c->sent = 0;
            c->data = NULL;
            c->log = pc->log;
            c->read->log = pc->log;
            c->write->log = pc->log;
            c->pool->log = pc->log;

            if (c->read->timer_set) {
                ngx_del_timer(c->read);
            }

            pc->connection = c;
            pc->cached = 1;

            return NGX_DONE;
        }
    }

    return NGX_OK;
}


static void
ngx_prom_origin_free(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    ngx_prom_origin_peer_t  *op = data;
    ngx_queue_t             *q;
    ngx_connection_t        *c;
    ngx_http_upstream_t     *u;
    ngx_prom_origin_item_t  *item;

    u = op->upstream;
    c = pc->connection;

    /* one destination: no next one to try */
    pc->tries = 0;

    if (state & NGX_PEER_FAILED
        || c == NULL
        || c->read->eof || c->read->error || c->read->timedout
        || c->write->error || c->write->timedout
        || !u->keepalive || !u->request_body_sent
        || ngx_terminate || ngx_exiting
        || ngx_handle_read_event(c->read, 0) != NGX_OK)
    {
        return;                 /* the upstream closes it */
    }

    if (ngx_queue_empty(&op->conf->free)) {
        q = ngx_queue_last(&op->conf->cache);
        ngx_queue_remove(q);
        item = ngx_queue_data(q, ngx_prom_origin_item_t, queue);
        ngx_prom_origin_close(item->connection);

    } else {
        q = ngx_queue_head(&op->conf->free);
        ngx_queue_remove(q);
        item = ngx_queue_data(q, ngx_prom_origin_item_t, queue);
    }

    ngx_queue_insert_head(&op->conf->cache, q);

    item->connection = c;
    item->key_len = op->dest->key_len;
    ngx_memcpy(item->key, op->dest->key, item->key_len);

    pc->connection = NULL;

    c->read->delayed = 0;
    ngx_add_timer(c->read, op->conf->timeout);

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    c->write->handler = ngx_prom_origin_dummy_handler;
    c->read->handler = ngx_prom_origin_close_handler;
    c->data = item;
    c->idle = 1;
    c->log = ngx_cycle->log;
    c->read->log = ngx_cycle->log;
    c->write->log = ngx_cycle->log;
    c->pool->log = ngx_cycle->log;

    if (c->read->ready) {
        ngx_prom_origin_close_handler(c->read);
    }
}


#if (NGX_HTTP_SSL)

/* kept-alive connections are what saves handshakes here, not sessions */
static ngx_int_t
ngx_prom_origin_set_session(ngx_peer_connection_t *pc, void *data)
{
    return NGX_OK;
}


static void
ngx_prom_origin_save_session(ngx_peer_connection_t *pc, void *data)
{
}

#endif


/* ------------------------------------------------------- on-demand trust */

#if (NGX_HTTP_SSL)

/*
 * prometheus_intercept_trusted <bundle>: the CAs destinations are verified
 * against, without holding the bundle in memory.
 *
 * proxy_ssl_trusted_certificate parses every certificate of a bundle at
 * start and keeps them all: ~0.7 MB for the ~120 CAs of a system bundle, to
 * verify the handful of destinations an app actually calls. Here the master
 * reads the bundle once, one certificate at a time, and keeps an index of it
 * only: the subject hash, offset and length of each certificate (24 bytes
 * apiece), and the open file. The workers add an X509_LOOKUP to the trust
 * store of the upstream TLS contexts that use it: when a verification looks
 * for an issuer the store doesn't hold yet, the certificates with that
 * subject are read from the bundle and added to the store, where they stay.
 *
 * nginx's own verification is unchanged (proxy_ssl_verify, the name check,
 * the depth); only where the store finds issuers is. proxy_ssl_verify still
 * needs a proxy_ssl_trusted_certificate: any certificate of the same bundle
 * keeps the trust the same.
 *
 * The file stays open from the master, so workers read it whatever their
 * user, and a bundle replaced on disk is seen at the next reload, whole.
 */

typedef struct {
    unsigned long              hash;
    off_t                      offset;
    size_t                     len;
} ngx_prom_trust_entry_t;


struct ngx_prom_trust_s {
    ngx_fd_t                   fd;
    ngx_str_t                  name;
    ngx_uint_t                 nelts;
    ngx_prom_trust_entry_t    *entries;
};


static X509_LOOKUP_METHOD  *ngx_prom_trust_method;


static X509 *
ngx_prom_trust_read(ngx_prom_trust_t *t, ngx_prom_trust_entry_t *e)
{
    u_char   *buf;
    BIO      *bio;
    X509     *x;
    ssize_t   n;

    buf = ngx_alloc(e->len, ngx_cycle->log);
    if (buf == NULL) {
        return NULL;
    }

    n = pread(t->fd, buf, e->len, e->offset);

    if (n != (ssize_t) e->len) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, n == -1 ? ngx_errno : 0,
                      "prometheus_intercept_trusted: cannot read \"%V\" "
                      "at %O", &t->name, e->offset);
        ngx_free(buf);
        return NULL;
    }

    x = NULL;

    bio = BIO_new_mem_buf(buf, (int) n);
    if (bio) {
        x = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
        BIO_free(bio);
    }

    ngx_free(buf);

    if (x == NULL) {
        ngx_ssl_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "prometheus_intercept_trusted: cannot parse \"%V\" "
                      "at %O", &t->name, e->offset);
    }

    return x;
}


#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
#define ngx_prom_const_name  const X509_NAME
#else
#define ngx_prom_const_name  X509_NAME
#endif

static int
ngx_prom_trust_by_subject(X509_LOOKUP *lu, X509_LOOKUP_TYPE type,
    ngx_prom_const_name *name, X509_OBJECT *ret)
{
    X509               *x;
    ngx_uint_t          i, found;
    unsigned long       hash;
    ngx_prom_trust_t   *t;

    t = X509_LOOKUP_get_method_data(lu);

    if (t == NULL || type != X509_LU_X509) {
        return 0;
    }

    hash = X509_NAME_hash((X509_NAME *) name);
    found = 0;

    /* every certificate with this subject: a CA can be in a bundle twice */

    for (i = 0; i < t->nelts; i++) {

        if (t->entries[i].hash != hash) {
            continue;
        }

        x = ngx_prom_trust_read(t, &t->entries[i]);
        if (x == NULL) {
            continue;
        }

        if (X509_NAME_cmp(X509_get_subject_name(x), name) == 0
            && X509_STORE_add_cert(X509_LOOKUP_get_store(lu), x) == 1)
        {
            if (!found && X509_OBJECT_set1_X509(ret, x) == 1) {
                found = 1;
            }
        }

        X509_free(x);
    }

    ERR_clear_error();

    return found;
}


static void
ngx_prom_trust_attach(ngx_prom_trust_t *t, SSL_CTX *ctx, ngx_log_t *log)
{
    X509_LOOKUP  *lu;

    if (ngx_prom_trust_method == NULL) {
        ngx_prom_trust_method =
                      X509_LOOKUP_meth_new("prometheus_intercept_trusted");

        if (ngx_prom_trust_method == NULL
            || X509_LOOKUP_meth_set_get_by_subject(ngx_prom_trust_method,
                                              ngx_prom_trust_by_subject) != 1)
        {
            ngx_ssl_error(NGX_LOG_ALERT, log, 0,
                          "prometheus_intercept_trusted: no lookup method");
            return;
        }
    }

    /* the store's lookup of this method: added once, then found again */
    lu = X509_STORE_add_lookup(SSL_CTX_get_cert_store(ctx),
                               ngx_prom_trust_method);

    if (lu == NULL) {
        ngx_ssl_error(NGX_LOG_ALERT, log, 0,
                      "prometheus_intercept_trusted: X509_STORE_add_lookup() "
                      "failed");
        return;
    }

    if (X509_LOOKUP_get_method_data(lu) == NULL) {
        X509_LOOKUP_set_method_data(lu, (char *) t);
    }
}

#endif


static ngx_int_t
ngx_prom_origin_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t               i;
    ngx_prom_dest_t         *d;
    ngx_prom_origin_conf_t  *conf;
    ngx_prom_origin_peer_t  *op;
    ngx_prom_origin_item_t  *items;

    conf = us->peer.data;

    d = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);

    if (d == NULL || !d->resolved) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "prometheus_intercept_origin: no destination; is "
                      "\"prometheus_intercept_resolve on\" set here?");
        return NGX_ERROR;
    }

    if (!conf->ready) {
        /* the pool of this worker, on first use */
        items = ngx_pcalloc(ngx_cycle->pool,
                            sizeof(ngx_prom_origin_item_t) * conf->max);
        if (items == NULL) {
            return NGX_ERROR;
        }

        ngx_queue_init(&conf->cache);
        ngx_queue_init(&conf->free);

        for (i = 0; i < conf->max; i++) {
            items[i].free = &conf->free;
            ngx_queue_insert_head(&conf->free, &items[i].queue);
        }

        conf->ready = 1;
    }

#if (NGX_HTTP_SSL)
    {
    ngx_prom_intercept_main_conf_t  *imcf;

    imcf = ngx_http_get_module_main_conf(r,
                                         ngx_http_prometheus_intercept_module);

    if (imcf->trust && r->upstream->conf->ssl
        && r->upstream->conf->ssl->ctx)
    {
        ngx_prom_trust_attach(imcf->trust, r->upstream->conf->ssl->ctx,
                              r->connection->log);
    }
    }
#endif

    op = ngx_palloc(r->pool, sizeof(ngx_prom_origin_peer_t));
    if (op == NULL) {
        return NGX_ERROR;
    }

    op->conf = conf;
    op->dest = d;
    op->upstream = r->upstream;

    r->upstream->peer.data = op;
    r->upstream->peer.get = ngx_prom_origin_get;
    r->upstream->peer.free = ngx_prom_origin_free;
    r->upstream->peer.tries = 1;
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session = ngx_prom_origin_set_session;
    r->upstream->peer.save_session = ngx_prom_origin_save_session;
#endif

    return NGX_OK;
}


static ngx_int_t
ngx_prom_origin_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    us->peer.init = ngx_prom_origin_init_peer;
    return NGX_OK;
}


static char *
ngx_prom_intercept_origin(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_int_t                      n;
    ngx_str_t                     *value;
    ngx_prom_origin_conf_t        *oc;
    ngx_http_upstream_srv_conf_t  *uscf;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    if (uscf->peer.init_upstream) {
        return "is duplicate, or after another balancer";
    }

    value = cf->args->elts;
    n = ngx_atoi(value[1].data, value[1].len);

    if (n == NGX_ERROR || n == 0) {
        return "takes the number of idle connections kept per worker";
    }

    oc = ngx_pcalloc(cf->pool, sizeof(ngx_prom_origin_conf_t));
    if (oc == NULL) {
        return NGX_CONF_ERROR;
    }

    oc->max = n;
    oc->timeout = 60000;

    uscf->peer.data = oc;
    uscf->peer.init_upstream = ngx_prom_origin_init;
    uscf->flags = NGX_HTTP_UPSTREAM_CREATE;

    return NGX_CONF_OK;
}


static void
ngx_prom_dest_resolved(ngx_resolver_ctx_t *ctx)
{
    ngx_prom_dest_t      *d;
    ngx_http_request_t   *r;
    ngx_resolver_addr_t  *a;

    r = ctx->data;
    d = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);
    a = ngx_prom_pick_addr(ctx);

    if (a) {
        ngx_memcpy(&d->sa, a->sockaddr, a->socklen);
        d->socklen = a->socklen;
        ngx_inet_set_port(&d->sa.sockaddr, d->port);
        d->resolved = 1;

    } else {
        d->failed = 1;
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "prometheus intercept: could not resolve \"%V\"",
                      &d->name);
    }

    d->pending = NULL;
    ngx_resolve_name_done(ctx);

    r->main->count--;
    ngx_http_core_run_phases(r);
}


static void
ngx_prom_dest_cleanup(void *data)
{
    ngx_prom_dest_t  *d = data;

    if (d->pending) {
        ngx_resolve_name_done(d->pending);
        d->pending = NULL;
    }
}


static ngx_int_t
ngx_prom_dest_handler(ngx_http_request_t *r)
{
    ngx_pool_cleanup_t             *cln;
    socklen_t                       len;
    ngx_int_t                       port;
    ngx_str_t                      *host, name;
    ngx_prom_dest_t                *d;
    ngx_resolver_ctx_t             *rctx, temp;
    ngx_http_core_loc_conf_t       *clcf;
    ngx_prom_intercept_loc_conf_t  *ilcf;

    ilcf = ngx_http_get_module_loc_conf(r, ngx_http_prometheus_intercept_module);

    if (!ilcf->resolve || r != r->main) {
        return NGX_DECLINED;
    }

    d = ngx_http_get_module_ctx(r, ngx_http_prometheus_intercept_module);

    if (d) {
        return d->failed ? NGX_HTTP_BAD_GATEWAY
                         : (d->resolved ? NGX_DECLINED : NGX_AGAIN);
    }

    if (r->connection->proxy_protocol == NULL) {
        return NGX_HTTP_BAD_GATEWAY;
    }

    port = r->connection->proxy_protocol->dst_port;
    host = &r->headers_in.server;

    if (host->len == 0 || host->len > NGX_MAXHOSTNAMELEN || port == 0) {
        return NGX_HTTP_BAD_GATEWAY;
    }

    d = ngx_pcalloc(r->pool, sizeof(ngx_prom_dest_t));
    if (d == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, d, ngx_http_prometheus_intercept_module);

    d->name = *host;
    d->key_len = ngx_sprintf(d->key, "%V:%d", host, (int) port) - d->key;
    d->port = (in_port_t) port;

    /* an IP literal, IPv6 in brackets as Host has it */
    name = *host;

    if (name.len > 2 && name.data[0] == '[' && name.data[name.len - 1] == ']') {
        name.data++;
        name.len -= 2;
    }

    len = ngx_prom_literal(name.data, name.len, d->port, &d->sa);

    if (len) {
        d->socklen = len;
        d->resolved = 1;
        return NGX_DECLINED;
    }

    if (name.len != host->len) {
        return NGX_HTTP_BAD_GATEWAY;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    temp.name = *host;
    rctx = ngx_resolve_start(clcf->resolver, &temp);

    if (rctx == NULL || rctx == NGX_NO_RESOLVER) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "prometheus intercept: no resolver for \"%V\"", host);
        return NGX_HTTP_BAD_GATEWAY;
    }

    rctx->name = *host;
    rctx->handler = ngx_prom_dest_resolved;
    rctx->data = r;
    rctx->timeout = clcf->resolver_timeout;

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        ngx_resolve_name_done(rctx);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cln->handler = ngx_prom_dest_cleanup;
    cln->data = d;
    d->pending = rctx;

    r->main->count++;

    if (ngx_resolve_name(rctx) != NGX_OK) {
        /* ngx_resolve_name() has freed the context */
        d->pending = NULL;
        r->main->count--;
        return NGX_HTTP_BAD_GATEWAY;
    }

    /* answered from the cache or not, the handler runs the phases on */
    return NGX_DONE;
}


static ngx_int_t
ngx_prom_intercept_postconf(ngx_conf_t *cf)
{
    ngx_uint_t                      i;
    ngx_http_handler_pt            *h;
    ngx_http_core_main_conf_t      *cmcf;
    ngx_http_upstream_main_conf_t  *umcf;
    ngx_http_upstream_srv_conf_t  **uscfp;

    /*
     * nginx 1.29+ gives every upstream{} an implicit keepalive pool keyed by
     * address only, wrapped around the balancer when the main conf is set up.
     * For ours that would hand a connection verified for one name to another
     * name on the same IP: take the upstream back, its own pool is per name.
     */
    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (uscfp[i]->peer.init_upstream == ngx_prom_origin_init) {
            uscfp[i]->peer.init = ngx_prom_origin_init_peer;
        }
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_prom_dest_handler;

    return NGX_OK;
}


/* ------------------------------------------------------- leaf certificates */

#if (NGX_HTTP_SSL)

typedef struct {
    ngx_str_t     name;
    ngx_str_t     pem;
    time_t        expires;
} ngx_prom_mint_t;


static X509             *ngx_prom_ca_cert;
static EVP_PKEY         *ngx_prom_ca_key;
static EVP_PKEY         *ngx_prom_leaf_key;
static ngx_str_t         ngx_prom_leaf_key_pem;
static ngx_prom_mint_t   ngx_prom_mints[NGX_PROM_MINT_CACHE];


static ngx_str_t
ngx_prom_pem(BIO *bio)
{
    char       *data;
    long        len;
    ngx_str_t   s = ngx_null_string;

    len = BIO_get_mem_data(bio, &data);

    s.data = ngx_alloc(len, ngx_cycle->log);
    if (s.data != NULL) {
        ngx_memcpy(s.data, data, len);
        s.len = len;
    }

    return s;
}


static ngx_str_t
ngx_prom_mint(ngx_str_t *name)
{
    BIO              *bio;
    X509             *x;
    ngx_str_t         pem = ngx_null_string;
    ngx_uint_t        h;
    X509_NAME        *subj;
    X509_EXTENSION   *ext;
    X509V3_CTX        v3;
    ngx_prom_mint_t  *m;
    u_char            san[NGX_MAXHOSTNAMELEN + 8], cn[NGX_MAXHOSTNAMELEN + 1];
    u_char           *p;

    if (name->len == 0 || name->len > NGX_MAXHOSTNAMELEN) {
        return pem;
    }

    h = ngx_crc32_short(name->data, name->len) % NGX_PROM_MINT_CACHE;
    m = &ngx_prom_mints[h];

    if (m->pem.len && m->expires > ngx_time()
        && m->name.len == name->len
        && ngx_strncmp(m->name.data, name->data, name->len) == 0)
    {
        return m->pem;
    }

    x = X509_new();
    if (x == NULL) {
        return pem;
    }

    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x),
                     (long) (ngx_random() & 0x7fffffff));
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 7 * 86400);
    X509_set_pubkey(x, ngx_prom_leaf_key);

    p = ngx_cpymem(cn, name->data, name->len);
    *p = '\0';

    subj = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC, cn, -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(ngx_prom_ca_cert));

    {
    ngx_sockaddr_t  sa;

    p = ngx_prom_literal(name->data, name->len, 0, &sa)
        ? ngx_sprintf(san, "IP:%V", name) : ngx_sprintf(san, "DNS:%V", name);
    *p = '\0';
    }

    X509V3_set_ctx(&v3, ngx_prom_ca_cert, x, NULL, NULL, 0);

    ext = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, (char *) san);
    if (ext) {
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }

    ext = X509V3_EXT_conf_nid(NULL, &v3, NID_ext_key_usage,
                              (char *) "serverAuth");
    if (ext) {
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }

    if (X509_sign(x, ngx_prom_ca_key, EVP_sha256()) == 0) {
        X509_free(x);
        return pem;
    }

    bio = BIO_new(BIO_s_mem());
    if (bio == NULL) {
        X509_free(x);
        return pem;
    }

    /* the leaf, then the CA, as a chain */
    PEM_write_bio_X509(bio, x);
    PEM_write_bio_X509(bio, ngx_prom_ca_cert);
    pem = ngx_prom_pem(bio);
    BIO_free(bio);
    X509_free(x);

    if (pem.len == 0) {
        return pem;
    }

    if (m->pem.data) {
        ngx_free(m->pem.data);
    }
    if (m->name.data) {
        ngx_free(m->name.data);
    }

    m->name.data = ngx_alloc(name->len, ngx_cycle->log);
    if (m->name.data == NULL) {
        m->pem.data = NULL;
        m->pem.len = 0;
        return pem;          /* leaks one cert until exit; rare */
    }

    ngx_memcpy(m->name.data, name->data, name->len);
    m->name.len = name->len;
    m->pem = pem;
    m->expires = ngx_time() + 6 * 86400;

    return m->pem;
}


static ngx_int_t
ngx_prom_leaf_name(ngx_http_request_t *r, ngx_str_t *name)
{
    const char  *sni;

    sni = r->connection->ssl
          ? SSL_get_servername(r->connection->ssl->connection,
                               TLSEXT_NAMETYPE_host_name)
          : NULL;

    if (sni && *sni) {
        name->data = (u_char *) sni;
        name->len = ngx_strlen(sni);
        return NGX_OK;
    }

    /* no SNI: the CONNECT was to an address, carried as the PROXY dst */
    if (r->connection->proxy_protocol
        && r->connection->proxy_protocol->dst_addr.len)
    {
        *name = r->connection->proxy_protocol->dst_addr;
        return NGX_OK;
    }

    return NGX_DECLINED;
}

#endif


static ngx_int_t
ngx_prom_intercept_cert_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
#if (NGX_HTTP_SSL)
    ngx_str_t  name, pem;

    if (ngx_prom_ca_key == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    if (data == 1) {
        pem = ngx_prom_leaf_key_pem;

    } else {
        if (ngx_prom_leaf_name(r, &name) != NGX_OK) {
            v->not_found = 1;
            return NGX_OK;
        }

        pem = ngx_prom_mint(&name);
    }

    if (pem.len == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    /* PEM, for ssl_certificate data:$... */
    v->data = pem.data;
    v->len = pem.len;
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    return NGX_OK;
#else
    v->not_found = 1;
    return NGX_OK;
#endif
}


static ngx_int_t
ngx_prom_intercept_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    static ngx_str_t  cert = ngx_string("prometheus_intercept_cert");
    static ngx_str_t  key = ngx_string("prometheus_intercept_key");

    var = ngx_http_add_variable(cf, &cert, NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_prom_intercept_cert_variable;
    var->data = 0;

    var = ngx_http_add_variable(cf, &key, NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_ERROR;
    }
    var->get_handler = ngx_prom_intercept_cert_variable;
    var->data = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_prom_intercept_init_worker(ngx_cycle_t *cycle)
{
#if (NGX_HTTP_SSL)
    BIO                             *bio;
    EVP_PKEY_CTX                    *kctx;
    ngx_prom_intercept_main_conf_t  *imcf;

    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return NGX_OK;
    }

    imcf = ngx_http_cycle_get_module_main_conf(cycle,
                                          ngx_http_prometheus_intercept_module);

    if (imcf == NULL || imcf->ca_cert.len == 0) {
        return NGX_OK;
    }

    if (ngx_prom_ca_cert == NULL || ngx_prom_ca_key == NULL) {
        return NGX_OK;
    }

    /* one EC P-256 key per worker for every leaf */
    kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (kctx == NULL
        || EVP_PKEY_keygen_init(kctx) <= 0
        || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx,
                                                  NID_X9_62_prime256v1) <= 0
        || EVP_PKEY_keygen(kctx, &ngx_prom_leaf_key) <= 0)
    {
        EVP_PKEY_CTX_free(kctx);
        ngx_prom_ca_key = NULL;
        return NGX_OK;
    }

    EVP_PKEY_CTX_free(kctx);

    bio = BIO_new(BIO_s_mem());
    if (bio) {
        PEM_write_bio_PrivateKey(bio, ngx_prom_leaf_key, NULL, NULL, 0,
                                 NULL, NULL);
        ngx_prom_leaf_key_pem = ngx_prom_pem(bio);
        BIO_free(bio);
    }
#endif

    return NGX_OK;
}


static void
ngx_prom_intercept_exit_worker(ngx_cycle_t *cycle)
{
#if (NGX_HTTP_SSL)
    ngx_uint_t  i;

    for (i = 0; i < NGX_PROM_MINT_CACHE; i++) {
        if (ngx_prom_mints[i].pem.data) {
            ngx_free(ngx_prom_mints[i].pem.data);
        }
        if (ngx_prom_mints[i].name.data) {
            ngx_free(ngx_prom_mints[i].name.data);
        }
    }

    if (ngx_prom_leaf_key_pem.data) {
        ngx_free(ngx_prom_leaf_key_pem.data);
    }
#endif
}


/* ------------------------------------------------------------ configuration */

static u_char *
ngx_pstrdup0(ngx_pool_t *pool, ngx_str_t *src)
{
    u_char  *dst;

    dst = ngx_pnalloc(pool, src->len + 1);
    if (dst) {
        *ngx_cpymem(dst, src->data, src->len) = '\0';
    }

    return dst;
}


static void *
ngx_prom_intercept_create_main_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_prom_intercept_main_conf_t));
}


static void *
ngx_prom_intercept_create_loc_conf(ngx_conf_t *cf)
{
    ngx_prom_intercept_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_prom_intercept_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->connect = NGX_CONF_UNSET;
    conf->resolve = NGX_CONF_UNSET;
    conf->timeout = NGX_CONF_UNSET_MSEC;
    conf->connect_timeout = NGX_CONF_UNSET_MSEC;

    return conf;
}


/* a default terminator address */
static ngx_addr_t *
ngx_prom_intercept_addr(ngx_conf_t *cf, const char *ip, in_port_t port)
{
    ngx_addr_t          *a;
    struct sockaddr_in  *sin;

    a = ngx_pcalloc(cf->pool, sizeof(ngx_addr_t));
    sin = ngx_pcalloc(cf->pool, sizeof(struct sockaddr_in));

    if (a == NULL || sin == NULL) {
        return NULL;
    }

    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = ngx_inet_addr((u_char *) ip, ngx_strlen(ip));
    sin->sin_port = htons(port);

    a->sockaddr = (struct sockaddr *) sin;
    a->socklen = sizeof(struct sockaddr_in);
    ngx_str_set(&a->name, "127.0.0.1");

    return a;
}


static char *
ngx_prom_intercept_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_prom_intercept_loc_conf_t  *prev = parent;
    ngx_prom_intercept_loc_conf_t  *conf = child;

    ngx_conf_merge_value(conf->connect, prev->connect, 0);
    ngx_conf_merge_value(conf->resolve, prev->resolve, 0);
    ngx_conf_merge_msec_value(conf->timeout, prev->timeout, 300000);
    ngx_conf_merge_msec_value(conf->connect_timeout, prev->connect_timeout,
                              10000);

    if (conf->tls == NULL) {
        conf->tls = prev->tls ? prev->tls
                              : ngx_prom_intercept_addr(cf, "127.0.0.1", 3129);
    }

    if (conf->h2c == NULL) {
        conf->h2c = prev->h2c ? prev->h2c
                              : ngx_prom_intercept_addr(cf, "127.0.0.1", 3130);
    }

    if (conf->tls == NULL || conf->h2c == NULL) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_prom_intercept_connect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_prom_intercept_loc_conf_t  *ilcf = conf;
    ngx_url_t                       u;
    ngx_str_t                      *value;
    ngx_uint_t                      i;
    ngx_addr_t                    **to;
    ngx_http_core_loc_conf_t       *clcf;
#if (nginx_version >= 1031000)
    ngx_http_core_srv_conf_t       *cscf;
#endif

    if (ilcf->connect != NGX_CONF_UNSET) {
        return "is duplicate";
    }

#if (nginx_version >= 1031000)
    /* the server accepts CONNECT, as with tunnel_pass */
    cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);
    cscf->allow_connect = 1;
#else
    return "needs nginx 1.31 or newer";
#endif

    ilcf->connect = 1;

    /* tls=<addr> h2c=<addr>: where the terminators listen */
    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        if (value[i].len > 4
            && ngx_strncmp(value[i].data, "tls=", 4) == 0)
        {
            to = &ilcf->tls;

        } else if (value[i].len > 4
                   && ngx_strncmp(value[i].data, "h2c=", 4) == 0)
        {
            to = &ilcf->h2c;

        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid parameter \"%V\": "
                               "tls=<address> or h2c=<address>", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (*to) {
            return "has a duplicate parameter";
        }

        ngx_memzero(&u, sizeof(ngx_url_t));
        u.url.data = value[i].data + 4;
        u.url.len = value[i].len - 4;
        u.no_resolve = 1;

        if (ngx_parse_url(cf->pool, &u) != NGX_OK || u.naddrs == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%s in \"%V\"",
                               u.err ? u.err : "invalid address", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (u.no_port && u.family != AF_UNIX) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "no port in \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        *to = &u.addrs[0];
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_prom_intercept_handler;

    return NGX_CONF_OK;
}


static char *
ngx_prom_intercept_ca(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_prom_intercept_main_conf_t  *imcf = conf;
    ngx_str_t                       *value;

    value = cf->args->elts;

#if !(NGX_HTTP_SSL)
    return "needs nginx built with ssl";
#endif

    imcf->ca_cert = value[1];
    imcf->ca_key = value[2];

    if (ngx_conf_full_name(cf->cycle, &imcf->ca_cert, 1) != NGX_OK
        || ngx_conf_full_name(cf->cycle, &imcf->ca_key, 1) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    /* NUL-terminated, for BIO_new_file() */
    imcf->ca_cert.data = (u_char *) ngx_pstrdup0(cf->pool, &imcf->ca_cert);
    imcf->ca_key.data = (u_char *) ngx_pstrdup0(cf->pool, &imcf->ca_key);

    if (imcf->ca_cert.data == NULL || imcf->ca_key.data == NULL) {
        return NGX_CONF_ERROR;
    }

#if (NGX_HTTP_SSL)
    {
    BIO  *bio;

    /* read now, by the master: the key is usually readable by root only */
    if (ngx_prom_ca_cert) {
        X509_free(ngx_prom_ca_cert);
        ngx_prom_ca_cert = NULL;
    }
    if (ngx_prom_ca_key) {
        EVP_PKEY_free(ngx_prom_ca_key);
        ngx_prom_ca_key = NULL;
    }

    bio = BIO_new_file((char *) imcf->ca_cert.data, "r");
    if (bio) {
        ngx_prom_ca_cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        BIO_free(bio);
    }

    bio = BIO_new_file((char *) imcf->ca_key.data, "r");
    if (bio) {
        ngx_prom_ca_key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
        BIO_free(bio);
    }

    if (ngx_prom_ca_cert == NULL || ngx_prom_ca_key == NULL) {
        ngx_ssl_error(NGX_LOG_EMERG, cf->log, 0,
                      "prometheus_intercept_ca: cannot read \"%s\", \"%s\"",
                      imcf->ca_cert.data, imcf->ca_key.data);
        return NGX_CONF_ERROR;
    }

    if (X509_check_private_key(ngx_prom_ca_cert, ngx_prom_ca_key) != 1) {
        return "the certificate and the key do not match";
    }
    }
#endif

    return NGX_CONF_OK;
}


static char *
ngx_prom_intercept_trusted(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
#if !(NGX_HTTP_SSL)
    return "needs nginx built with ssl";
#else
    ngx_prom_intercept_main_conf_t  *imcf = conf;

    BIO                     *bio;
    X509                    *x;
    long                     at;
    ngx_str_t               *value, name;
    ngx_array_t              entries;
    ngx_prom_trust_t        *t;
    ngx_pool_cleanup_t      *cln;
    ngx_pool_cleanup_file_t *clnf;
    ngx_prom_trust_entry_t  *e;

    if (imcf->trust) {
        return "is duplicate";
    }

    value = cf->args->elts;
    name = value[1];

    if (ngx_conf_full_name(cf->cycle, &name, 1) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    name.data = (u_char *) ngx_pstrdup0(cf->pool, &name);
    if (name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ngx_array_init(&entries, cf->temp_pool, 128,
                       sizeof(ngx_prom_trust_entry_t))
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    bio = BIO_new_file((char *) name.data, "r");
    if (bio == NULL) {
        ngx_ssl_error(NGX_LOG_EMERG, cf->log, 0,
                      "prometheus_intercept_trusted: cannot open \"%s\"",
                      name.data);
        return NGX_CONF_ERROR;
    }

    /* one certificate at a time: only its hash and place are kept */

    for ( ;; ) {
        at = BIO_tell(bio);

        x = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
        if (x == NULL) {
            break;
        }

        e = ngx_array_push(&entries);
        if (e == NULL) {
            X509_free(x);
            BIO_free(bio);
            return NGX_CONF_ERROR;
        }

        e->hash = X509_NAME_hash(X509_get_subject_name(x));
        e->offset = at;
        e->len = BIO_tell(bio) - at;

        X509_free(x);
    }

    BIO_free(bio);

    /* the end of the file, or a block that isn't a certificate */

    if (ERR_GET_REASON(ERR_peek_last_error()) != PEM_R_NO_START_LINE) {
        ngx_ssl_error(NGX_LOG_EMERG, cf->log, 0,
                      "prometheus_intercept_trusted: cannot read \"%s\" "
                      "after %ui certificates", name.data, entries.nelts);
        return NGX_CONF_ERROR;
    }

    ERR_clear_error();

    if (entries.nelts == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "prometheus_intercept_trusted: no certificates "
                           "in \"%s\"", name.data);
        return NGX_CONF_ERROR;
    }

    t = ngx_pcalloc(cf->pool, sizeof(ngx_prom_trust_t));
    if (t == NULL) {
        return NGX_CONF_ERROR;
    }

    t->entries = ngx_pnalloc(cf->pool,
                             entries.nelts * sizeof(ngx_prom_trust_entry_t));
    if (t->entries == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(t->entries, entries.elts,
               entries.nelts * sizeof(ngx_prom_trust_entry_t));
    t->nelts = entries.nelts;
    t->name = name;

    /* kept open: the workers read it whatever their user */

    cln = ngx_pool_cleanup_add(cf->pool, sizeof(ngx_pool_cleanup_file_t));
    if (cln == NULL) {
        return NGX_CONF_ERROR;
    }

    t->fd = ngx_open_file(name.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);

    if (t->fd == NGX_INVALID_FILE) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           ngx_open_file_n " \"%s\" failed", name.data);
        return NGX_CONF_ERROR;
    }

    (void) fcntl(t->fd, F_SETFD, FD_CLOEXEC);

    cln->handler = ngx_pool_cleanup_file;
    clnf = cln->data;
    clnf->fd = t->fd;
    clnf->name = name.data;
    clnf->log = cf->log;

    imcf->trust = t;

    return NGX_CONF_OK;
#endif
}
