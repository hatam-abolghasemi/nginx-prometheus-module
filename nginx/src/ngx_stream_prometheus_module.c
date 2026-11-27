/*
 * ngx_stream_prometheus_module: L4 metrics for every TCP/UDP session nginx's
 * stream module proxies, the same metrics as the Lua module's stream
 * feature. Its own .so, so an nginx built without stream can still load the
 * http module:
 *
 *   load_module modules/ngx_http_prometheus_module.so;
 *   load_module modules/ngx_stream_prometheus_module.so;
 *
 *   stream {
 *       server {
 *           listen 127.0.0.1:16379;
 *           prometheus_destination redis;
 *           proxy_pass redis.example.svc:6379;
 *       }
 *   }
 *
 * Sessions are recorded in the log phase into the "prometheus_stream" zone
 * (made with 1m when there is no prometheus_zone in stream{}). There is no
 * port of its own: the http module finds the zone and serves it on the same
 * /metrics, so one scrape covers both.
 *
 * Labels: destination (prometheus_destination), upstream_addr, status, and
 * (off by default) source_service / source_namespace: who opened the
 * session, worked out by the http module (its ranges, PTR cache and DNS
 * client), which this module finds by its zone's name. With the source
 * labels on, connections_active is kept per caller. The last-value gauges
 * are left out of a scrape once their label set has had no session for
 * prometheus_gauge_expiry; histograms are opt-in.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

#include "ngx_prometheus_shared.h"
#include "ngx_http_prometheus_label.h"


#define NGX_PROM_STREAM_LABELS_MAX  1024


typedef struct {
    ngx_shm_zone_t          *zone;
    ngx_flag_t               zone_off;
    ngx_flag_t               source;     /* a source_* label is on */
    ngx_prom_identity_t     *identity;   /* the http module's, if loaded */
    ngx_prom_stream_conf_t   conf;
} ngx_stream_prometheus_main_conf_t;


typedef struct {
    ngx_flag_t               record;
    ngx_str_t                destination;
} ngx_stream_prometheus_srv_conf_t;


typedef struct {
    ngx_str_t                active;     /* the per-caller series counted */
    ngx_uint_t               generation; /* of the zone it was counted in */
    unsigned                 counted:1;
} ngx_stream_prometheus_ctx_t;


static ngx_prom_stream_metric_t  ngx_prom_stream_metrics[NGX_PROM_S_COUNT]
    = NGX_PROM_STREAM_METRICS;


static const char  *ngx_prom_stream_label_names[NGX_PROM_SL_COUNT] = {
    "destination", "upstream_addr", "status", "source_service",
    "source_namespace"
};

/* the Lua module's defaults: the source labels are opt-in */
static ngx_flag_t  ngx_prom_stream_label_on[NGX_PROM_SL_COUNT] = {
    1, 1, 1, 0, 0
};

static const char  *ngx_prom_stream_hist_names[NGX_PROM_SH_COUNT] = {
    "session_duration_histogram",
    "upstream_connect_time_histogram",
    "upstream_first_byte_time_histogram"
};

static double  ngx_prom_stream_buckets_session[] =
    { 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1, 5, 15, 30, 60 };
static double  ngx_prom_stream_buckets_connect[] =
    { 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1 };


static ngx_int_t ngx_stream_prometheus_init(ngx_conf_t *cf);
static ngx_int_t ngx_stream_prometheus_init_module(ngx_cycle_t *cycle);
static void *ngx_stream_prometheus_create_main_conf(ngx_conf_t *cf);
static char *ngx_stream_prometheus_init_main_conf(ngx_conf_t *cf, void *conf);
static void *ngx_stream_prometheus_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_prometheus_merge_srv_conf(ngx_conf_t *cf,
    void *parent, void *child);
