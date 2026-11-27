/*
 * ngx_http_prometheus_module: Prometheus metrics for every HTTP request nginx
 * serves, kept in one shared memory zone and exposed by a `prometheus;`
 * location. No Lua, no exporter process, no access log to parse.
 *
 *   load_module modules/ngx_http_prometheus_module.so;
 *
 * is all it takes: with no prometheus_zone a 4m zone is made, and with no
 * `prometheus;` location anywhere the module listens on :9145 itself
 * (prometheus_listen) and serves /metrics and /healthz there, whatever the
 * rest of nginx.conf says.
 *
 * Every request is recorded in the log phase: one lookup of its label set in
 * the zone and a few additions, under the zone's mutex (the same pattern as
 * limit_req). A scrape copies the zone out under that mutex, then formats the
 * copy without holding it.
 *
 * Gauges hold the last value seen for a label set and are left out of a
 * scrape once that label set has gone quiet for prometheus_gauge_expiry: with
 * routes as a label, histograms multiply the series count, so a last-value
 * gauge is the cheap way to keep latency and sizes. Histograms are opt-in.
 */

#include "ngx_http_prometheus.h"


typedef struct {
    const char   *name;          /* directive name */
    const char   *metric;        /* exported name */
    const char   *help;
    const char   *type;
    ngx_flag_t    on;            /* default */
    ngx_int_t     hist;          /* NGX_PROM_H_*, or -1 */
    ngx_int_t     gauge;         /* NGX_PROM_G_*, or -1 */
} ngx_prom_metric_def_t;


static ngx_prom_metric_def_t  ngx_prom_metrics[NGX_PROM_M_COUNT] = {
    { "requests", "nginx_http_requests_total",
      "Number of HTTP requests", "counter", 1, -1, -1 },
    { "request_duration", "nginx_http_request_duration_seconds",
      "Duration of last HTTP request in seconds", "gauge", 1,
      -1, NGX_PROM_G_DURATION },
    { "request_duration_histogram",
      "nginx_http_request_duration_seconds_histogram",
      "HTTP request duration histogram in seconds", "histogram", 0,
      NGX_PROM_H_DURATION, -1 },
    { "request_size", "nginx_http_request_size_bytes",
      "Size of last HTTP request in bytes", "gauge", 1,
      -1, NGX_PROM_G_REQ_SIZE },
    { "request_size_histogram", "nginx_http_request_size_bytes_histogram",
      "HTTP request size histogram in bytes", "histogram", 0,
      NGX_PROM_H_REQ_SIZE, -1 },
    { "response_size", "nginx_http_response_size_bytes",
      "Size of last HTTP response in bytes", "gauge", 1,
      -1, NGX_PROM_G_RESP_SIZE },
    { "response_size_histogram", "nginx_http_response_size_bytes_histogram",
      "HTTP response size histogram in bytes", "histogram", 0,
      NGX_PROM_H_RESP_SIZE, -1 },
    { "upstream_response_time", "nginx_http_upstream_response_time_seconds",
      "Upstream response time of last proxied HTTP request in seconds",
      "gauge", 1, -1, NGX_PROM_G_UP_RESPONSE },
    { "upstream_response_time_histogram",
      "nginx_http_upstream_response_time_seconds_histogram",
      "Upstream response time histogram in seconds", "histogram", 0,
      NGX_PROM_H_UP_RESPONSE, -1 },
    { "upstream_connect_time", "nginx_http_upstream_connect_time_seconds",
      "Upstream connect time of last proxied HTTP request in seconds",
      "gauge", 0, -1, NGX_PROM_G_UP_CONNECT },
    { "upstream_header_time", "nginx_http_upstream_header_time_seconds",
      "Upstream time to response header of last proxied HTTP request "
      "in seconds", "gauge", 0, -1, NGX_PROM_G_UP_HEADER },
    { "connections", "nginx_http_connections",
      "Number of HTTP connections by state", "gauge", 1, -1, -1 },
    { "connection_requests", "nginx_connection_requests",
      "Number of requests made on the current connection", "gauge", 1,
      -1, -1 },
    { "connection_requests_histogram", "nginx_connection_requests_histogram",
      "Distribution of requests per connection", "histogram", 0,
      NGX_PROM_H_CONN_REQUESTS, -1 },
    { "connection_time", "nginx_connection_time_seconds",
      "Time the current connection has been open in seconds", "gauge", 1,
      -1, -1 },
    { "connection_time_histogram", "nginx_connection_time_seconds_histogram",
      "Distribution of connection lifetimes in seconds", "histogram", 0,
      NGX_PROM_H_CONN_TIME, -1 },
    { "connection_slots_limit", "nginx_connection_slots_limit",
      "Connection slots available: worker_connections x worker processes",
      "gauge", 1, -1, -1 },
};


static ngx_prom_stream_metric_t  ngx_prom_stream_metrics[NGX_PROM_S_COUNT]
    = NGX_PROM_STREAM_METRICS;


static const char  *ngx_prom_label_names[NGX_PROM_L_COUNT] = {
    "method", "route", "status", "client_subnet", "host", "app", "namespace",
    "mode", "source_service", "source_namespace", "protocol"
};


/* the sidecar's defaults: identity labels are opt-in */
static ngx_flag_t  ngx_prom_label_on[NGX_PROM_L_COUNT] = {
    1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 1
};


static const char  *ngx_prom_hist_names[NGX_PROM_H_COUNT] = {
    "request_duration_histogram",
    "request_size_histogram",
    "response_size_histogram",
    "upstream_response_time_histogram",
    "connection_requests_histogram",
    "connection_time_histogram"
};


static double  ngx_prom_default_buckets_time[] =
    { 0.01, 0.025, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9,
      1, 2.5, 5, 10 };
static double  ngx_prom_default_buckets_size[] =
    { 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576 };
static double  ngx_prom_default_buckets_conn_requests[] =
    { 1, 2, 5, 10, 25, 50, 100, 250 };
static double  ngx_prom_default_buckets_conn_time[] =
    { 0.1, 0.5, 1, 5, 15, 30, 60, 300 };


static struct {
    double      *v;
    ngx_uint_t   n;
} ngx_prom_default_buckets[NGX_PROM_H_COUNT] = {
    { ngx_prom_default_buckets_time, 16 },
    { ngx_prom_default_buckets_size, 8 },
    { ngx_prom_default_buckets_size, 8 },
    { ngx_prom_default_buckets_time, 16 },
    { ngx_prom_default_buckets_conn_requests, 8 },
    { ngx_prom_default_buckets_conn_time, 8 },
};


