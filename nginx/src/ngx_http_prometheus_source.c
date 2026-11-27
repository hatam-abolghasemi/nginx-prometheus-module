/*
 * Who a request came from (source_service / source_namespace) and who this
 * nginx is (app / namespace), the way the OpenResty sidecar works them out:
 *
 *   1. a User-Agent starting with the probe prefix (kube-probe/) is a kubelet
 *      probe: prometheus_source_unresolved, no lookup;
 *   2. an address in a prometheus_source_static range gets that range's
 *      labels (the longest matching range wins);
 *   3. an address in a prometheus_source_ptr range (the pod CIDR) is looked
 *      up by reverse DNS: <pod>.<service>.<namespace>.svc.<cluster domain>;
 *   4. anything else is prometheus_source_default.
 *
 * Then, if the caller turns out to be the mesh waypoint, the real caller is
 * read from the waypoint's X-Forwarded-Client-Cert. On the outbound leg
 * (prometheus_mode 1) the caller is this nginx itself.
 *
 * Reverse DNS never sits in the request path. A request from an address with
 * no answer yet starts the lookup in the precontent phase (after realip has
 * set the real client address) and is labelled with whatever is known when it
 * is logged; the answer is usually there by then. Answers live in the zone,
 * shared by all workers, and are kept until a new answer replaces them:
 * worker 0 re-resolves every known address
 * each prometheus_source_refresh, and forgets addresses no request has come
 * from for prometheus_source_forget. An error (timeout, SERVFAIL) keeps what
 * is stored; NXDOMAIN stores the unresolved labels.
 *
 * Own identity (prometheus_identity on): worker 0 finds this pod's IP in
 * /etc/hosts by its hostname and resolves it the same way, retrying with
 * backoff (5s up to 60s) until the pod is a Service endpoint.
 *
 * The DNS client is a small one of its own, one UDP socket per worker,
 * because nginx's resolver keeps only the first PTR answer: a pod behind
 * several Services has one PTR record per Service, in no fixed order.
 */

#include "ngx_http_prometheus.h"


#define NGX_PROM_DNS_PENDING     1024
#define NGX_PROM_REFRESH_BATCH   1024
#define NGX_PROM_TICK            1000
#define NGX_PROM_TOUCH_EVERY     60000
#define NGX_PROM_SELF_FIRST      5000
#define NGX_PROM_SELF_MAX        60000

#define NGX_PROM_LOOKUP_CALLER   1
#define NGX_PROM_LOOKUP_SELF     2


typedef struct {
    uint16_t            id;
    uint8_t             kind;           /* 0 = free */
    uint32_t            ip;
    ngx_msec_t          deadline;
} ngx_prom_pending_t;


/* per worker */
static ngx_connection_t   *ngx_prom_dns_conn;
static ngx_log_t           ngx_prom_dns_log;
static ngx_event_t         ngx_prom_tick_ev;
static ngx_prom_pending_t  ngx_prom_pending[NGX_PROM_DNS_PENDING];
static ngx_uint_t          ngx_prom_npending;
static ngx_msec_t          ngx_prom_next_refresh;
static ngx_msec_t          ngx_prom_next_self;
static ngx_msec_t          ngx_prom_self_delay;
static ngx_http_prometheus_main_conf_t  *ngx_prom_pmcf;


static void ngx_prom_dns_read(ngx_event_t *rev);
static void ngx_prom_tick(ngx_event_t *ev);
static void ngx_prom_lookup(uint32_t ip, ngx_uint_t kind);


/* configuration */

static void
ngx_prom_name_set(ngx_prom_name_t *n, const char *s)
{
    n->len = ngx_min(ngx_strlen(s), NGX_PROM_NAME_MAX);
    ngx_memcpy(n->data, s, n->len);
}


static char *
ngx_prom_name_arg(ngx_conf_t *cf, ngx_str_t *v, ngx_prom_name_t *n)
{
    if (v->len == 0 || v->len > NGX_PROM_NAME_MAX) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid label value \"%V\": 1 to %d bytes", v,
                           NGX_PROM_NAME_MAX);
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(n->data, v->data, v->len);
    n->len = v->len;

    return NGX_CONF_OK;
}


static char *
ngx_prom_cidr_arg(ngx_conf_t *cf, ngx_str_t *v, ngx_prom_cidr_t *range)
{
    ngx_cidr_t  cidr;

    if (ngx_ptocidr(v, &cidr) == NGX_ERROR || cidr.family != AF_INET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid IPv4 range \"%V\"", v);
        return NGX_CONF_ERROR;
    }

    range->addr = ntohl(cidr.u.in.addr);
    range->mask = ntohl(cidr.u.in.mask);

    return NGX_CONF_OK;
}


void
ngx_prom_source_create_conf(ngx_prom_source_conf_t *conf)
{
    conf->identity = NGX_CONF_UNSET;
    conf->dns_timeout = NGX_CONF_UNSET_MSEC;
    conf->refresh = NGX_CONF_UNSET_MSEC;
    conf->forget = NGX_CONF_UNSET_MSEC;
    conf->probe_prefix.len = NGX_CONF_UNSET_SIZE;
    conf->waypoint.len = NGX_CONF_UNSET_SIZE;
}