static char *ngx_stream_prometheus_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_stream_prometheus_metric(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_stream_prometheus_label(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_stream_prometheus_buckets(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_stream_prometheus_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);


static ngx_command_t  ngx_stream_prometheus_commands[] = {

    { ngx_string("prometheus_zone"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_stream_prometheus_zone,
      NGX_STREAM_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_metric"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_stream_prometheus_metric,
      NGX_STREAM_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_label"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_stream_prometheus_label,
      NGX_STREAM_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_buckets"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_2MORE,
      ngx_stream_prometheus_buckets,
      NGX_STREAM_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_gauge_expiry"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_STREAM_MAIN_CONF_OFFSET,
      offsetof(ngx_stream_prometheus_main_conf_t, conf.gauge_expiry),
      NULL },

    { ngx_string("prometheus_record"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_prometheus_srv_conf_t, record),
      NULL },

    { ngx_string("prometheus_destination"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_prometheus_srv_conf_t, destination),
      NULL },

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_prometheus_module_ctx = {
    NULL,                                    /* preconfiguration */
    ngx_stream_prometheus_init,              /* postconfiguration */

    ngx_stream_prometheus_create_main_conf,  /* create main configuration */
    ngx_stream_prometheus_init_main_conf,    /* init main configuration */

    ngx_stream_prometheus_create_srv_conf,   /* create server configuration */
    ngx_stream_prometheus_merge_srv_conf     /* merge server configuration */
};


ngx_module_t  ngx_stream_prometheus_module = {
    NGX_MODULE_V1,
    &ngx_stream_prometheus_module_ctx,       /* module context */
    ngx_stream_prometheus_commands,          /* module directives */
    NGX_STREAM_MODULE,                       /* module type */
    NULL,                                    /* init master */
    ngx_stream_prometheus_init_module,       /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    NULL,                                    /* exit process */
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


/* configuration */

static void *
ngx_stream_prometheus_create_main_conf(ngx_conf_t *cf)
{
    ngx_uint_t                          i;
    ngx_stream_prometheus_main_conf_t  *pmcf;

    pmcf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_prometheus_main_conf_t));
    if (pmcf == NULL) {
        return NULL;
    }

    for (i = 0; i < NGX_PROM_S_COUNT; i++) {
        pmcf->conf.metric[i] = NGX_CONF_UNSET;
    }

    for (i = 0; i < NGX_PROM_SL_COUNT; i++) {
        pmcf->conf.label[i] = NGX_CONF_UNSET;
    }

    for (i = 0; i < NGX_PROM_SH_COUNT; i++) {
        pmcf->conf.hist[i].n = NGX_CONF_UNSET_UINT;
    }

    pmcf->conf.gauge_expiry = NGX_CONF_UNSET_MSEC;

    return pmcf;
}


static ngx_int_t
ngx_stream_prometheus_add_zone(ngx_conf_t *cf,
    ngx_stream_prometheus_main_conf_t *pmcf, ssize_t size)
{
    ngx_str_t               name;
    ngx_prom_stream_ctx_t  *ctx;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_prom_stream_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->magic = NGX_PROM_STREAM_MAGIC;
    ctx->conf = &pmcf->conf;

    ngx_str_set(&name, NGX_PROM_STREAM_ZONE_NAME);

    pmcf->zone = ngx_shared_memory_add(cf, &name, size,
                                       &ngx_stream_prometheus_module);
    if (pmcf->zone == NULL) {
        return NGX_ERROR;
    }

    pmcf->zone->init = ngx_stream_prometheus_init_zone;
    pmcf->zone->data = ctx;

    return NGX_OK;
}


static char *
ngx_stream_prometheus_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_stream_prometheus_main_conf_t *pmcf = conf;

    size_t                   size;
    uint32_t                 crc;
    ngx_int_t                h;
    ngx_uint_t               i, j;
    ngx_prom_stream_conf_t  *sc;

    sc = &pmcf->conf;

    for (i = 0; i < NGX_PROM_S_COUNT; i++) {
        ngx_conf_init_value(sc->metric[i], ngx_prom_stream_metrics[i].on);
    }

    for (i = 0; i < NGX_PROM_SL_COUNT; i++) {
        ngx_conf_init_value(sc->label[i], ngx_prom_stream_label_on[i]);
    }

    pmcf->source = sc->label[NGX_PROM_SL_SOURCE_SERVICE]
                   || sc->label[NGX_PROM_SL_SOURCE_NAMESPACE];

    for (i = 0; i < NGX_PROM_SH_COUNT; i++) {
        double      *v = (i == NGX_PROM_SH_SESSION)
                         ? ngx_prom_stream_buckets_session
                         : ngx_prom_stream_buckets_connect;
        ngx_uint_t   n = (i == NGX_PROM_SH_SESSION) ? 11 : 7;

        if (sc->hist[i].n == NGX_CONF_UNSET_UINT) {
            sc->hist[i].n = n;
            for (j = 0; j < n; j++) {
                sc->hist[i].le[j] = v[j];
            }
        }
    }

    ngx_conf_init_msec_value(sc->gauge_expiry, 15000);

    sc->tail = 0;

    for (i = 0; i < NGX_PROM_S_COUNT; i++) {
        h = ngx_prom_stream_metrics[i].hist;

        if (h < 0 || !sc->metric[i]) {
            continue;
        }

        size = (sc->hist[h].n + 1) * sizeof(uint64_t) + sizeof(double);
        sc->hist[h].offset = sc->tail;
        sc->tail += size;
    }

    ngx_crc32_init(crc);
    ngx_crc32_update(&crc, (u_char *) sc->metric, sizeof(sc->metric));
    ngx_crc32_update(&crc, (u_char *) sc->label, sizeof(sc->label));
    for (i = 0; i < NGX_PROM_SH_COUNT; i++) {
        ngx_crc32_update(&crc, (u_char *) &sc->hist[i].n, sizeof(ngx_uint_t));
        ngx_crc32_update(&crc, (u_char *) sc->hist[i].le,
                         sc->hist[i].n * sizeof(double));
    }
    ngx_crc32_final(crc);

    sc->layout = crc;

    /* loaded and a stream{} block is enough: with no prometheus_zone, 1m */
    if (pmcf->zone == NULL && !pmcf->zone_off) {
        if (ngx_stream_prometheus_add_zone(cf, pmcf, 1024 * 1024) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static void *
ngx_stream_prometheus_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_prometheus_srv_conf_t  *pscf;

    pscf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_prometheus_srv_conf_t));
    if (pscf == NULL) {
        return NULL;
    }

    pscf->record = NGX_CONF_UNSET;

    return pscf;
}


static char *
ngx_stream_prometheus_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_stream_prometheus_srv_conf_t *prev = parent;
    ngx_stream_prometheus_srv_conf_t *conf = child;

    ngx_conf_merge_value(conf->record, prev->record, 1);
    ngx_conf_merge_str_value(conf->destination, prev->destination, "unknown");

    return NGX_CONF_OK;
}


static char *
ngx_stream_prometheus_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_prometheus_main_conf_t *pmcf = conf;

    ssize_t     size;
    ngx_str_t  *value;

    if (pmcf->zone || pmcf->zone_off) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (value[1].len == 3 && ngx_strncmp(value[1].data, "off", 3) == 0) {
        pmcf->zone_off = 1;
        return NGX_CONF_OK;
    }

    size = ngx_parse_size(&value[1]);

    if (size == NGX_ERROR || size < (ssize_t) (8 * ngx_pagesize)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid zone size \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (ngx_stream_prometheus_add_zone(cf, pmcf, size) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_stream_prometheus_flag(ngx_conf_t *cf, ngx_str_t *value, ngx_flag_t *flag)
{
    if (ngx_strcmp(value->data, "on") == 0) {
        *flag = 1;

    } else if (ngx_strcmp(value->data, "off") == 0) {
        *flag = 0;

    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid value \"%V\", it must be \"on\" or \"off\"",
                           value);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_stream_prometheus_find(ngx_str_t *name, const char **names, ngx_uint_t n)
{
    ngx_uint_t  i;

    for (i = 0; i < n; i++) {
        if (name->len == ngx_strlen(names[i])
            && ngx_strncmp(name->data, names[i], name->len) == 0)
        {
            return i;
        }
    }

    return NGX_ERROR;
}


static char *
ngx_stream_prometheus_metric(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_prometheus_main_conf_t *pmcf = conf;

    ngx_str_t   *value;
    ngx_uint_t   i;

    value = cf->args->elts;

    for (i = 0; i < NGX_PROM_S_COUNT; i++) {
        if (value[1].len == ngx_strlen(ngx_prom_stream_metrics[i].name)
            && ngx_strncmp(value[1].data, ngx_prom_stream_metrics[i].name,
                           value[1].len) == 0)
        {
            return ngx_stream_prometheus_flag(cf, &value[2],
                                              &pmcf->conf.metric[i]);
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unknown stream metric \"%V\"",
                       &value[1]);
    return NGX_CONF_ERROR;
}


static char *
ngx_stream_prometheus_label(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_prometheus_main_conf_t *pmcf = conf;

    ngx_int_t   i;
    ngx_str_t  *value;

    value = cf->args->elts;

    i = ngx_stream_prometheus_find(&value[1], ngx_prom_stream_label_names,
                                   NGX_PROM_SL_COUNT);
    if (i == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown stream label \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    return ngx_stream_prometheus_flag(cf, &value[2], &pmcf->conf.label[i]);
}


static char *
ngx_stream_prometheus_buckets(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_prometheus_main_conf_t *pmcf = conf;

    double                 v;
    u_char                *end;
    ngx_int_t              h;
    ngx_str_t             *value;
    ngx_uint_t             i;
    ngx_prom_hist_conf_t  *hist;

    value = cf->args->elts;

    h = ngx_stream_prometheus_find(&value[1], ngx_prom_stream_hist_names,
                                   NGX_PROM_SH_COUNT);
    if (h == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "unknown stream histogram \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    hist = &pmcf->conf.hist[h];

    if (hist->n != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    if (cf->args->nelts - 2 > NGX_PROM_MAX_BUCKETS) {
        return "has too many buckets";
    }

    hist->n = 0;

    for (i = 2; i < cf->args->nelts; i++) {
        v = strtod((char *) value[i].data, (char **) &end);

        if (end != value[i].data + value[i].len || value[i].len == 0
            || (hist->n > 0 && v <= hist->le[hist->n - 1]))
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid bucket \"%V\": buckets must be "
                               "numbers in increasing order", &value[i]);
            return NGX_CONF_ERROR;
        }

        hist->le[hist->n++] = v;
    }

    return NGX_CONF_OK;
}


/* shared memory */

static ngx_int_t
ngx_stream_prometheus_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_prom_stream_ctx_t  *octx = data;

    ngx_queue_t               *q;
    ngx_prom_stream_ctx_t     *ctx;
    ngx_prom_stream_active_t  *a;
    ngx_prom_stream_series_t  *s;

    ctx = shm_zone->data;

    if (octx) {
        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        if (ctx->sh->layout == ctx->conf->layout) {
            return NGX_OK;
        }

        /* the series layout changed with the config: start over */
        ngx_shmtx_lock(&ctx->shpool->mutex);

        while (!ngx_queue_empty(&ctx->sh->queue)) {
            q = ngx_queue_head(&ctx->sh->queue);
            s = ngx_queue_data(q, ngx_prom_stream_series_t, queue);
            ngx_queue_remove(q);
            ngx_rbtree_delete(&ctx->sh->rbtree, &s->sn.node);
            ngx_slab_free_locked(ctx->shpool, s);
        }

        while (!ngx_queue_empty(&ctx->sh->active_queue)) {
            q = ngx_queue_head(&ctx->sh->active_queue);
            a = ngx_queue_data(q, ngx_prom_stream_active_t, queue);
            ngx_queue_remove(q);
            ngx_rbtree_delete(&ctx->sh->active_rbtree, &a->sn.node);
            ngx_slab_free_locked(ctx->shpool, a);
        }

        ctx->sh->nseries = 0;
        ctx->sh->bytes = 0;
        ctx->sh->active_bytes = 0;
        ctx->sh->generation++;
        ctx->sh->layout = ctx->conf->layout;

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    ctx->sh = ngx_slab_calloc(ctx->shpool, sizeof(ngx_prom_stream_sh_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;
    ctx->sh->layout = ctx->conf->layout;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_str_rbtree_insert_value);
    ngx_queue_init(&ctx->sh->queue);

    ngx_rbtree_init(&ctx->sh->active_rbtree, &ctx->sh->active_sentinel,
                    ngx_str_rbtree_insert_value);
    ngx_queue_init(&ctx->sh->active_queue);

    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


/* recording */

static ngx_inline void
ngx_prom_stream_observe(u_char *tail, ngx_prom_hist_conf_t *hc, double v)
{
    uint64_t    *count;
    double      *sum;
    ngx_uint_t   i;

    count = (uint64_t *) (tail + hc->offset);
    sum = (double *) (count + hc->n + 1);

    for (i = 0; i < hc->n; i++) {
        if (v <= hc->le[i]) {
            break;
        }
    }

    count[i]++;
    *sum += v;
}


static size_t
ngx_prom_stream_label(u_char *buf, size_t o, const char *name,
    const u_char *value, size_t len)
{
    size_t  n;

    n = ngx_strlen(name);

    if (o + n + 4 > NGX_PROM_STREAM_LABELS_MAX) {
        return o;
    }

    if (o) {
        buf[o++] = ',';
    }

    o = ngx_cpymem(buf + o, name, n) - buf;
    buf[o++] = '=';
    buf[o++] = '"';
    o += ngx_prom_label_escape(value, len, buf + o,
                               NGX_PROM_STREAM_LABELS_MAX - o - 1);
    buf[o++] = '"';

    return o;
}


/*
 * The source labels of a session, as text ("source_service=..." and/or
 * "source_namespace=..."), appended at o. Without the http module every
 * caller is "unknown".
 */
static size_t
ngx_prom_stream_source(ngx_stream_session_t *s,
    ngx_stream_prometheus_main_conf_t *pmcf, u_char *buf, size_t o)
{
    ngx_prom_name_t  svc, ns;

    if (pmcf->identity) {
        pmcf->identity->source(pmcf->identity->data, s->connection->sockaddr,
                               &svc, &ns);

    } else {
        svc.len = sizeof("unknown") - 1;
        ngx_memcpy(svc.data, "unknown", svc.len);
        ns = svc;
    }

    if (pmcf->conf.label[NGX_PROM_SL_SOURCE_SERVICE]) {
        o = ngx_prom_stream_label(buf, o, "source_service", svc.data,
                                  svc.len);
    }

    if (pmcf->conf.label[NGX_PROM_SL_SOURCE_NAMESPACE]) {
        o = ngx_prom_stream_label(buf, o, "source_namespace", ns.data,
                                  ns.len);
    }

    return o;
}


/* +1 or -1 on the per-caller active series; called with the zone locked */
static ngx_int_t
ngx_prom_stream_active_add(ngx_prom_stream_ctx_t *ctx, ngx_str_t *key,
    int64_t n)
{
    size_t                     size;
    uint32_t                   hash;
    ngx_prom_stream_sh_t      *sh;
    ngx_prom_stream_active_t  *a;

    sh = ctx->sh;
    hash = ngx_crc32_short(key->data, key->len);

    a = (ngx_prom_stream_active_t *)
            ngx_str_rbtree_lookup(&sh->active_rbtree, key, hash);

    if (a == NULL) {
        if (n < 0) {
            return NGX_DECLINED;
        }

        size = ngx_prom_stream_active_size(key->len);

        a = ngx_slab_calloc_locked(ctx->shpool, size);
        if (a == NULL) {
            sh->errors++;
            return NGX_ERROR;
        }

        ngx_memcpy(a->data, key->data, key->len);
        a->sn.str.data = a->data;
        a->sn.str.len = key->len;
        a->sn.node.key = hash;

        ngx_rbtree_insert(&sh->active_rbtree, &a->sn.node);
        ngx_queue_insert_tail(&sh->active_queue, &a->queue);
        sh->active_bytes += size;
    }

    if (n < 0 && a->count <= 0) {
        return NGX_DECLINED;
    }

    a->count += n;

    return NGX_OK;
}


static ngx_int_t
ngx_stream_prometheus_preaccess(ngx_stream_session_t *s)
{
    size_t                              len;
    ngx_str_t                           key;
    ngx_prom_stream_ctx_t              *ctx;
    ngx_stream_prometheus_ctx_t        *pctx;
    ngx_stream_prometheus_srv_conf_t   *pscf;
    ngx_stream_prometheus_main_conf_t  *pmcf;
    u_char                              labels[NGX_PROM_STREAM_LABELS_MAX];

    pscf = ngx_stream_get_module_srv_conf(s, ngx_stream_prometheus_module);
    pmcf = ngx_stream_get_module_main_conf(s, ngx_stream_prometheus_module);

    if (!pscf->record || pmcf->zone == NULL
        || !pmcf->conf.metric[NGX_PROM_S_ACTIVE]
        || ngx_stream_get_module_ctx(s, ngx_stream_prometheus_module))
    {
        return NGX_DECLINED;
    }

    pctx = ngx_pcalloc(s->connection->pool,
                       sizeof(ngx_stream_prometheus_ctx_t));
    if (pctx == NULL) {
        return NGX_DECLINED;
    }

    ngx_stream_set_ctx(s, pctx, ngx_stream_prometheus_module);

    ctx = pmcf->zone->data;

    if (!pmcf->source) {
        /* the one per-session cost before the session ends: an atomic add */
        (void) ngx_atomic_fetch_add(&ctx->sh->active, 1);
        pctx->counted = 1;
        return NGX_DECLINED;
    }

    /*
     * Per caller: as known now (a first session from a new address starts
     * its lookup here). The key is kept so the end of the session takes one
     * off the same series even if the answer has changed by then.
     */

    len = ngx_prom_stream_label(labels, 0, "state", (u_char *) "active",
                                sizeof("active") - 1);
    len = ngx_prom_stream_source(s, pmcf, labels, len);

    key.data = ngx_pnalloc(s->connection->pool, len);
    if (key.data == NULL) {
        return NGX_DECLINED;
    }

    key.len = len;
    ngx_memcpy(key.data, labels, len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    if (ngx_prom_stream_active_add(ctx, &key, 1) == NGX_OK) {
        pctx->active = key;
        pctx->generation = ctx->sh->generation;
        pctx->counted = 1;
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_DECLINED;
}


static ngx_int_t
ngx_stream_prometheus_log(ngx_stream_session_t *s)
{
    size_t                              len, total;
    u_char                             *tail, *p;
    double                              session, connect, first_byte;
    uint32_t                            hash;
    ngx_str_t                           key, upstream;
    ngx_msec_t                          now, ms;
    ngx_flag_t                          connected;
    ngx_time_t                         *tp;
    ngx_prom_stream_sh_t               *sh;
    ngx_prom_stream_ctx_t              *ctx;
    ngx_prom_stream_conf_t             *sc;
    ngx_prom_stream_series_t           *ser;
    ngx_stream_upstream_state_t        *state;
    ngx_stream_prometheus_ctx_t        *pctx;
    ngx_stream_prometheus_srv_conf_t   *pscf;
    ngx_stream_prometheus_main_conf_t  *pmcf;
    off_t                               up_sent, up_received;
    u_char                              labels[NGX_PROM_STREAM_LABELS_MAX];
    u_char                              num[NGX_INT_T_LEN];

    pmcf = ngx_stream_get_module_main_conf(s, ngx_stream_prometheus_module);

    if (pmcf->zone == NULL) {
        return NGX_OK;
    }

    ctx = pmcf->zone->data;
    sc = &pmcf->conf;

    pctx = ngx_stream_get_module_ctx(s, ngx_stream_prometheus_module);

    if (pctx && pctx->counted) {
        pctx->counted = 0;

        if (pctx->active.data == NULL) {
            (void) ngx_atomic_fetch_add(&ctx->sh->active, -1);

        } else {
            ngx_shmtx_lock(&ctx->shpool->mutex);

            /* a reset since (a reload with new labels) has dropped it */
            if (pctx->generation == ctx->sh->generation) {
                (void) ngx_prom_stream_active_add(ctx, &pctx->active, -1);
            }

            ngx_shmtx_unlock(&ctx->shpool->mutex);
        }
    }

    pscf = ngx_stream_get_module_srv_conf(s, ngx_stream_prometheus_module);

    if (!pscf->record) {
        return NGX_OK;
    }

    /* the upstream attempt that carried the session is the last one */

    connected = 0;
    connect = 0;
    first_byte = -1;
    up_sent = 0;
    up_received = 0;
    ngx_str_set(&upstream, "none");

    if (s->upstream_states && s->upstream_states->nelts) {
        state = s->upstream_states->elts;
        state = &state[s->upstream_states->nelts - 1];

        if (state->peer) {
            upstream = *state->peer;
        }

        if (state->connect_time != (ngx_msec_t) -1) {
            connected = 1;
            connect = (double) state->connect_time / 1000;
        }

        if (state->first_byte_time != (ngx_msec_t) -1) {
            first_byte = (double) state->first_byte_time / 1000;
        }

        up_sent = state->bytes_sent;
        up_received = state->bytes_received;
    }

    len = 0;

    if (sc->label[NGX_PROM_SL_DESTINATION]) {
        len = ngx_prom_stream_label(labels, len, "destination",
                                    pscf->destination.data,
                                    pscf->destination.len);
    }

    if (sc->label[NGX_PROM_SL_UPSTREAM_ADDR]) {
        len = ngx_prom_stream_label(labels, len, "upstream_addr",
                                    upstream.data, upstream.len);
    }

    if (sc->label[NGX_PROM_SL_STATUS]) {
        p = ngx_sprintf(num, "%03ui", s->status);
        len = ngx_prom_stream_label(labels, len, "status", num, p - num);
    }

    if (pmcf->source) {
        len = ngx_prom_stream_source(s, pmcf, labels, len);
    }

    key.data = labels;
    key.len = len;

    tp = ngx_timeofday();
    ms = (ngx_msec_t) ((tp->sec - s->start_sec) * 1000
                       + (tp->msec - s->start_msec));
    session = (double) ms / 1000;
    now = ngx_current_msec;

    hash = ngx_crc32_short(key.data, key.len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;

    ser = (ngx_prom_stream_series_t *)
              ngx_str_rbtree_lookup(&sh->rbtree, &key, hash);

    if (ser == NULL) {
        total = ngx_prom_stream_size(key.len, sc->tail);

        ser = ngx_slab_calloc_locked(ctx->shpool, total);

        if (ser == NULL) {
            sh->errors++;
            ngx_shmtx_unlock(&ctx->shpool->mutex);

            ngx_log_error(NGX_LOG_WARN, s->connection->log, 0,
                          "prometheus_stream zone is full, session not "
                          "recorded; raise prometheus_zone in stream{}");
            return NGX_OK;
        }

        ngx_memcpy(ser->data, key.data, key.len);
        ser->sn.str.data = (u_char *) ser->data;
        ser->sn.str.len = key.len;
        ser->sn.node.key = hash;

        ngx_rbtree_insert(&sh->rbtree, &ser->sn.node);
        ngx_queue_insert_tail(&sh->queue, &ser->queue);
        sh->nseries++;
        sh->bytes += total;
    }

    tail = (u_char *) ser->data + ngx_align(ser->sn.str.len, sizeof(uint64_t));

    ser->counter[NGX_PROM_SC_CONNECTIONS]++;
    ser->counter[NGX_PROM_SC_BYTES_SENT] += s->connection->sent;
    ser->counter[NGX_PROM_SC_BYTES_RECEIVED] += s->received;
    ser->counter[NGX_PROM_SC_UP_BYTES_SENT] += up_sent;
    ser->counter[NGX_PROM_SC_UP_BYTES_RECEIVED] += up_received;

    ser->gauge[NGX_PROM_SG_SESSION] = session;
    ser->touched = now;

    if (sc->metric[NGX_PROM_S_SESSION_HIST]) {
        ngx_prom_stream_observe(tail, &sc->hist[NGX_PROM_SH_SESSION],
                                session);
    }

    if (connected) {
        ser->gauge[NGX_PROM_SG_CONNECT] = connect;
        ser->up_touched = now;

        if (sc->metric[NGX_PROM_S_CONNECT_HIST]) {
            ngx_prom_stream_observe(tail, &sc->hist[NGX_PROM_SH_CONNECT],
                                    connect);
        }
    }

    if (first_byte >= 0 && sc->metric[NGX_PROM_S_FIRST_BYTE_HIST]) {
        ngx_prom_stream_observe(tail, &sc->hist[NGX_PROM_SH_FIRST_BYTE],
                                first_byte);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}


static ngx_int_t
ngx_stream_prometheus_init(ngx_conf_t *cf)
{
    ngx_stream_handler_pt              *h;
    ngx_stream_core_main_conf_t        *cmcf;
    ngx_stream_prometheus_main_conf_t  *pmcf;

    pmcf = ngx_stream_conf_get_module_main_conf(cf,
                                                ngx_stream_prometheus_module);
    if (pmcf->zone == NULL) {
        return NGX_OK;
    }

    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_prometheus_preaccess;

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_prometheus_log;

    return NGX_OK;
}


/*
 * The http module's caller identity, found by its zone's name: the two
 * .so files never link to each other, and either can be loaded first.
 */
static ngx_int_t
ngx_stream_prometheus_init_module(ngx_cycle_t *cycle)
{
    ngx_uint_t                          i;
    ngx_shm_zone_t                     *zone;
    ngx_list_part_t                    *part;
    ngx_prom_identity_t                *id;
    ngx_stream_prometheus_main_conf_t  *pmcf;

    pmcf = ngx_stream_cycle_get_module_main_conf(cycle,
                                                 ngx_stream_prometheus_module);

    if (pmcf == NULL || !pmcf->source || pmcf->zone == NULL) {
        return NGX_OK;
    }

    pmcf->identity = NULL;

    part = &cycle->shared_memory.part;
    zone = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            zone = part->elts;
            i = 0;
        }

        if (zone[i].shm.name.len != sizeof(NGX_PROM_HTTP_ZONE_NAME) - 1
            || ngx_strncmp(zone[i].shm.name.data, NGX_PROM_HTTP_ZONE_NAME,
                           zone[i].shm.name.len) != 0)
        {
            continue;
        }

        id = zone[i].data;

        if (id && id->magic == NGX_PROM_IDENTITY_MAGIC) {
            pmcf->identity = id;
        }

        break;
    }

    if (pmcf->identity == NULL) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "prometheus: the stream source labels need "
                      "ngx_http_prometheus_module with its zone; every "
                      "caller is \"unknown\"");
    }

    return NGX_OK;
}