static struct {
    const char  *name;
    unsigned     bit;
} ngx_prom_route_rules[] = {
    { "pure_numeric", NGX_PROM_RULE_PURE_NUMERIC },
    { "uuid", NGX_PROM_RULE_UUID },
    { "hex", NGX_PROM_RULE_HEX },
    { "slug_suffix_digit", NGX_PROM_RULE_SLUG_SUFFIX_DIGIT },
    { "long_with_digit", NGX_PROM_RULE_LONG_WITH_DIGIT },
    { "file_extension", NGX_PROM_RULE_FILE_EXTENSION },
    { NULL, 0 }
};


#define ngx_prom_data(p)  ((u_char *) (p)->data)


typedef struct {
    ngx_str_node_t   sn;          /* key: the rendered label set */
    ngx_queue_t      queue;
    uint64_t         requests;
    double           gauge[NGX_PROM_G_COUNT];
    ngx_msec_t       touched;     /* last request of this label set */
    ngx_msec_t       up_touched;  /* last proxied request of this label set */
    /* label text, then histograms: uint64_t only for the alignment */
    uint64_t         data[1];
} ngx_prom_series_t;


/* histograms in a tail: n+1 per-bucket counts (the last is +Inf), then sum */
#define NGX_PROM_HEADER_TAIL_MAX                                              \
    (2 * ((NGX_PROM_MAX_BUCKETS + 1) * sizeof(uint64_t) + sizeof(double)))