char *
ngx_prom_source_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

    ngx_str_t                *value, *name;
    ngx_uint_t                i;
    ngx_prom_cidr_t          *cidr;
    ngx_prom_source_conf_t   *sc;
    ngx_prom_static_range_t  *range;

    sc = &pmcf->source;
    value = cf->args->elts;
    name = &cmd->name;

#define IS(s)  (name->len == sizeof(s) - 1 \
                && ngx_strncmp(name->data, s, sizeof(s) - 1) == 0)

    if (IS("prometheus_source_ptr")) {
        if (sc->ptr_ranges == NULL) {
            sc->ptr_ranges = ngx_array_create(cf->pool, 2,
                                              sizeof(ngx_prom_cidr_t));
            if (sc->ptr_ranges == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        for (i = 1; i < cf->args->nelts; i++) {
            cidr = ngx_array_push(sc->ptr_ranges);
            if (cidr == NULL
                || ngx_prom_cidr_arg(cf, &value[i], cidr) != NGX_CONF_OK)
            {
                return NGX_CONF_ERROR;
            }
        }

        return NGX_CONF_OK;
    }

    if (IS("prometheus_source_static")) {
        if (sc->static_ranges == NULL) {
            sc->static_ranges = ngx_array_create(cf->pool, 4,
                                        sizeof(ngx_prom_static_range_t));
            if (sc->static_ranges == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        range = ngx_array_push(sc->static_ranges);
        if (range == NULL
            || ngx_prom_cidr_arg(cf, &value[1], &range->cidr) != NGX_CONF_OK
            || ngx_prom_name_arg(cf, &value[2], &range->service)
               != NGX_CONF_OK
            || ngx_prom_name_arg(cf, &value[3], &range->namespace)
               != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (IS("prometheus_source_default")) {
        if (ngx_prom_name_arg(cf, &value[1], &sc->default_service)
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }
        return ngx_prom_name_arg(cf, &value[2], &sc->default_namespace);
    }

    if (IS("prometheus_source_unresolved")) {
        if (ngx_prom_name_arg(cf, &value[1], &sc->unresolved_service)
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }
        return ngx_prom_name_arg(cf, &value[2], &sc->unresolved_namespace);
    }

    if (IS("prometheus_identity_fallback")) {
        if (ngx_prom_name_arg(cf, &value[1], &sc->fallback_app)
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }
        return ngx_prom_name_arg(cf, &value[2], &sc->fallback_namespace);
    }

    if (IS("prometheus_source_probe") || IS("prometheus_source_waypoint")) {
        ngx_str_t  *to = IS("prometheus_source_probe") ? &sc->probe_prefix
                                                       : &sc->waypoint;

        if (value[1].len == 3 && ngx_strncmp(value[1].data, "off", 3) == 0) {
            ngx_str_null(to);

        } else {
            *to = value[1];
        }

        return NGX_CONF_OK;
    }

    if (IS("prometheus_source_ignore_service")) {
#if (NGX_PCRE)
        ngx_regex_t          **re;
        ngx_regex_compile_t    rc;
        u_char                 errstr[NGX_MAX_CONF_ERRSTR];

        if (sc->ignore == NULL) {
            sc->ignore = ngx_array_create(cf->pool, 2, sizeof(ngx_regex_t *));
            if (sc->ignore == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        for (i = 1; i < cf->args->nelts; i++) {
            if (value[i].len == 3
                && ngx_strncmp(value[i].data, "off", 3) == 0)
            {
                sc->ignore->nelts = 0;
                continue;
            }

            ngx_memzero(&rc, sizeof(ngx_regex_compile_t));
            rc.pattern = value[i];
            rc.pool = cf->pool;
            rc.err.len = NGX_MAX_CONF_ERRSTR;
            rc.err.data = errstr;

            if (ngx_regex_compile(&rc) != NGX_OK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%V", &rc.err);
                return NGX_CONF_ERROR;
            }

            re = ngx_array_push(sc->ignore);
            if (re == NULL) {
                return NGX_CONF_ERROR;
            }

            *re = rc.regex;
        }

        return NGX_CONF_OK;
#else
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" needs nginx built with PCRE", name);
        return NGX_CONF_ERROR;
#endif
    }

#undef IS

    return "is unknown";
}


/* the first IPv4 nameserver in /etc/resolv.conf */
static ngx_int_t
ngx_prom_resolv_conf(ngx_conf_t *cf, ngx_str_t *out)
{
    ssize_t    n;
    u_char    *p, *end, *s, buf[4096];
    uint32_t   ip;
    ngx_fd_t   fd;

    fd = ngx_open_file("/etc/resolv.conf", NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        return NGX_DECLINED;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    ngx_close_file(fd);

    if (n <= 0) {
        return NGX_DECLINED;
    }

    end = buf + n;

    for (p = buf; p < end; p++) {
        if (end - p < 11 || ngx_strncmp(p, "nameserver", 10) != 0
            || (p != buf && p[-1] != '\n'))
        {
            continue;
        }

        p += 10;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        s = p;
        while (p < end && *p != '\n' && *p != ' ' && *p != '\t'
               && *p != '\r')
        {
            p++;
        }

        if (ngx_prom_parse_ipv4(s, p - s, &ip)) {
            out->len = p - s;
            out->data = ngx_pstrdup(cf->pool, &(ngx_str_t) { p - s, s });
            return out->data ? NGX_OK : NGX_ERROR;
        }
    }

    return NGX_DECLINED;
}


/* the pod's namespace, as the service account mount has it */
static void
ngx_prom_sa_namespace(ngx_prom_name_t *ns)
{
    ssize_t   n;
    u_char    buf[NGX_PROM_NAME_MAX + 2];
    ngx_fd_t  fd;

    fd = ngx_open_file(
        "/var/run/secrets/kubernetes.io/serviceaccount/namespace",
        NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);

    if (fd == NGX_INVALID_FILE) {
        return;
    }

    n = read(fd, buf, sizeof(buf));
    ngx_close_file(fd);

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
        n--;
    }

    if (n > 0 && n <= NGX_PROM_NAME_MAX) {
        ngx_memcpy(ns->data, buf, n);
        ns->len = n;
    }
}


char *
ngx_prom_source_init_conf(ngx_conf_t *cf,
    ngx_http_prometheus_main_conf_t *pmcf)
{
    ngx_url_t                u;
    ngx_regex_compile_t      rc;
    ngx_prom_source_conf_t  *sc;

    sc = &pmcf->source;

    sc->enabled = pmcf->label[NGX_PROM_L_SOURCE_SERVICE]
                  || pmcf->label[NGX_PROM_L_SOURCE_NAMESPACE];

    ngx_conf_init_value(sc->identity, 0);
    ngx_conf_init_msec_value(sc->dns_timeout, 2000);
    ngx_conf_init_msec_value(sc->refresh, 15000);
    ngx_conf_init_msec_value(sc->forget, 300000);

    if (sc->default_service.len == 0) {
        ngx_prom_name_set(&sc->default_service, "public");
        ngx_prom_name_set(&sc->default_namespace, "unknown");
    }

    if (sc->unresolved_service.len == 0) {
        ngx_prom_name_set(&sc->unresolved_service, "health-probe");
        ngx_prom_name_set(&sc->unresolved_namespace, "kubernetes");
    }

    if (sc->probe_prefix.len == NGX_CONF_UNSET_SIZE) {
        ngx_str_set(&sc->probe_prefix, "kube-probe/");
    }

    if (sc->waypoint.len == NGX_CONF_UNSET_SIZE) {
        ngx_str_set(&sc->waypoint, "waypoint");
    }

    if (sc->cluster_domain.data == NULL) {
        ngx_str_set(&sc->cluster_domain, "cluster.local");
    }

    if (sc->fallback_app.len == 0) {
        ngx_prom_name_set(&sc->fallback_app, "unknown");
        ngx_prom_name_set(&sc->fallback_namespace, "unknown");
        ngx_prom_sa_namespace(&sc->fallback_namespace);
    }

#if (NGX_PCRE)
    if (sc->ignore == NULL) {
        static const char  *defaults[] = { "-metrics$", "-headless$" };
        u_char              errstr[NGX_MAX_CONF_ERRSTR];
        ngx_regex_t       **re;
        ngx_uint_t          i;

        sc->ignore = ngx_array_create(cf->pool, 2, sizeof(ngx_regex_t *));
        if (sc->ignore == NULL) {
            return NGX_CONF_ERROR;
        }

        for (i = 0; i < 2; i++) {
            ngx_memzero(&rc, sizeof(ngx_regex_compile_t));
            rc.pattern.data = (u_char *) defaults[i];
            rc.pattern.len = ngx_strlen(defaults[i]);
            rc.pool = cf->pool;
            rc.err.len = NGX_MAX_CONF_ERRSTR;
            rc.err.data = errstr;

            if (ngx_regex_compile(&rc) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            re = ngx_array_push(sc->ignore);
            if (re == NULL) {
                return NGX_CONF_ERROR;
            }

            *re = rc.regex;
        }
    }
#else
    (void) rc;
#endif

    /* DNS is only needed for PTR ranges or own identity */

    if (!(sc->enabled && sc->ptr_ranges) && !sc->identity) {
        return NGX_CONF_OK;
    }

    if (sc->dns.data == NULL
        || (sc->dns.len == 4 && ngx_strncmp(sc->dns.data, "auto", 4) == 0))
    {
        if (ngx_prom_resolv_conf(cf, &sc->dns) != NGX_OK) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "no IPv4 nameserver in /etc/resolv.conf; "
                               "set \"prometheus_dns\"");
            return NGX_CONF_ERROR;
        }
    }

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.url = sc->dns;
    u.default_port = 53;
    u.no_resolve = 1;

    if (ngx_parse_url(cf->pool, &u) != NGX_OK || u.naddrs == 0
        || u.addrs[0].sockaddr->sa_family != AF_INET)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid prometheus_dns \"%V\": an IPv4 address "
                           "with an optional port", &sc->dns);
        return NGX_CONF_ERROR;
    }

    sc->dns_addr = u.addrs[0];

    return NGX_CONF_OK;
}


void
ngx_prom_source_init_zone(ngx_prom_sh_t *sh)
{
    ngx_rbtree_init(&sh->callers, &sh->callers_sentinel,
                    ngx_rbtree_insert_value);
    ngx_queue_init(&sh->callers_queue);
}


/* the DNS client */

static ngx_int_t
ngx_prom_dns_connect(void)
{
    ngx_int_t          event;
    ngx_socket_t       s;
    ngx_connection_t  *c;
    ngx_addr_t        *addr;

    addr = &ngx_prom_pmcf->source.dns_addr;

    s = ngx_socket(AF_INET, SOCK_DGRAM, 0);
    if (s == (ngx_socket_t) -1) {
        ngx_log_error(NGX_LOG_ALERT, &ngx_prom_dns_log, ngx_socket_errno,
                      "prometheus: " ngx_socket_n " failed");
        return NGX_ERROR;
    }

    c = ngx_get_connection(s, &ngx_prom_dns_log);
    if (c == NULL) {
        ngx_close_socket(s);
        return NGX_ERROR;
    }

    if (ngx_nonblocking(s) == -1
        || connect(s, addr->sockaddr, addr->socklen) == -1)
    {
        ngx_log_error(NGX_LOG_ERR, &ngx_prom_dns_log, ngx_socket_errno,
                      "prometheus: cannot reach the DNS server %V",
                      &addr->name);
        ngx_close_connection(c);
        return NGX_ERROR;
    }

    c->read->handler = ngx_prom_dns_read;
    c->read->log = &ngx_prom_dns_log;
    c->write->log = &ngx_prom_dns_log;
    c->write->ready = 1;
    c->data = NULL;

    event = (ngx_event_flags & NGX_USE_CLEAR_EVENT) ? NGX_CLEAR_EVENT
                                                     : NGX_LEVEL_EVENT;

    if (ngx_add_event(c->read, NGX_READ_EVENT, event) != NGX_OK) {
        ngx_close_connection(c);
        return NGX_ERROR;
    }

    ngx_prom_dns_conn = c;

    return NGX_OK;
}


static void
ngx_prom_lookup(uint32_t ip, ngx_uint_t kind)
{
    u_char               query[64];
    size_t               len;
    ngx_uint_t           i;
    ngx_prom_pending_t  *p;

    if (ngx_prom_dns_conn == NULL && ngx_prom_dns_connect() != NGX_OK) {
        return;
    }

    if (ngx_prom_npending == NGX_PROM_DNS_PENDING) {
        return;                         /* the next refresh asks again */
    }

    p = NULL;

    for (i = 0; i < NGX_PROM_DNS_PENDING; i++) {
        if (ngx_prom_pending[i].kind == 0) {
            p = &ngx_prom_pending[i];
            break;
        }
    }

    if (p == NULL) {
        return;
    }

    p->id = (uint16_t) ngx_random();
    p->ip = ip;
    p->kind = (uint8_t) kind;
    p->deadline = ngx_current_msec + ngx_prom_pmcf->source.dns_timeout;

    len = ngx_prom_dns_ptr_query(ip, p->id, query, sizeof(query));

    if (send(ngx_prom_dns_conn->fd, query, len, 0) != (ssize_t) len) {
        ngx_log_error(NGX_LOG_WARN, &ngx_prom_dns_log, ngx_socket_errno,
                      "prometheus: PTR query not sent");
        p->kind = 0;
        return;
    }

    ngx_prom_npending++;
}


static ngx_prom_caller_t *
ngx_prom_caller_find(ngx_prom_sh_t *sh, uint32_t ip)
{
    ngx_rbtree_node_t  *node, *sentinel;

    node = sh->callers.root;
    sentinel = sh->callers.sentinel;

    while (node != sentinel) {
        if (ip < node->key) {
            node = node->left;

        } else if (ip > node->key) {
            node = node->right;

        } else {
            return (ngx_prom_caller_t *) node;
        }
    }

    return NULL;
}


static int
ngx_prom_ignore(void *data, const ngx_prom_name_t *service)
{
#if (NGX_PCRE)
    ngx_array_t   *ignore = data;
    ngx_regex_t  **re;
    ngx_str_t      s;
    ngx_uint_t     i;

    if (ignore == NULL) {
        return 0;
    }

    s.data = (u_char *) service->data;
    s.len = service->len;
    re = ignore->elts;

    for (i = 0; i < ignore->nelts; i++) {
        if (ngx_regex_exec(re[i], &s, NULL, 0) >= 0) {
            return 1;
        }
    }
#endif

    return 0;
}


static void
ngx_prom_answer(ngx_prom_pending_t *p, ngx_prom_dns_answer_t *a)
{
    int                       picked;
    ngx_prom_sh_t            *sh;
    ngx_prom_name_t           svc, ns;
    ngx_prom_caller_t        *caller;
    ngx_prom_zone_ctx_t      *ctx;
    ngx_prom_source_conf_t   *sc;

    sc = &ngx_prom_pmcf->source;
    ctx = ngx_prom_pmcf->zone->data;

    picked = 0;

    if (a && a->rcode == 0) {
        picked = ngx_prom_ptr_pick(a, sc->cluster_domain.data,
                                   sc->cluster_domain.len, ngx_prom_ignore,
                                   sc->ignore, &svc, &ns);
    }

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;

    if (p->kind == NGX_PROM_LOOKUP_SELF) {
        if (picked == 2) {
            sh->self_service = svc;
            sh->self_namespace = ns;
            sh->self_resolved = 1;
        }

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        if (picked == 2) {
            ngx_log_error(NGX_LOG_NOTICE, &ngx_prom_dns_log, 0,
                          "prometheus: this pod is %*s in %*s",
                          svc.len, svc.data, ns.len, ns.data);
        }

        return;
    }

    caller = ngx_prom_caller_find(sh, p->ip);

    if (caller) {
        if (picked) {
            /* only a pod name: its namespace, the unresolved service */
            caller->service = picked == 2 ? svc : sc->unresolved_service;
            caller->namespace = ns;
            caller->answered = 1;

        } else if (a && a->rcode == 3) {
            /* NXDOMAIN or no usable record: nothing owns this IP now */
            caller->service = sc->unresolved_service;
            caller->namespace = sc->unresolved_namespace;
            caller->answered = 1;

        } else if (a && a->rcode == 0) {
            caller->service = sc->unresolved_service;
            caller->namespace = sc->unresolved_namespace;
            caller->answered = 1;
        }

        /* an error keeps what is stored; the lock expires on its own */
        if (a && (a->rcode == 0 || a->rcode == 3)) {
            caller->inflight = 0;
        }
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);
}


static void
ngx_prom_dns_read(ngx_event_t *rev)
{
    ssize_t                 n;
    u_char                  buf[4096];
    ngx_uint_t              i;
    ngx_prom_pending_t     *p;
    ngx_prom_dns_answer_t   a;

    for ( ;; ) {
        n = recv(ngx_prom_dns_conn->fd, buf, sizeof(buf), 0);

        if (n == -1) {
            if (ngx_socket_errno != NGX_EAGAIN) {
                ngx_log_error(NGX_LOG_WARN, rev->log, ngx_socket_errno,
                              "prometheus: DNS recv() failed");
            }
            break;
        }

        if (ngx_prom_dns_parse(buf, n, &a) != 0) {
            continue;
        }

        for (i = 0; i < NGX_PROM_DNS_PENDING; i++) {
            p = &ngx_prom_pending[i];

            if (p->kind && p->id == a.id && p->ip == a.ip) {
                ngx_prom_answer(p, &a);
                p->kind = 0;
                ngx_prom_npending--;
                break;
            }
        }
    }

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_WARN, rev->log, 0,
                      "prometheus: DNS read event failed");
    }
}


/* the timer: query timeouts on every worker; refreshes on worker 0 */

static void
ngx_prom_refresh(void)
{
    uint32_t             ips[NGX_PROM_REFRESH_BATCH];
    ngx_uint_t           n, i;
    ngx_msec_t           now;
    ngx_queue_t         *q, *next;
    ngx_prom_sh_t       *sh;
    ngx_prom_caller_t   *caller;
    ngx_prom_zone_ctx_t *ctx;

    ctx = ngx_prom_pmcf->zone->data;
    now = ngx_current_msec;
    n = 0;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;

    for (q = ngx_queue_head(&sh->callers_queue);
         q != ngx_queue_sentinel(&sh->callers_queue);
         q = next)
    {
        next = ngx_queue_next(q);
        caller = ngx_queue_data(q, ngx_prom_caller_t, queue);

        if (now - caller->seen > ngx_prom_pmcf->source.forget) {
            ngx_queue_remove(q);
            ngx_rbtree_delete(&sh->callers, &caller->node);
            ngx_slab_free_locked(ctx->shpool, caller);
            sh->ncallers--;
            continue;
        }

        if (n < NGX_PROM_REFRESH_BATCH
            && (caller->inflight == 0 || now > caller->inflight))
        {
            caller->inflight = now + ngx_prom_pmcf->source.dns_timeout + 1000;
            ips[n++] = (uint32_t) caller->node.key;
        }
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    for (i = 0; i < n; i++) {
        ngx_prom_lookup(ips[i], NGX_PROM_LOOKUP_CALLER);
    }
}


static void
ngx_prom_self_try(void)
{
    ssize_t               n;
    u_char                hosts[16384], hostname[256];
    size_t                hlen;
    uint32_t              ip;
    ngx_fd_t              fd;
    ngx_prom_sh_t        *sh;
    ngx_prom_zone_ctx_t  *ctx;

    ctx = ngx_prom_pmcf->zone->data;
    sh = ctx->sh;

    if (sh->self_resolved) {
        return;
    }

    ip = sh->self_ip;

    if (ip == 0) {
        if (gethostname((char *) hostname, sizeof(hostname) - 1) == -1) {
            return;
        }

        hostname[sizeof(hostname) - 1] = '\0';
        hlen = ngx_strlen(hostname);

        fd = ngx_open_file("/etc/hosts", NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
        if (fd == NGX_INVALID_FILE) {
            return;
        }

        n = read(fd, hosts, sizeof(hosts));
        ngx_close_file(fd);

        if (n <= 0
            || !ngx_prom_hosts_find(hosts, n, hostname, hlen, &ip))
        {
            return;
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);
        sh->self_ip = ip;
        sh->self_ip_len = ngx_sprintf(sh->self_ip_text, "%ud.%ud.%ud.%ud",
                                      ip >> 24, (ip >> 16) & 0xff,
                                      (ip >> 8) & 0xff, ip & 0xff)
                          - sh->self_ip_text;
        ngx_shmtx_unlock(&ctx->shpool->mutex);
    }

    ngx_prom_lookup(ip, NGX_PROM_LOOKUP_SELF);
}


static void
ngx_prom_tick(ngx_event_t *ev)
{
    ngx_uint_t           i;
    ngx_msec_t           now;
    ngx_prom_pending_t  *p;

    now = ngx_current_msec;

    for (i = 0; ngx_prom_npending && i < NGX_PROM_DNS_PENDING; i++) {
        p = &ngx_prom_pending[i];

        if (p->kind && (ngx_msec_int_t) (now - p->deadline) > 0) {
            ngx_prom_answer(p, NULL);   /* an error: keep what's stored */
            p->kind = 0;
            ngx_prom_npending--;
        }
    }

    if (ngx_worker == 0) {
        if (ngx_prom_pmcf->source.enabled
            && ngx_prom_pmcf->source.ptr_ranges
            && (ngx_msec_int_t) (now - ngx_prom_next_refresh) >= 0)
        {
            ngx_prom_next_refresh = now + ngx_prom_pmcf->source.refresh;
            ngx_prom_refresh();
        }

        if (ngx_prom_pmcf->source.identity
            && (ngx_msec_int_t) (now - ngx_prom_next_self) >= 0)
        {
            ngx_prom_next_self = now + ngx_prom_self_delay;
            ngx_prom_self_delay = ngx_min(ngx_prom_self_delay * 2,
                                          NGX_PROM_SELF_MAX);
            ngx_prom_self_try();
        }
    }

    if (!ngx_exiting && !ngx_terminate && !ngx_quit) {
        ngx_add_timer(ev, NGX_PROM_TICK);
    }
}


ngx_int_t
ngx_prom_source_init_process(ngx_cycle_t *cycle)
{
    ngx_http_prometheus_main_conf_t  *pmcf;

    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return NGX_OK;
    }

    pmcf = ngx_http_cycle_get_module_main_conf(cycle,
                                               ngx_http_prometheus_module);

    if (pmcf == NULL || pmcf->zone == NULL
        || (!(pmcf->source.enabled && pmcf->source.ptr_ranges)
            && !pmcf->source.identity))
    {
        return NGX_OK;
    }

    ngx_prom_pmcf = pmcf;
    ngx_prom_dns_log = *cycle->log;
    ngx_prom_self_delay = NGX_PROM_SELF_FIRST;
    ngx_prom_next_refresh = ngx_current_msec + pmcf->source.refresh;
    ngx_prom_next_self = ngx_current_msec;

    ngx_prom_tick_ev.handler = ngx_prom_tick;
    ngx_prom_tick_ev.log = &ngx_prom_dns_log;
    ngx_prom_tick_ev.data = NULL;
    ngx_prom_tick_ev.cancelable = 1;

    ngx_add_timer(&ngx_prom_tick_ev, 0);

    return NGX_OK;
}


void
ngx_prom_source_exit_process(ngx_cycle_t *cycle)
{
    if (ngx_prom_dns_conn) {
        ngx_close_connection(ngx_prom_dns_conn);
        ngx_prom_dns_conn = NULL;
    }
}


/* per request */

static ngx_int_t
ngx_prom_request_ip(ngx_http_request_t *r, uint32_t *ip)
{
    struct sockaddr_in  *sin;

    if (r->connection->sockaddr->sa_family != AF_INET) {
        return 0;
    }

    sin = (struct sockaddr_in *) r->connection->sockaddr;
    *ip = ntohl(sin->sin_addr.s_addr);

    return 1;
}


static ngx_flag_t
ngx_prom_in_ptr_range(ngx_prom_source_conf_t *sc, uint32_t ip)
{
    ngx_uint_t        i;
    ngx_prom_cidr_t  *range;

    if (sc->ptr_ranges == NULL) {
        return 0;
    }

    range = sc->ptr_ranges->elts;

    for (i = 0; i < sc->ptr_ranges->nelts; i++) {
        if ((ip & range[i].mask) == range[i].addr) {
            return 1;
        }
    }

    return 0;
}


/* the longest prometheus_source_static range with ip in it */
static ngx_flag_t
ngx_prom_static_find(ngx_prom_source_conf_t *sc, uint32_t ip,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace)
{
    ngx_uint_t                i, best;
    ngx_prom_static_range_t  *range;

    if (sc->static_ranges == NULL) {
        return 0;
    }

    range = sc->static_ranges->elts;
    best = sc->static_ranges->nelts;

    for (i = 0; i < sc->static_ranges->nelts; i++) {
        if ((ip & range[i].cidr.mask) == range[i].cidr.addr
            && (best == sc->static_ranges->nelts
                || range[i].cidr.mask > range[best].cidr.mask))
        {
            best = i;
        }
    }

    if (best == sc->static_ranges->nelts) {
        return 0;
    }

    *service = range[best].service;
    *namespace = range[best].namespace;

    return 1;
}


static ngx_flag_t
ngx_prom_is_probe(ngx_http_request_t *r, ngx_prom_source_conf_t *sc)
{
    ngx_table_elt_t  *ua;

    ua = r->headers_in.user_agent;

    return sc->probe_prefix.len && ua
           && ua->value.len >= sc->probe_prefix.len
           && ngx_strncmp(ua->value.data, sc->probe_prefix.data,
                          sc->probe_prefix.len) == 0;
}


/*
 * Looks an address up in the zone. Returns 1 with the labels if it has an
 * answer; otherwise 0, and the address is added (if new) and a lookup is
 * started (if none is running).
 */
static ngx_int_t
ngx_prom_caller_get(ngx_http_prometheus_main_conf_t *pmcf, uint32_t ip,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace)
{
    ngx_int_t             found, ask;
    ngx_msec_t            now;
    ngx_prom_sh_t        *sh;
    ngx_prom_caller_t    *caller;
    ngx_prom_zone_ctx_t  *ctx;

    ctx = pmcf->zone->data;
    now = ngx_current_msec;
    found = 0;
    ask = 0;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;
    caller = ngx_prom_caller_find(sh, ip);

    if (caller == NULL) {
        caller = ngx_slab_calloc_locked(ctx->shpool,
                                        sizeof(ngx_prom_caller_t));
        if (caller) {
            caller->node.key = ip;
            ngx_rbtree_insert(&sh->callers, &caller->node);
            ngx_queue_insert_tail(&sh->callers_queue, &caller->queue);
            sh->ncallers++;
        }
    }

    if (caller) {
        if (now - caller->seen >= NGX_PROM_TOUCH_EVERY || caller->seen == 0) {
            caller->seen = now;
        }

        if (caller->answered) {
            *service = caller->service;
            *namespace = caller->namespace;
            found = 1;

        } else if (caller->inflight == 0
                   || (ngx_msec_int_t) (now - caller->inflight) > 0)
        {
            caller->inflight = now + pmcf->source.dns_timeout + 1000;
            ask = 1;
        }
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    if (ask && ngx_prom_pmcf) {
        ngx_prom_lookup(ip, NGX_PROM_LOOKUP_CALLER);
    }

    return found;
}


ngx_int_t
ngx_prom_source_post_read(ngx_http_request_t *r)
{
    uint32_t                          ip;
    ngx_prom_request_ctx_t           *rctx;
    ngx_http_prometheus_main_conf_t  *pmcf;

    if (r != r->main
        || ngx_http_get_module_ctx(r, ngx_http_prometheus_module))
    {
        return NGX_DECLINED;
    }

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_prometheus_module);

    if (!ngx_prom_request_ip(r, &ip)
        || !ngx_prom_in_ptr_range(&pmcf->source, ip)
        || ngx_prom_is_probe(r, &pmcf->source))
    {
        return NGX_DECLINED;
    }

    rctx = ngx_pcalloc(r->pool, sizeof(ngx_prom_request_ctx_t));
    if (rctx == NULL) {
        return NGX_DECLINED;
    }

    ngx_http_set_ctx(r, rctx, ngx_http_prometheus_module);

    rctx->done = ngx_prom_caller_get(pmcf, ip, &rctx->service,
                                     &rctx->namespace) ? 1 : 0;

    return NGX_DECLINED;
}


static ngx_table_elt_t *
ngx_prom_header(ngx_http_request_t *r, const char *name, size_t len)
{
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                return NULL;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == len
            && ngx_strncasecmp(h[i].key.data, (u_char *) name, len) == 0)
        {
            return &h[i];
        }
    }
}


