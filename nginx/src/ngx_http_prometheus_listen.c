/*
 * The built-in metrics listener: with no `prometheus;` location anywhere in
 * the configuration, the module opens prometheus_listen (default 9145) on its
 * own and answers there, whatever nginx.conf and its servers are:
 *
 *   GET /metrics   the exposition
 *   GET /healthz   "ok"
 *
 * It is not an http{} server: no server_name, no location, nothing in it is
 * recorded, and the rest of the configuration can't shadow it. Each scrape
 * is one HTTP/1.1 request on one connection (Connection: close); the request
 * must fit in 4k and arrive within 10s.
 *
 * If a server in the configuration already listens on that port, the module
 * says so in the error log and stays out of the way rather than failing
 * nginx's start: add a `prometheus;` location or set prometheus_listen.
 */

#include "ngx_http_prometheus.h"


#define NGX_PROM_LISTEN_BUF      4096
#define NGX_PROM_LISTEN_TIMEOUT  10000


typedef struct {
    ngx_buf_t    *in;
    ngx_chain_t  *out;
} ngx_prom_listen_ctx_t;


static void ngx_prom_listen_init(ngx_connection_t *c);
static void ngx_prom_listen_read(ngx_event_t *rev);
static void ngx_prom_listen_write(ngx_event_t *wev);


ngx_int_t
ngx_prom_listen_add(ngx_conf_t *cf, ngx_http_prometheus_main_conf_t *pmcf)
{
    in_port_t                   port;
    ngx_url_t                   u;
    ngx_uint_t                  i;
    ngx_listening_t            *ls;
    ngx_http_conf_port_t       *ports;
    ngx_http_core_main_conf_t  *cmcf;

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.url = pmcf->listen;
    u.listen = 1;
    u.default_port = 9145;

    if (ngx_parse_url(cf->pool, &u) != NGX_OK || u.naddrs == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid prometheus_listen \"%V\"%s%s",
                           &pmcf->listen, u.err ? ": " : "",
                           u.err ? u.err : "");
        return NGX_ERROR;
    }

    port = ngx_inet_get_port(u.addrs[0].sockaddr);

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    if (cmcf->ports) {
        ports = cmcf->ports->elts;

        for (i = 0; i < cmcf->ports->nelts; i++) {
            if (ports[i].port == port) {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "prometheus: a server already listens on "
                                   "port %d, so metrics are not served there; "
                                   "add \"location = /metrics { prometheus; }"
                                   "\" to a server, or set "
                                   "\"prometheus_listen\"", (int) port);
                return NGX_OK;
            }
        }
    }

    ls = ngx_create_listening(cf, u.addrs[0].sockaddr, u.addrs[0].socklen);
    if (ls == NULL) {
        return NGX_ERROR;
    }

    ls->addr_ntop = 1;
    ls->handler = ngx_prom_listen_init;
    ls->pool_size = 1024;
    ls->backlog = NGX_LISTEN_BACKLOG;
    ls->rcvbuf = -1;
    ls->sndbuf = -1;
    ls->wildcard = u.wildcard;
    ls->logp = &cf->cycle->new_log;
    ls->log.data = &ls->addr_text;
    ls->log.handler = ngx_accept_log_error;

#if (NGX_HAVE_INET6)
    ls->ipv6only = 1;
#endif

    return NGX_OK;
}


static void
ngx_prom_listen_close(ngx_connection_t *c)
{
    ngx_pool_t  *pool;

    /*
     * The accept counted this connection as active, as for any listener;
     * http and stream take it back when they close theirs, so this one must
     * too, or every scrape would add one to "active" (and to stub_status).
     */
#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif

    pool = c->pool;
    ngx_close_connection(c);
    ngx_destroy_pool(pool);
}


static void
ngx_prom_listen_init(ngx_connection_t *c)
{
    ngx_prom_listen_ctx_t  *ctx;

    ctx = ngx_pcalloc(c->pool, sizeof(ngx_prom_listen_ctx_t));
    if (ctx == NULL) {
        ngx_prom_listen_close(c);
        return;
    }

    ctx->in = ngx_create_temp_buf(c->pool, NGX_PROM_LISTEN_BUF);
    if (ctx->in == NULL) {
        ngx_prom_listen_close(c);
        return;
    }

    c->data = ctx;
    c->log->action = "serving prometheus metrics";
    c->read->handler = ngx_prom_listen_read;
    c->write->handler = ngx_prom_listen_write;

    ngx_add_timer(c->read, NGX_PROM_LISTEN_TIMEOUT);

    if (c->read->ready) {
        ngx_prom_listen_read(c->read);
        return;
    }

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_prom_listen_close(c);
    }
}