static ngx_int_t ngx_http_prometheus_init(ngx_conf_t *cf);
static void *ngx_http_prometheus_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_prometheus_init_main_conf(ngx_conf_t *cf, void *conf);
static void *ngx_http_prometheus_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_prometheus_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_prometheus_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_prometheus_metric(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_prometheus_label(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_prometheus_buckets(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_prometheus_route_rule(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_prometheus(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_prometheus_add_zone(ngx_conf_t *cf,
    ngx_http_prometheus_main_conf_t *pmcf, ssize_t size);
static ngx_int_t ngx_http_prometheus_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_int_t ngx_http_prometheus_log_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_prometheus_handler(ngx_http_request_t *r);


static ngx_command_t  ngx_http_prometheus_commands[] = {

    { ngx_string("prometheus_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_prometheus_zone,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_metric"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_prometheus_metric,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_label"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_prometheus_label,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_buckets"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
      ngx_http_prometheus_buckets,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_gauge_expiry"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, gauge_expiry),
      NULL },

    { ngx_string("prometheus_route_rule"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_prometheus_route_rule,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_route_min_len"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, route_min_len),
      NULL },

    { ngx_string("prometheus_route_max_segment_len"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, route_max_segment_len),
      NULL },

    { ngx_string("prometheus_cluster_ip_prefix"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, cluster_ip_prefix),
      NULL },

    { ngx_string("prometheus_source_ptr"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_1MORE,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_static"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_default"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_unresolved"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_probe"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_waypoint"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_ignore_service"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_1MORE,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_identity_fallback"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_prom_source_directive,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("prometheus_source_refresh"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, source.refresh),
      NULL },

    { ngx_string("prometheus_source_forget"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, source.forget),
      NULL },

    { ngx_string("prometheus_dns"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, source.dns),
      NULL },

    { ngx_string("prometheus_dns_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, source.dns_timeout),
      NULL },

    { ngx_string("prometheus_cluster_domain"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, source.cluster_domain),
      NULL },

    { ngx_string("prometheus_identity"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, source.identity),
      NULL },

    { ngx_string("prometheus_listen"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_prometheus_main_conf_t, listen),
      NULL },

    { ngx_string("prometheus_record"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_prometheus_loc_conf_t, record),
      NULL },

    { ngx_string("prometheus_mode"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_prometheus_loc_conf_t, mode),
      NULL },

    { ngx_string("prometheus"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_prometheus,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_prometheus_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_prometheus_init,              /* postconfiguration */

    ngx_http_prometheus_create_main_conf,  /* create main configuration */
    ngx_http_prometheus_init_main_conf,    /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_prometheus_create_loc_conf,   /* create location configuration */
    ngx_http_prometheus_merge_loc_conf     /* merge location configuration */
};


ngx_module_t  ngx_http_prometheus_module = {
    NGX_MODULE_V1,
    &ngx_http_prometheus_module_ctx,       /* module context */
    ngx_http_prometheus_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_prom_source_init_process,          /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_prom_source_exit_process,          /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/* configuration */

static void *
ngx_http_prometheus_create_main_conf(ngx_conf_t *cf)
{
    ngx_uint_t                        i;
    ngx_http_prometheus_main_conf_t  *pmcf;

    pmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_prometheus_main_conf_t));
    if (pmcf == NULL) {
        return NULL;
    }

    for (i = 0; i < NGX_PROM_M_COUNT; i++) {
        pmcf->metric[i] = NGX_CONF_UNSET;
    }

    for (i = 0; i < NGX_PROM_L_COUNT; i++) {
        pmcf->label[i] = NGX_CONF_UNSET;
    }

    for (i = 0; i < NGX_PROM_H_COUNT; i++) {
        pmcf->hist[i].n = NGX_CONF_UNSET_UINT;
    }

    pmcf->gauge_expiry = NGX_CONF_UNSET_MSEC;
    pmcf->route.rules = NGX_PROM_RULE_ALL;
    pmcf->route_min_len = NGX_CONF_UNSET_UINT;
    pmcf->route_max_segment_len = NGX_CONF_UNSET_UINT;

    ngx_prom_source_create_conf(&pmcf->source);

    return pmcf;
}


static char *
ngx_http_prometheus_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

    size_t      size;
    uint32_t    crc;
    ngx_uint_t  i, j;

    /* loaded is enough: with no prometheus_zone, a 4m zone */
    if (pmcf->zone == NULL && !pmcf->zone_off) {
        if (ngx_http_prometheus_add_zone(cf, pmcf, 4 * 1024 * 1024)
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    if (pmcf->listen.data == NULL) {
        ngx_str_set(&pmcf->listen, "9145");
    }

    for (i = 0; i < NGX_PROM_M_COUNT; i++) {
        ngx_conf_init_value(pmcf->metric[i], ngx_prom_metrics[i].on);
    }

    for (i = 0; i < NGX_PROM_L_COUNT; i++) {
        ngx_conf_init_value(pmcf->label[i], ngx_prom_label_on[i]);
    }

    for (i = 0; i < NGX_PROM_H_COUNT; i++) {
        if (pmcf->hist[i].n == NGX_CONF_UNSET_UINT) {
            pmcf->hist[i].n = ngx_prom_default_buckets[i].n;
            for (j = 0; j < pmcf->hist[i].n; j++) {
                pmcf->hist[i].le[j] = ngx_prom_default_buckets[i].v[j];
            }
        }
    }

    ngx_conf_init_msec_value(pmcf->gauge_expiry, 15000);
    ngx_conf_init_uint_value(pmcf->route_min_len, 4);
    ngx_conf_init_uint_value(pmcf->route_max_segment_len, 64);

    if (ngx_prom_source_init_conf(cf, pmcf) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    pmcf->route.min_len = pmcf->route_min_len;
    pmcf->route.max_segment_len = pmcf->route_max_segment_len;

    /*
     * Histogram space: per-request histograms sit after each series' label
     * text, connection histograms after the zone header. Only enabled ones
     * take space.
     */

    pmcf->series_tail = 0;
    pmcf->header_tail = 0;

    for (i = 0; i < NGX_PROM_M_COUNT; i++) {
        ngx_int_t  h = ngx_prom_metrics[i].hist;

        if (h < 0 || !pmcf->metric[i]) {
            continue;
        }

        size = (pmcf->hist[h].n + 1) * sizeof(uint64_t) + sizeof(double);

        if (h == NGX_PROM_H_CONN_REQUESTS || h == NGX_PROM_H_CONN_TIME) {
            pmcf->hist[h].offset = pmcf->header_tail;
            pmcf->header_tail += size;

        } else {
            pmcf->hist[h].offset = pmcf->series_tail;
            pmcf->series_tail += size;
        }
    }

    /*
     * A zone kept over a reload is reused only if the series it holds still
     * mean the same: same metrics, buckets, labels and label shaping.
     */

    ngx_crc32_init(crc);
    ngx_crc32_update(&crc, (u_char *) pmcf->metric, sizeof(pmcf->metric));
    ngx_crc32_update(&crc, (u_char *) pmcf->label, sizeof(pmcf->label));
    ngx_crc32_update(&crc, (u_char *) &pmcf->route, sizeof(pmcf->route));
    ngx_crc32_update(&crc, pmcf->cluster_ip_prefix.data,
                     pmcf->cluster_ip_prefix.len);
    for (i = 0; i < NGX_PROM_H_COUNT; i++) {
        ngx_crc32_update(&crc, (u_char *) &pmcf->hist[i].n, sizeof(ngx_uint_t));
        ngx_crc32_update(&crc, (u_char *) pmcf->hist[i].le,
                         pmcf->hist[i].n * sizeof(double));
    }
    ngx_crc32_final(crc);

    pmcf->layout = crc;

    if (pmcf->zone) {
        ((ngx_prom_zone_ctx_t *) pmcf->zone->data)->layout = crc;
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_prometheus_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_prometheus_loc_conf_t  *plcf;

    plcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_prometheus_loc_conf_t));
    if (plcf == NULL) {
        return NULL;
    }

    plcf->record = NGX_CONF_UNSET;
    plcf->handler = NGX_CONF_UNSET;

    return plcf;
}


static char *
ngx_http_prometheus_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_prometheus_loc_conf_t *prev = parent;
    ngx_http_prometheus_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->handler, 0, 0);

    /* the metrics location itself is not recorded unless asked to */
    if (conf->record == NGX_CONF_UNSET && conf->handler) {
        conf->record = 0;
    }

    ngx_conf_merge_value(conf->record, prev->record, 1);
    ngx_conf_merge_str_value(conf->mode, prev->mode, "0");

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_prometheus_add_zone(ngx_conf_t *cf,
    ngx_http_prometheus_main_conf_t *pmcf, ssize_t size)
{
    ngx_str_t             name;
    ngx_prom_zone_ctx_t  *ctx;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_prom_zone_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_str_set(&name, NGX_PROM_ZONE_NAME);

    pmcf->zone = ngx_shared_memory_add(cf, &name, size,
                                       &ngx_http_prometheus_module);
    if (pmcf->zone == NULL) {
        return NGX_ERROR;
    }

    pmcf->zone->init = ngx_http_prometheus_init_zone;
    pmcf->zone->data = ctx;

    return NGX_OK;
}


static char *
ngx_http_prometheus_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

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

    if (size == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid zone size \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (size < (ssize_t) (8 * ngx_pagesize)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "zone \"%V\" is too small", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (ngx_http_prometheus_add_zone(cf, pmcf, size) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_prometheus_find(ngx_str_t *name, const char **names, ngx_uint_t n)
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
ngx_http_prometheus_flag(ngx_conf_t *cf, ngx_str_t *value, ngx_flag_t *flag)
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


static char *
ngx_http_prometheus_metric(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

    ngx_str_t   *value;
    ngx_uint_t   i;

    value = cf->args->elts;

    for (i = 0; i < NGX_PROM_M_COUNT; i++) {
        if (value[1].len == ngx_strlen(ngx_prom_metrics[i].name)
            && ngx_strncmp(value[1].data, ngx_prom_metrics[i].name,
                           value[1].len) == 0)
        {
            return ngx_http_prometheus_flag(cf, &value[2], &pmcf->metric[i]);
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unknown metric \"%V\"",
                       &value[1]);
    return NGX_CONF_ERROR;
}


static char *
ngx_http_prometheus_label(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

    ngx_int_t   i;
    ngx_str_t  *value;

    value = cf->args->elts;

    i = ngx_http_prometheus_find(&value[1], ngx_prom_label_names,
                                 NGX_PROM_L_COUNT);
    if (i == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unknown label \"%V\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    return ngx_http_prometheus_flag(cf, &value[2], &pmcf->label[i]);
}


static char *
ngx_http_prometheus_buckets(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

    double                 v;
    u_char                *end;
    ngx_int_t              h;
    ngx_str_t             *value;
    ngx_uint_t             i;
    ngx_prom_hist_conf_t  *hist;

    value = cf->args->elts;

    h = ngx_http_prometheus_find(&value[1], ngx_prom_hist_names,
                                 NGX_PROM_H_COUNT);
    if (h == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unknown histogram \"%V\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    hist = &pmcf->hist[h];

    if (hist->n != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    if (cf->args->nelts - 2 > NGX_PROM_MAX_BUCKETS) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "too many buckets, at most %d",
                           NGX_PROM_MAX_BUCKETS);
        return NGX_CONF_ERROR;
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


static char *
ngx_http_prometheus_route_rule(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_main_conf_t *pmcf = conf;

    char        *rv;
    ngx_str_t   *value;
    ngx_uint_t   i;
    ngx_flag_t   on;

    value = cf->args->elts;

    for (i = 0; ngx_prom_route_rules[i].name; i++) {
        if (value[1].len == ngx_strlen(ngx_prom_route_rules[i].name)
            && ngx_strncmp(value[1].data, ngx_prom_route_rules[i].name,
                           value[1].len) == 0)
        {
            rv = ngx_http_prometheus_flag(cf, &value[2], &on);
            if (rv != NGX_CONF_OK) {
                return rv;
            }

            if (on) {
                pmcf->route.rules |= ngx_prom_route_rules[i].bit;

            } else {
                pmcf->route.rules &= ~ngx_prom_route_rules[i].bit;
            }

            return NGX_CONF_OK;
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "unknown route rule \"%V\"",
                       &value[1]);
    return NGX_CONF_ERROR;
}


static char *
ngx_http_prometheus(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_prometheus_loc_conf_t *plcf = conf;

    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_prometheus_main_conf_t  *pmcf;

    if (plcf->handler != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    plcf->handler = 1;

    pmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_prometheus_module);
    pmcf->has_location = 1;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_prometheus_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_prometheus_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt              *h;
    ngx_http_core_main_conf_t        *cmcf;
    ngx_http_prometheus_main_conf_t  *pmcf;

    pmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_prometheus_module);

    if (pmcf->zone == NULL) {
        /* the module is loaded but not used: record nothing */
        return NGX_OK;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_prometheus_log_handler;

    /* no `prometheus;` anywhere: serve the metrics on prometheus_listen */
    if (!pmcf->has_location
        && !(pmcf->listen.len == 3
             && ngx_strncmp(pmcf->listen.data, "off", 3) == 0)
        && ngx_prom_listen_add(cf, pmcf) != NGX_OK)
    {
        return NGX_ERROR;
    }

    /* PTR lookups start before the content, after realip */
    if (pmcf->source.enabled && pmcf->source.ptr_ranges) {
        h = ngx_array_push(&cmcf->phases[NGX_HTTP_PRECONTENT_PHASE].handlers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        *h = ngx_prom_source_post_read;
    }

    return NGX_OK;
}


/* shared memory */

static ngx_int_t
ngx_http_prometheus_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_prom_zone_ctx_t  *octx = data;

    size_t                len;
    ngx_queue_t          *q;
    ngx_prom_series_t    *s;
    ngx_prom_zone_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
        /* a reload: counters carry over unless the series layout changed */

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        if (ctx->sh->layout == ctx->layout) {
            return NGX_OK;
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        while (!ngx_queue_empty(&ctx->sh->queue)) {
            q = ngx_queue_head(&ctx->sh->queue);
            s = ngx_queue_data(q, ngx_prom_series_t, queue);
            ngx_queue_remove(q);
            ngx_rbtree_delete(&ctx->sh->rbtree, &s->sn.node);
            ngx_slab_free_locked(ctx->shpool, s);
        }

        ctx->sh->nseries = 0;
        ctx->sh->bytes = 0;
        ctx->sh->conn_touched = 0;
        ngx_memzero(ngx_prom_data(ctx->sh), NGX_PROM_HEADER_TAIL_MAX);
        ctx->sh->layout = ctx->layout;

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    ctx->sh = ngx_slab_calloc(ctx->shpool,
                              sizeof(ngx_prom_sh_t) + NGX_PROM_HEADER_TAIL_MAX);
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;
    ctx->sh->layout = ctx->layout;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_str_rbtree_insert_value);
    ngx_prom_source_init_zone(ctx->sh);
    ngx_queue_init(&ctx->sh->queue);

    len = sizeof(" in prometheus zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in prometheus zone \"%V\"%Z",
                &shm_zone->shm.name);

    /* a full zone is reported by nginx_metric_errors_total, not per request */
    ctx->shpool->log_nomem = 0;

    return NGX_OK;
}


/* recording */

static ngx_inline void
ngx_prom_observe(u_char *tail, ngx_prom_hist_conf_t *hc, double v)
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

    /* counts are per bucket here; cumulated when rendered */
    count[i]++;
    *sum += v;
}


static size_t
ngx_prom_append_label(u_char *buf, size_t o, const char *name,
    const u_char *value, size_t len)
{
    size_t  n;

    n = ngx_strlen(name);

    /* ,name="" must fit; a label that doesn't is left out */
    if (o + n + 4 > NGX_PROM_LABELS_MAX) {
        return o;
    }

    if (o) {
        buf[o++] = ',';
    }

    o = ngx_cpymem(buf + o, name, n) - buf;
    buf[o++] = '=';
    buf[o++] = '"';

    /* leave room for the closing quote */
    o += ngx_prom_label_escape(value, len, buf + o,
                               NGX_PROM_LABELS_MAX - o - 1);
    buf[o++] = '"';

    return o;
}


static ngx_str_t *
ngx_prom_request_host(ngx_http_request_t *r)
{
    ngx_http_core_srv_conf_t  *cscf;

    if (r->headers_in.server.len) {
        return &r->headers_in.server;
    }

    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);

    return &cscf->server_name;
}


static ngx_flag_t
ngx_prom_is_grpc(ngx_http_request_t *r)
{
    ngx_str_t  *ct;

    if (r->headers_in.content_type == NULL) {
        return 0;
    }

    ct = &r->headers_in.content_type->value;

    if (ct->len < 16
        || ngx_strncasecmp(ct->data, (u_char *) "application/grpc", 16) != 0)
    {
        return 0;
    }

    return ct->len == 16 || ct->data[16] == '+' || ct->data[16] == ';'
           || ct->data[16] == ' ' || ct->data[16] == '\t';
}


static ngx_inline ngx_flag_t
ngx_prom_is_connect(ngx_http_request_t *r)
{
    return r->method_name.len == 7
           && ngx_strncmp(r->method_name.data, "CONNECT", 7) == 0;
}


/*
 * A CONNECT's route is its target, "host:port", with the host classified
 * like the host label (an IP is "$ip", a cluster IP "kubernetes"): the
 * destinations a pod calls, not one series per address.
 */
static size_t
ngx_prom_connect_route(ngx_http_request_t *r,
    ngx_http_prometheus_main_conf_t *pmcf, u_char *out, size_t cap)
{
    u_char        *p, *end, *target, *colon;
    size_t         n, hlen;
    const u_char  *h;

    p = r->request_line.data;
    end = p + r->request_line.len;

    while (p < end && *p != ' ') {
        p++;
    }

    target = ++p;

    while (p < end && *p != ' ') {
        p++;
    }

    if (target >= end) {
        return 0;
    }

    /* the port is after the last ':' (an IPv6 target is [addr]:port) */
    for (colon = p; colon > target && colon[-1] != ':'; colon--) {
        /* void */
    }

    if (colon == target) {
        colon = p + 1;              /* no port */
    }

    h = ngx_prom_host_classify(target, colon - 1 - target,
                               pmcf->cluster_ip_prefix.data,
                               pmcf->cluster_ip_prefix.len, &hlen);

    n = ngx_min(hlen, cap);
    ngx_memcpy(out, h, n);

    if (colon <= p && n + 1 + (p - colon) <= cap) {
        out[n++] = ':';
        n = ngx_cpymem(out + n, colon, p - colon) - out;
    }

    return n;
}


/* X-Forwarded-For's first address, else the peer, masked; or "unknown" */
static size_t
ngx_prom_client_subnet_label(ngx_http_request_t *r, u_char *out)
{
    u_char              *p, *s, *end;
    uint32_t             ip;
    ngx_uint_t           i;
    ngx_list_part_t     *part;
    ngx_table_elt_t     *h;
    struct sockaddr_in  *sin;

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == 15
            && ngx_strncasecmp(h[i].key.data, (u_char *) "X-Forwarded-For",
                               15) == 0)
        {
            p = h[i].value.data;
            end = p + h[i].value.len;

            while (p < end && *p == ' ') {
                p++;
            }

            s = p;

            while (p < end && *p != ',' && *p != ' ') {
                p++;
            }

            if (ngx_prom_parse_ipv4(s, p - s, &ip)) {
                return ngx_prom_client_subnet(ip, out);
            }

            return ngx_cpymem(out, "unknown", 7) - out;
        }
    }

    if (r->connection->sockaddr->sa_family == AF_INET) {
        sin = (struct sockaddr_in *) r->connection->sockaddr;
        return ngx_prom_client_subnet(ntohl(sin->sin_addr.s_addr), out);
    }

    return ngx_cpymem(out, "unknown", 7) - out;
}


#define ngx_prom_series_size(len, tail)                                       \
    (offsetof(ngx_prom_series_t, data) + ngx_align(len, sizeof(uint64_t))     \
     + (tail))


static ngx_int_t
ngx_http_prometheus_log_handler(ngx_http_request_t *r)
{
    size_t                            len, n, size, total;
    u_char                           *tail, *p;
    double                            duration, up_resp, up_conn, up_head;
    uint32_t                          hash;
    ngx_msec_t                        now, ms;
    ngx_str_t                         key, *host;
    ngx_uint_t                        status;
    ngx_flag_t                        upstream, tls;
    ngx_time_t                       *tp;
    ngx_connection_t                 *c;
    ngx_prom_sh_t                    *sh;
    ngx_prom_series_t                *s;
    ngx_prom_zone_ctx_t              *ctx;
    ngx_http_upstream_state_t        *state;
    const u_char                     *v;
    ngx_http_prometheus_loc_conf_t   *plcf;
    ngx_http_prometheus_main_conf_t  *pmcf;
    u_char                            labels[NGX_PROM_LABELS_MAX];
    u_char                            route[NGX_PROM_ROUTE_MAX];
    u_char                            num[NGX_INT_T_LEN + 16];
    ngx_prom_name_t                   name, name2;

    if (r != r->main) {
        return NGX_OK;
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_prometheus_module);

    if (!plcf->record) {
        return NGX_OK;
    }

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_prometheus_module);
    ctx = pmcf->zone->data;
    c = r->connection;

    tp = ngx_timeofday();
    now = ngx_current_msec;

    /* the label set */

    len = 0;

    if (pmcf->label[NGX_PROM_L_METHOD]) {
        len = ngx_prom_append_label(labels, len, "method",
                                    r->method_name.data, r->method_name.len);
    }

    if (pmcf->label[NGX_PROM_L_ROUTE]) {
        if (ngx_prom_is_connect(r)) {
            n = ngx_prom_connect_route(r, pmcf, route, sizeof(route));

        } else {
            n = ngx_prom_route_sanitize(r->unparsed_uri.data,
                                        r->unparsed_uri.len, &pmcf->route,
                                        route, sizeof(route));
        }

        len = ngx_prom_append_label(labels, len, "route", route, n);
    }

    if (pmcf->label[NGX_PROM_L_STATUS]) {
        status = r->err_status ? r->err_status : r->headers_out.status;
        p = ngx_sprintf(num, "%03ui", status);
        len = ngx_prom_append_label(labels, len, "status", num, p - num);
    }

    if (pmcf->label[NGX_PROM_L_CLIENT_SUBNET]) {
        n = ngx_prom_client_subnet_label(r, num);
        len = ngx_prom_append_label(labels, len, "client_subnet", num, n);
    }

    if (pmcf->label[NGX_PROM_L_HOST]) {
        host = ngx_prom_request_host(r);

        if (ngx_prom_source_own_host(pmcf, host, &name)) {
            /* this pod's own IP: the name callers reach it by */
            v = name.data;
            n = name.len;

        } else {
            v = ngx_prom_host_classify(host->data, host->len,
                                       pmcf->cluster_ip_prefix.data,
                                       pmcf->cluster_ip_prefix.len, &n);
        }

        len = ngx_prom_append_label(labels, len, "host", v, n);
    }

    if (pmcf->label[NGX_PROM_L_APP] || pmcf->label[NGX_PROM_L_NAMESPACE]) {
        ngx_prom_source_self(pmcf, &name, &name2);

        if (pmcf->label[NGX_PROM_L_APP]) {
            len = ngx_prom_append_label(labels, len, "app", name.data,
                                        name.len);
        }

        if (pmcf->label[NGX_PROM_L_NAMESPACE]) {
            len = ngx_prom_append_label(labels, len, "namespace", name2.data,
                                        name2.len);
        }
    }

    if (pmcf->label[NGX_PROM_L_MODE]) {
        len = ngx_prom_append_label(labels, len, "mode",
                                    plcf->mode.data, plcf->mode.len);
    }

    if (pmcf->source.enabled) {
        ngx_prom_source_caller(r, &plcf->mode, &name, &name2);

        if (pmcf->label[NGX_PROM_L_SOURCE_SERVICE]) {
            len = ngx_prom_append_label(labels, len, "source_service",
                                        name.data, name.len);
        }

        if (pmcf->label[NGX_PROM_L_SOURCE_NAMESPACE]) {
            len = ngx_prom_append_label(labels, len, "source_namespace",
                                        name2.data, name2.len);
        }
    }

    if (pmcf->label[NGX_PROM_L_PROTOCOL]) {
        tls = 0;
#if (NGX_HTTP_SSL)
        tls = (c->ssl != NULL);
#endif
        if (ngx_prom_is_connect(r)) {
            v = (u_char *) "tunnel";

        } else if (ngx_prom_is_grpc(r)) {
            v = (u_char *) (tls ? "grpcs" : "grpc");

        } else {
            v = (u_char *) (tls ? "https" : "http");
        }

        len = ngx_prom_append_label(labels, len, "protocol", v,
                                    ngx_strlen(v));
    }

    key.data = labels;
    key.len = len;

    /* the values */

    ms = (ngx_msec_t) ((tp->sec - r->start_sec) * 1000
                       + (tp->msec - r->start_msec));
    duration = (double) ms / 1000;

    upstream = 0;
    up_resp = 0;
    up_conn = 0;
    up_head = 0;

    if (r->upstream_states && r->upstream_states->nelts) {
        /* the attempt that answered is the last one */
        state = r->upstream_states->elts;
        state = &state[r->upstream_states->nelts - 1];

        if (state->peer) {
            upstream = 1;
            up_resp = (double) state->response_time / 1000;
            up_conn = state->connect_time == (ngx_msec_t) -1
                      ? 0 : (double) state->connect_time / 1000;
            up_head = state->header_time == (ngx_msec_t) -1
                      ? 0 : (double) state->header_time / 1000;
        }
    }

    size = c->sent > (off_t) r->header_size ? c->sent - r->header_size : 0;

    hash = ngx_crc32_short(key.data, key.len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;

    s = (ngx_prom_series_t *) ngx_str_rbtree_lookup(&sh->rbtree, &key, hash);

    if (s == NULL) {
        total = ngx_prom_series_size(key.len, pmcf->series_tail);

        s = ngx_slab_calloc_locked(ctx->shpool, total);

        if (s == NULL) {
            sh->errors++;
            ngx_shmtx_unlock(&ctx->shpool->mutex);

            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "prometheus zone is full, request not recorded; "
                          "raise prometheus_zone or drop a label");
            return NGX_OK;
        }

        ngx_memcpy(ngx_prom_data(s), key.data, key.len);
        s->sn.str.data = ngx_prom_data(s);
        s->sn.str.len = key.len;
        s->sn.node.key = hash;

        ngx_rbtree_insert(&sh->rbtree, &s->sn.node);
        ngx_queue_insert_tail(&sh->queue, &s->queue);
        sh->nseries++;
        sh->bytes += total;
    }

    tail = ngx_prom_data(s) + ngx_align(s->sn.str.len, sizeof(uint64_t));

    s->requests++;
    s->touched = now;

    s->gauge[NGX_PROM_G_DURATION] = duration;
    s->gauge[NGX_PROM_G_REQ_SIZE] = (double) r->request_length;
    s->gauge[NGX_PROM_G_RESP_SIZE] = (double) size;

    if (pmcf->metric[NGX_PROM_M_DURATION_HIST]) {
        ngx_prom_observe(tail, &pmcf->hist[NGX_PROM_H_DURATION], duration);
    }

    if (pmcf->metric[NGX_PROM_M_REQ_SIZE_HIST]) {
        ngx_prom_observe(tail, &pmcf->hist[NGX_PROM_H_REQ_SIZE],
                         (double) r->request_length);
    }

    if (pmcf->metric[NGX_PROM_M_RESP_SIZE_HIST]) {
        ngx_prom_observe(tail, &pmcf->hist[NGX_PROM_H_RESP_SIZE],
                         (double) size);
    }

    if (upstream) {
        s->up_touched = now;
        s->gauge[NGX_PROM_G_UP_RESPONSE] = up_resp;
        s->gauge[NGX_PROM_G_UP_CONNECT] = up_conn;
        s->gauge[NGX_PROM_G_UP_HEADER] = up_head;

        if (pmcf->metric[NGX_PROM_M_UP_RESPONSE_HIST]) {
            ngx_prom_observe(tail, &pmcf->hist[NGX_PROM_H_UP_RESPONSE],
                             up_resp);
        }
    }

    /* connection-level: no request labels, a connection carries many */

    sh->conn_requests = (double) c->requests;
    sh->conn_time = (double) (now - c->start_time) / 1000;
    sh->conn_touched = now;

    if (pmcf->metric[NGX_PROM_M_CONN_REQUESTS_HIST]) {
        ngx_prom_observe(ngx_prom_data(sh),
                         &pmcf->hist[NGX_PROM_H_CONN_REQUESTS],
                         sh->conn_requests);
    }

    if (pmcf->metric[NGX_PROM_M_CONN_TIME_HIST]) {
        ngx_prom_observe(ngx_prom_data(sh),
                         &pmcf->hist[NGX_PROM_H_CONN_TIME], sh->conn_time);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}


/* exposition */

typedef struct {
    ngx_pool_t          *pool;
    ngx_chain_t         *out;
    ngx_chain_t        **last;
    ngx_buf_t           *b;
    ngx_int_t            error;
} ngx_prom_writer_t;


#define NGX_PROM_CHUNK  (64 * 1024)
#define NGX_PROM_LINE   (NGX_PROM_LABELS_MAX + 256)


static u_char *
ngx_prom_reserve(ngx_prom_writer_t *w, size_t need)
{
    ngx_chain_t  *cl;

    if (w->b && (size_t) (w->b->end - w->b->last) >= need) {
        return w->b->last;
    }

    w->b = ngx_create_temp_buf(w->pool, ngx_max(NGX_PROM_CHUNK, need));
    if (w->b == NULL) {
        w->error = 1;
        return NULL;
    }

    cl = ngx_alloc_chain_link(w->pool);
    if (cl == NULL) {
        w->error = 1;
        return NULL;
    }

    cl->buf = w->b;
    cl->next = NULL;
    *w->last = cl;
    w->last = &cl->next;

    return w->b->last;
}


static void
ngx_prom_header_raw(ngx_prom_writer_t *w, const char *metric,
    const char *help, const char *type)
{
    u_char  *p;

    p = ngx_prom_reserve(w, NGX_PROM_LINE);
    if (p == NULL) {
        return;
    }

    w->b->last = ngx_sprintf(p, "# HELP %s %s\n# TYPE %s %s\n",
                             metric, help, metric, type);
}


static void
ngx_prom_header(ngx_prom_writer_t *w, ngx_prom_metric_def_t *m)
{
    ngx_prom_header_raw(w, m->metric, m->help, m->type);
}


/* a float the way Prometheus reads it: integers plainly, others %.6f */
static u_char *
ngx_prom_float(u_char *p, double v)
{
    int64_t   i;
    uint64_t  frac;

    if (v < 0) {
        *p++ = '-';
        v = -v;
    }

    i = (int64_t) v;

    if ((double) i == v) {
        return ngx_sprintf(p, "%L", i);
    }

    frac = (uint64_t) ((v - (double) i) * 1000000 + 0.5);

    if (frac >= 1000000) {
        i++;
        frac -= 1000000;
    }

    p = ngx_sprintf(p, "%L.%06uL", i, frac);

    /* trim trailing zeros */
    while (p[-1] == '0') {
        p--;
    }

    if (p[-1] == '.') {
        p--;
    }

    return p;
}


static void
ngx_prom_line(ngx_prom_writer_t *w, const char *name, const char *suffix,
    u_char *labels, size_t len, const char *extra, double v)
{
    u_char  *p;

    p = ngx_prom_reserve(w, NGX_PROM_LINE);
    if (p == NULL) {
        return;
    }

    p = ngx_sprintf(p, "%s%s", name, suffix);

    if (len || extra) {
        *p++ = '{';

        if (len) {
            p = ngx_cpymem(p, labels, len);
        }

        if (extra) {
            if (len) {
                *p++ = ',';
            }
            p = ngx_sprintf(p, "%s", extra);
        }

        *p++ = '}';
    }

    *p++ = ' ';
    p = ngx_prom_float(p, v);
    *p++ = '\n';

    w->b->last = p;
}


static void
ngx_prom_histogram(ngx_prom_writer_t *w, const char *name, u_char *labels,
    size_t len, u_char *tail, ngx_prom_hist_conf_t *hc)
{
    u_char      *p, le[64];
    double      *sum;
    uint64_t    *count, cum;
    ngx_uint_t   i;

    count = (uint64_t *) (tail + hc->offset);
    sum = (double *) (count + hc->n + 1);

    cum = 0;

    for (i = 0; i <= hc->n; i++) {
        cum += count[i];

        if (i < hc->n) {
            p = ngx_cpymem(le, "le=\"", 4);
            p = ngx_prom_float(p, hc->le[i]);

        } else {
            p = ngx_cpymem(le, "le=\"+Inf", 8);
        }

        *p++ = '"';
        *p = '\0';

        ngx_prom_line(w, name, "_bucket", labels, len, (char *) le,
                      (double) cum);
    }

    ngx_prom_line(w, name, "_sum", labels, len, NULL, *sum);
    ngx_prom_line(w, name, "_count", labels, len, NULL, (double) cum);
}


/*
 * The stream module's zone, if there is one: found by name in this cycle's
 * shared memory, so this module needs nothing from the stream module's .so.
 * Returns the stream zone's errors, for nginx_metric_errors_total.
 */
static uint64_t
ngx_prom_render_stream(ngx_prom_writer_t *w, ngx_pool_t *pool,
    ngx_msec_t now)
{
    size_t                     bytes, size, off, len;
    u_char                    *snap, *labels, *tail;
    uint64_t                   errors;
    ngx_int_t                  c, g, h;
    ngx_uint_t                 i, m;
    ngx_atomic_uint_t          active;
    ngx_queue_t               *q;
    ngx_list_part_t           *part;
    ngx_shm_zone_t            *zone;
    ngx_prom_stream_sh_t      *sh;
    ngx_prom_stream_ctx_t     *ctx;
    ngx_prom_stream_conf_t    *sc;
    ngx_prom_stream_series_t  *s;
    ngx_prom_stream_metric_t  *md;

    ctx = NULL;
    part = (ngx_list_part_t *) &ngx_cycle->shared_memory.part;
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

        if (zone[i].shm.name.len == sizeof(NGX_PROM_STREAM_ZONE_NAME) - 1
            && ngx_strncmp(zone[i].shm.name.data, NGX_PROM_STREAM_ZONE_NAME,
                           zone[i].shm.name.len) == 0)
        {
            ctx = zone[i].data;
            break;
        }
    }

    if (ctx == NULL || ctx->magic != NGX_PROM_STREAM_MAGIC || ctx->sh == NULL)
    {
        return 0;
    }

    sc = ctx->conf;

    ngx_shmtx_lock(&ctx->shpool->mutex);
    bytes = ctx->sh->bytes;
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    bytes += bytes / 8 + 16 * ngx_prom_stream_size(256, sc->tail);

    snap = ngx_palloc(pool, bytes + sizeof(uint64_t));
    if (snap == NULL) {
        w->error = 1;
        return 0;
    }

    snap = ngx_align_ptr(snap, sizeof(uint64_t));
    off = 0;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;

    for (q = ngx_queue_head(&sh->queue);
         q != ngx_queue_sentinel(&sh->queue);
         q = ngx_queue_next(q))
    {
        s = ngx_queue_data(q, ngx_prom_stream_series_t, queue);
        size = ngx_prom_stream_size(s->sn.str.len, sc->tail);

        if (off + size > bytes) {
            break;
        }

        ngx_memcpy(snap + off, s, size);
        off += size;
    }

    errors = sh->errors;
    active = sh->active;

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    bytes = off;

    for (m = 0; m < NGX_PROM_S_COUNT; m++) {

        if (!sc->metric[m]) {
            continue;
        }

        md = &ngx_prom_stream_metrics[m];

        if (m == NGX_PROM_S_ACTIVE) {
            ngx_prom_header_raw(w, md->metric, md->help, md->type);
            ngx_prom_line(w, md->metric, "", NULL, 0, "state=\"active\"",
                          (double) active);
            continue;
        }

        if (bytes == 0) {
            continue;
        }

        ngx_prom_header_raw(w, md->metric, md->help, md->type);

        c = md->counter;
        g = md->gauge;
        h = md->hist;

        for (off = 0; off < bytes; off += size) {
            s = (ngx_prom_stream_series_t *) (snap + off);
            size = ngx_prom_stream_size(s->sn.str.len, sc->tail);
            labels = (u_char *) s->data;
            len = s->sn.str.len;
            tail = labels + ngx_align(len, sizeof(uint64_t));

            if (c >= 0) {
                ngx_prom_line(w, md->metric, "", labels, len, NULL,
                              (double) s->counter[c]);

            } else if (g >= 0) {
                if (g == NGX_PROM_SG_CONNECT
                    ? (s->up_touched == 0
                       || now - s->up_touched > sc->gauge_expiry)
                    : now - s->touched > sc->gauge_expiry)
                {
                    continue;
                }

                ngx_prom_line(w, md->metric, "", labels, len, NULL,
                              s->gauge[g]);

            } else if (h >= 0) {
                if (h != NGX_PROM_SH_SESSION && s->up_touched == 0) {
                    continue;
                }

                ngx_prom_histogram(w, md->metric, labels, len, tail,
                                   &sc->hist[h]);
            }
        }
    }

    return errors;
}


/*
 * The exposition of everything in the zone, as a chain allocated from pool.
 * Used by the `prometheus;` location and by the built-in :9145 listener.
 */
ngx_int_t
ngx_prom_render(ngx_http_prometheus_main_conf_t *pmcf, ngx_pool_t *pool,
    ngx_chain_t **out, off_t *length)
{
    size_t                            bytes, size, off, len;
    u_char                           *snap, *tail, *header, *labels;
    ngx_int_t                         g;
    ngx_uint_t                        m;
    ngx_msec_t                        now, expiry;
    ngx_chain_t                      *cl;
    ngx_queue_t                      *q;
    ngx_prom_sh_t                    *sh, hcopy;
    ngx_prom_series_t                *s;
    ngx_prom_writer_t                 w;
    ngx_prom_zone_ctx_t              *ctx;
    ngx_prom_metric_def_t            *md;
    ngx_core_conf_t                  *ccf;
    uint64_t                          stream_errors;

    static ngx_prom_metric_def_t  errors = {
        NULL, "nginx_metric_errors_total",
        "Requests and sessions not recorded because a prometheus zone was full",
        "counter", 1, -1, -1
    };

    ctx = pmcf->zone->data;
    now = ngx_current_msec;
    expiry = pmcf->gauge_expiry;

    /*
     * Snapshot: the series are copied out as they are while the mutex is
     * held, so recording waits only for memcpy, never for formatting. The
     * copy is sized outside the lock, with headroom for series added between
     * the two locks; any beyond it show up on the next scrape.
     */

    /* both copies hold uint64_t and double: align them for 32-bit too */

    header = ngx_palloc(pool, NGX_PROM_HEADER_TAIL_MAX + sizeof(uint64_t));
    if (header == NULL) {
        return NGX_ERROR;
    }

    header = ngx_align_ptr(header, sizeof(uint64_t));

    ngx_shmtx_lock(&ctx->shpool->mutex);
    bytes = ctx->sh->bytes;
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    bytes += bytes / 8 + 16 * ngx_prom_series_size(256, pmcf->series_tail);

    snap = ngx_palloc(pool, bytes + sizeof(uint64_t));
    if (snap == NULL) {
        return NGX_ERROR;
    }

    snap = ngx_align_ptr(snap, sizeof(uint64_t));

    off = 0;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    sh = ctx->sh;

    for (q = ngx_queue_head(&sh->queue);
         q != ngx_queue_sentinel(&sh->queue);
         q = ngx_queue_next(q))
    {
        s = ngx_queue_data(q, ngx_prom_series_t, queue);
        size = ngx_prom_series_size(s->sn.str.len, pmcf->series_tail);

        if (off + size > bytes) {
            break;
        }

        ngx_memcpy(snap + off, s, size);
        off += size;
    }

    hcopy = *sh;
    ngx_memcpy(header, ngx_prom_data(sh), pmcf->header_tail);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    bytes = off;

    /* format */

    ngx_memzero(&w, sizeof(ngx_prom_writer_t));
    w.pool = pool;
    w.last = &w.out;

    for (m = 0; m < NGX_PROM_M_COUNT; m++) {

        if (!pmcf->metric[m]) {
            continue;
        }

        md = &ngx_prom_metrics[m];

        switch (m) {

        case NGX_PROM_M_CONNECTIONS:
#if (NGX_STAT_STUB)
            ngx_prom_header(&w, md);
            ngx_prom_line(&w, md->metric, "", NULL, 0, "state=\"active\"",
                          (double) *ngx_stat_active);
            ngx_prom_line(&w, md->metric, "", NULL, 0, "state=\"reading\"",
                          (double) *ngx_stat_reading);
            ngx_prom_line(&w, md->metric, "", NULL, 0, "state=\"writing\"",
                          (double) *ngx_stat_writing);
            ngx_prom_line(&w, md->metric, "", NULL, 0, "state=\"waiting\"",
                          (double) *ngx_stat_waiting);
#endif
            continue;

        case NGX_PROM_M_CONN_REQUESTS:
        case NGX_PROM_M_CONN_TIME:
            if (hcopy.conn_touched == 0 || now - hcopy.conn_touched > expiry) {
                continue;
            }

            ngx_prom_header(&w, md);
            ngx_prom_line(&w, md->metric, "", NULL, 0, NULL,
                          m == NGX_PROM_M_CONN_REQUESTS
                          ? hcopy.conn_requests : hcopy.conn_time);
            continue;

        case NGX_PROM_M_CONN_REQUESTS_HIST:
        case NGX_PROM_M_CONN_TIME_HIST:
            ngx_prom_header(&w, md);
            ngx_prom_histogram(&w, md->metric, NULL, 0, header,
                               &pmcf->hist[md->hist]);
            continue;

        case NGX_PROM_M_SLOTS_LIMIT:
            ccf = (ngx_core_conf_t *) ngx_get_conf(ngx_cycle->conf_ctx,
                                                   ngx_core_module);
            ngx_prom_header(&w, md);
            ngx_prom_line(&w, md->metric, "", NULL, 0, NULL,
                          (double) ((ngx_uint_t) ccf->worker_processes
                                    * ngx_cycle->connection_n));
            continue;
        }

        if (bytes == 0) {
            continue;
        }

        ngx_prom_header(&w, md);

        for (off = 0; off < bytes; off += size) {
            s = (ngx_prom_series_t *) (snap + off);
            size = ngx_prom_series_size(s->sn.str.len, pmcf->series_tail);
            labels = ngx_prom_data(s);
            len = s->sn.str.len;
            tail = labels + ngx_align(len, sizeof(uint64_t));

            if (m == NGX_PROM_M_REQUESTS) {
                ngx_prom_line(&w, md->metric, "", labels, len, NULL,
                              (double) s->requests);
                continue;
            }

            g = md->gauge;

            if (g >= 0) {
                if (g >= NGX_PROM_G_UP_RESPONSE) {
                    if (s->up_touched == 0 || now - s->up_touched > expiry) {
                        continue;
                    }

                } else if (now - s->touched > expiry) {
                    continue;
                }

                ngx_prom_line(&w, md->metric, "", labels, len, NULL,
                              s->gauge[g]);
                continue;
            }

            if (md->hist >= 0) {
                if (md->hist == NGX_PROM_H_UP_RESPONSE && s->up_touched == 0) {
                    continue;
                }

                ngx_prom_histogram(&w, md->metric, labels, len, tail,
                                   &pmcf->hist[md->hist]);
            }
        }
    }

    stream_errors = ngx_prom_render_stream(&w, pool, now);

    ngx_prom_header(&w, &errors);
    ngx_prom_line(&w, errors.metric, "", NULL, 0, NULL,
                  (double) (hcopy.errors + stream_errors));

    if (w.error) {
        return NGX_ERROR;
    }

    *length = 0;

    for (cl = w.out; cl; cl = cl->next) {
        *length += cl->buf->last - cl->buf->pos;

        if (cl->next == NULL) {
            cl->buf->last_in_chain = 1;
        }
    }

    *out = w.out;

    return NGX_OK;
}


static ngx_int_t
ngx_http_prometheus_handler(ngx_http_request_t *r)
{
    off_t                             length;
    ngx_int_t                         rc;
    ngx_chain_t                      *out, *cl;
    ngx_http_prometheus_main_conf_t  *pmcf;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_prometheus_module);

    if (pmcf->zone == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "\"prometheus\" with \"prometheus_zone off\"");
        return NGX_HTTP_NOT_FOUND;
    }

    if (ngx_prom_render(pmcf, r->pool, &out, &length) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    for (cl = out; cl->next; cl = cl->next) { /* void */ }
    cl->buf->last_buf = (r == r->main) ? 1 : 0;

    r->headers_out.content_length_n = length;
    r->headers_out.status = NGX_HTTP_OK;
    ngx_str_set(&r->headers_out.content_type,
                "text/plain; version=0.0.4; charset=utf-8");
    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    r->headers_out.content_type_lowcase = NULL;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, out);
}