void
ngx_prom_source_self(ngx_http_prometheus_main_conf_t *pmcf,
    ngx_prom_name_t *app, ngx_prom_name_t *namespace)
{
    ngx_prom_sh_t        *sh;
    ngx_prom_zone_ctx_t  *ctx;

    *app = pmcf->source.fallback_app;
    *namespace = pmcf->source.fallback_namespace;

    if (!pmcf->source.identity) {
        return;
    }

    ctx = pmcf->zone->data;
    sh = ctx->sh;

    /* one word, written once: no lock needed to see it set */
    if (sh->self_resolved) {
        ngx_shmtx_lock(&ctx->shpool->mutex);
        *app = sh->self_service;
        *namespace = sh->self_namespace;
        ngx_shmtx_unlock(&ctx->shpool->mutex);
    }
}


ngx_flag_t
ngx_prom_source_own_host(ngx_http_prometheus_main_conf_t *pmcf,
    ngx_str_t *host, ngx_prom_name_t *service)
{
    ngx_flag_t            own;
    ngx_prom_sh_t        *sh;
    ngx_prom_zone_ctx_t  *ctx;

    if (!pmcf->source.identity) {
        return 0;
    }

    ctx = pmcf->zone->data;
    sh = ctx->sh;

    if (!sh->self_resolved) {
        return 0;
    }

    own = 0;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    if (host->len == sh->self_ip_len
        && ngx_strncmp(host->data, sh->self_ip_text, host->len) == 0)
    {
        *service = sh->self_service;
        own = 1;
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return own;
}


void
ngx_prom_source_caller(ngx_http_request_t *r, ngx_str_t *mode,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace)
{
    uint32_t                          ip;
    ngx_table_elt_t                  *xfcc;
    ngx_prom_name_t                   xns, xsa;
    ngx_prom_request_ctx_t           *rctx;
    ngx_prom_source_conf_t           *sc;
    ngx_http_prometheus_main_conf_t  *pmcf;

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_prometheus_module);
    sc = &pmcf->source;

    /* the outbound leg: the caller is this nginx */
    if (mode->len == 1 && mode->data[0] == '1') {
        ngx_prom_source_self(pmcf, service, namespace);
        return;
    }

    if (ngx_prom_is_probe(r, sc)) {
        *service = sc->unresolved_service;
        *namespace = sc->unresolved_namespace;
        return;
    }

    *service = sc->default_service;
    *namespace = sc->default_namespace;

    if (!ngx_prom_request_ip(r, &ip)) {
        return;
    }

    /* the PTR ranges first, as in the sidecar: a pod CIDR is never static */

    if (!ngx_prom_in_ptr_range(sc, ip)) {
        (void) ngx_prom_static_find(sc, ip, service, namespace);
        return;
    }

    rctx = ngx_http_get_module_ctx(r, ngx_http_prometheus_module);

    if (rctx && rctx->done) {
        *service = rctx->service;
        *namespace = rctx->namespace;

    } else if (!ngx_prom_caller_get(pmcf, ip, service, namespace)) {
        *service = sc->unresolved_service;
        *namespace = sc->unresolved_namespace;
        return;
    }

    /*
     * The mesh waypoint: trust its XFCC only now that PTR, not the request,
     * has said the peer is the waypoint.
     */

    if (sc->waypoint.len
        && service->len == sc->waypoint.len
        && ngx_strncmp(service->data, sc->waypoint.data, service->len) == 0)
    {
        xfcc = ngx_prom_header(r, "X-Forwarded-Client-Cert",
                               sizeof("X-Forwarded-Client-Cert") - 1);

        if (xfcc && ngx_prom_xfcc_parse(xfcc->value.data, xfcc->value.len,
                                        &xns, &xsa))
        {
            *service = xsa;
            *namespace = xns;
        }
    }
}