static ngx_int_t
ngx_prom_listen_respond(ngx_connection_t *c, ngx_uint_t head,
    ngx_str_t *path)
{
    off_t                             length;
    ngx_buf_t                        *b;
    ngx_str_t                         status, type;
    ngx_chain_t                      *body, *cl;
    ngx_prom_listen_ctx_t            *ctx;
    ngx_http_prometheus_main_conf_t  *pmcf;

    ctx = c->data;
    body = NULL;
    length = 0;

    ngx_str_set(&type, "text/plain; charset=utf-8");

    if (path->len == 8 && ngx_strncmp(path->data, "/metrics", 8) == 0) {
        pmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                                   ngx_http_prometheus_module);

        if (ngx_prom_render(pmcf, c->pool, &body, &length) != NGX_OK) {
            return NGX_ERROR;
        }

        ngx_str_set(&status, "200 OK");
        ngx_str_set(&type, "text/plain; version=0.0.4; charset=utf-8");

    } else {
        b = ngx_calloc_buf(c->pool);
        body = ngx_alloc_chain_link(c->pool);
        if (b == NULL || body == NULL) {
            return NGX_ERROR;
        }

        if (path->len == 8 && ngx_strncmp(path->data, "/healthz", 8) == 0) {
            ngx_str_set(&status, "200 OK");
            b->pos = (u_char *) "ok\n";

        } else {
            ngx_str_set(&status, "404 Not Found");
            b->pos = (u_char *) "metrics are at /metrics\n";
        }

        b->last = b->pos + ngx_strlen(b->pos);
        b->memory = 1;
        body->buf = b;
        body->next = NULL;
        length = b->last - b->pos;
    }

    b = ngx_create_temp_buf(c->pool, 256);
    cl = ngx_alloc_chain_link(c->pool);
    if (b == NULL || cl == NULL) {
        return NGX_ERROR;
    }

    b->last = ngx_sprintf(b->last, "HTTP/1.1 %V\r\nContent-Type: %V\r\n"
                          "Content-Length: %O\r\nConnection: close\r\n\r\n",
                          &status, &type, length);

    cl->buf = b;
    cl->next = head ? NULL : body;

    ctx->out = cl;

    return NGX_OK;
}


static void
ngx_prom_listen_read(ngx_event_t *rev)
{
    u_char                 *p, *end, *sp;
    ssize_t                 n;
    ngx_str_t               method, path;
    ngx_connection_t       *c;
    ngx_prom_listen_ctx_t  *ctx;

    c = rev->data;
    ctx = c->data;

    if (rev->timedout) {
        ngx_prom_listen_close(c);
        return;
    }

    n = c->recv(c, ctx->in->last, ctx->in->end - ctx->in->last);

    if (n == NGX_AGAIN) {
        if (ngx_handle_read_event(rev, 0) != NGX_OK) {
            ngx_prom_listen_close(c);
        }
        return;
    }

    if (n == NGX_ERROR || n == 0) {
        ngx_prom_listen_close(c);
        return;
    }

    ctx->in->last += n;

    /* the request line and headers are in once a blank line is */
    p = ctx->in->pos;
    end = ctx->in->last;

    if (ngx_strlcasestrn(p, end, (u_char *) "\r\n\r\n", 3) == NULL
        && ngx_strlcasestrn(p, end, (u_char *) "\n\n", 1) == NULL)
    {
        if (ctx->in->last == ctx->in->end) {
            ngx_prom_listen_close(c);
        }
        return;
    }

    if (rev->timer_set) {
        ngx_del_timer(rev);
    }

    /* METHOD SP PATH[?query] SP VERSION */

    sp = ngx_strlchr(p, end, ' ');
    if (sp == NULL) {
        ngx_prom_listen_close(c);
        return;
    }

    method.data = p;
    method.len = sp - p;

    path.data = sp + 1;
    for (p = path.data; p < end && *p != ' ' && *p != '?' && *p != '\r'
                        && *p != '\n'; p++)
    { /* void */ }
    path.len = p - path.data;

    if (!((method.len == 3 && ngx_strncmp(method.data, "GET", 3) == 0)
          || (method.len == 4 && ngx_strncmp(method.data, "HEAD", 4) == 0)))
    {
        ngx_str_set(&path, "");
    }

    if (ngx_prom_listen_respond(c, method.len == 4, &path) != NGX_OK) {
        ngx_prom_listen_close(c);
        return;
    }

    ngx_prom_listen_write(c->write);
}


static void
ngx_prom_listen_write(ngx_event_t *wev)
{
    ngx_chain_t            *cl;
    ngx_connection_t       *c;
    ngx_prom_listen_ctx_t  *ctx;

    c = wev->data;
    ctx = c->data;

    if (wev->timedout) {
        ngx_prom_listen_close(c);
        return;
    }

    if (ctx->out == NULL) {
        return;                     /* writable before the request is in */
    }

    cl = c->send_chain(c, ctx->out, 0);

    if (cl == NGX_CHAIN_ERROR) {
        ngx_prom_listen_close(c);
        return;
    }

    ctx->out = cl;

    if (cl) {
        ngx_add_timer(wev, NGX_PROM_LISTEN_TIMEOUT);

        if (ngx_handle_write_event(wev, 0) != NGX_OK) {
            ngx_prom_listen_close(c);
        }
        return;
    }

    ngx_prom_listen_close(c);
}