/*
 * The caller of a stream session, for ngx_stream_prometheus_module through
 * the zone's ngx_prom_identity_t: the same ranges, cache and DNS client as
 * HTTP, with no User-Agent or XFCC to look at. A configured range comes
 * first; then a loopback or unix-socket client is one of this pod's own
 * containers (in the sidecar, the app), so it is this pod's own identity.
 */
void
ngx_prom_source_stream(void *data, struct sockaddr *sa,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace)
{
    ngx_http_prometheus_main_conf_t *pmcf = data;

    uint32_t                 ip;
    ngx_prom_source_conf_t  *sc;

    sc = &pmcf->source;

    *service = sc->default_service;
    *namespace = sc->default_namespace;

    switch (sa->sa_family) {

    case AF_INET:
        ip = ntohl(((struct sockaddr_in *) sa)->sin_addr.s_addr);

        if (ngx_prom_in_ptr_range(sc, ip)) {
            if (!ngx_prom_caller_get(pmcf, ip, service, namespace)) {
                *service = sc->unresolved_service;
                *namespace = sc->unresolved_namespace;
            }
            return;
        }

        if (ngx_prom_static_find(sc, ip, service, namespace)) {
            return;
        }

        if ((ip >> 24) == 127) {
            ngx_prom_source_self(pmcf, service, namespace);
        }

        return;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        if (IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *) sa)->sin6_addr)) {
            ngx_prom_source_self(pmcf, service, namespace);
        }
        return;
#endif

#if (NGX_HAVE_UNIX_DOMAIN)
    case AF_UNIX:
        ngx_prom_source_self(pmcf, service, namespace);
        return;
#endif

    default:
        return;
    }
}
