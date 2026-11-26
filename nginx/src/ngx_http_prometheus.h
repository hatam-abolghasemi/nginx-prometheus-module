/*
 * Shared between the files of ngx_http_prometheus_module: the configuration,
 * the shared memory layout, and what the recording and identity code call in
 * each other.
 */

#ifndef NGX_HTTP_PROMETHEUS_H
#define NGX_HTTP_PROMETHEUS_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_prometheus_label.h"
#include "ngx_http_prometheus_identity.h"


#define NGX_PROM_MAX_BUCKETS     32
#define NGX_PROM_LABELS_MAX      2048
#define NGX_PROM_ROUTE_MAX       1024
#define NGX_PROM_ZONE_NAME       "prometheus"


/* request labels, in label order (the same order as the OpenResty sidecar) */
enum {
    NGX_PROM_L_METHOD = 0,
    NGX_PROM_L_ROUTE,
    NGX_PROM_L_STATUS,
    NGX_PROM_L_CLIENT_SUBNET,
    NGX_PROM_L_HOST,
    NGX_PROM_L_APP,
    NGX_PROM_L_NAMESPACE,
    NGX_PROM_L_MODE,
    NGX_PROM_L_SOURCE_SERVICE,
    NGX_PROM_L_SOURCE_NAMESPACE,
    NGX_PROM_L_PROTOCOL,
    NGX_PROM_L_COUNT
};


/* metrics; gauges and histograms per label set, then connection-level ones */
enum {
    NGX_PROM_M_REQUESTS = 0,
    NGX_PROM_M_DURATION,
    NGX_PROM_M_DURATION_HIST,
    NGX_PROM_M_REQ_SIZE,
    NGX_PROM_M_REQ_SIZE_HIST,
    NGX_PROM_M_RESP_SIZE,
    NGX_PROM_M_RESP_SIZE_HIST,
    NGX_PROM_M_UP_RESPONSE,
    NGX_PROM_M_UP_RESPONSE_HIST,
    NGX_PROM_M_UP_CONNECT,
    NGX_PROM_M_UP_HEADER,
    NGX_PROM_M_CONNECTIONS,
    NGX_PROM_M_CONN_REQUESTS,
    NGX_PROM_M_CONN_REQUESTS_HIST,
    NGX_PROM_M_CONN_TIME,
    NGX_PROM_M_CONN_TIME_HIST,
    NGX_PROM_M_SLOTS_LIMIT,
    NGX_PROM_M_COUNT
};


/* histograms, each with its own buckets */
enum {
    NGX_PROM_H_DURATION = 0,
    NGX_PROM_H_REQ_SIZE,
    NGX_PROM_H_RESP_SIZE,
    NGX_PROM_H_UP_RESPONSE,
    NGX_PROM_H_CONN_REQUESTS,
    NGX_PROM_H_CONN_TIME,
    NGX_PROM_H_COUNT
};


/* per label set gauges */
enum {
    NGX_PROM_G_DURATION = 0,
    NGX_PROM_G_REQ_SIZE,
    NGX_PROM_G_RESP_SIZE,
    NGX_PROM_G_UP_RESPONSE,
    NGX_PROM_G_UP_CONNECT,
    NGX_PROM_G_UP_HEADER,
    NGX_PROM_G_COUNT
};


/* an address range: addr/mask in host order */
typedef struct {
    uint32_t          addr;
    uint32_t          mask;
} ngx_prom_cidr_t;


/* prometheus_source_static: a range and the labels its callers get */
typedef struct {
    ngx_prom_cidr_t   cidr;
    ngx_prom_name_t   service;
    ngx_prom_name_t   namespace;
} ngx_prom_static_range_t;


/* caller attribution and own identity (ngx_http_prometheus_source.c) */
typedef struct {
    ngx_flag_t        enabled;          /* any source_* label on */
    ngx_flag_t        identity;         /* prometheus_identity */

    ngx_array_t      *ptr_ranges;       /* ngx_prom_cidr_t: resolved by PTR */
    ngx_array_t      *static_ranges;    /* ngx_prom_static_range_t */
    ngx_prom_name_t   default_service;
    ngx_prom_name_t   default_namespace;
    ngx_prom_name_t   unresolved_service;
    ngx_prom_name_t   unresolved_namespace;
    ngx_str_t         probe_prefix;     /* User-Agent; empty = off */
    ngx_str_t         waypoint;         /* empty = off */
    ngx_array_t      *ignore;           /* ngx_regex_t * */
    ngx_str_t         cluster_domain;

    ngx_str_t         dns;              /* "auto" or host[:port] */
    ngx_addr_t        dns_addr;
    ngx_msec_t        dns_timeout;
    ngx_msec_t        refresh;
    ngx_msec_t        forget;

    ngx_prom_name_t   fallback_app;
    ngx_prom_name_t   fallback_namespace;
} ngx_prom_source_conf_t;


/* a known caller IP, in the zone */
typedef struct {
    ngx_rbtree_node_t  node;            /* key: the IPv4, host order */
    ngx_queue_t        queue;
    ngx_prom_name_t    service;
    ngx_prom_name_t    namespace;
    ngx_msec_t         seen;            /* last request from it */
    ngx_msec_t         inflight;        /* a lookup runs until then */
    unsigned           answered:1;
} ngx_prom_caller_t;


typedef struct {
    ngx_rbtree_t       rbtree;
    ngx_rbtree_node_t  sentinel;
    ngx_queue_t        queue;
    ngx_uint_t         nseries;
    size_t             bytes;       /* sum of series record sizes */
    uint32_t           layout;      /* fingerprint of the series layout */
    uint64_t           errors;      /* nginx_metric_errors_total */
    double             conn_requests;
    double             conn_time;
    ngx_msec_t         conn_touched;

    /* callers by IP */
    ngx_rbtree_t       callers;
    ngx_rbtree_node_t  callers_sentinel;
    ngx_queue_t        callers_queue;
    ngx_uint_t         ncallers;

    /* own identity, once discovered */
    ngx_uint_t         self_resolved;
    uint32_t           self_ip;         /* 0 until read from /etc/hosts */
    u_char             self_ip_text[16];
    size_t             self_ip_len;
    ngx_prom_name_t    self_service;
    ngx_prom_name_t    self_namespace;

    /* connection-level histograms: uint64_t only for the alignment */
    uint64_t           data[1];
} ngx_prom_sh_t;


typedef struct {
    ngx_prom_sh_t     *sh;
    ngx_slab_pool_t   *shpool;
    uint32_t           layout;      /* of the config this zone belongs to */
} ngx_prom_zone_ctx_t;


typedef struct {
    ngx_uint_t         n;
    double             le[NGX_PROM_MAX_BUCKETS];
    size_t             offset;      /* into a series' (or the header's) tail */
} ngx_prom_hist_conf_t;


typedef struct ngx_http_prometheus_main_conf_s  ngx_http_prometheus_main_conf_t;

struct ngx_http_prometheus_main_conf_s {
    ngx_shm_zone_t          *zone;
    ngx_flag_t               zone_off;      /* prometheus_zone off */
    ngx_flag_t               has_location;  /* a `prometheus;` exists */
    ngx_str_t                listen;        /* prometheus_listen */
    ngx_flag_t               metric[NGX_PROM_M_COUNT];
    ngx_flag_t               label[NGX_PROM_L_COUNT];
    ngx_prom_hist_conf_t     hist[NGX_PROM_H_COUNT];
    ngx_msec_t               gauge_expiry;
    ngx_prom_route_conf_t    route;
    ngx_uint_t               route_min_len;
    ngx_uint_t               route_max_segment_len;
    ngx_str_t                cluster_ip_prefix;
    size_t                   series_tail;   /* histogram bytes per series */
    size_t                   header_tail;   /* histogram bytes in the header */
    uint32_t                 layout;
    ngx_prom_source_conf_t   source;
};


typedef struct {
    ngx_flag_t               record;
    ngx_str_t                mode;
    ngx_flag_t               handler;
} ngx_http_prometheus_loc_conf_t;


/* who a request came from, worked out before it is recorded */
typedef struct {
    ngx_prom_name_t          service;
    ngx_prom_name_t          namespace;
    unsigned                 done:1;
} ngx_prom_request_ctx_t;


extern ngx_module_t  ngx_http_prometheus_module;


/* ngx_http_prometheus_module.c */

ngx_int_t ngx_prom_render(ngx_http_prometheus_main_conf_t *pmcf,
    ngx_pool_t *pool, ngx_chain_t **out, off_t *length);


/* ngx_http_prometheus_listen.c */

ngx_int_t ngx_prom_listen_add(ngx_conf_t *cf,
    ngx_http_prometheus_main_conf_t *pmcf);


/* ngx_http_prometheus_source.c */

char *ngx_prom_source_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
void ngx_prom_source_create_conf(ngx_prom_source_conf_t *conf);
char *ngx_prom_source_init_conf(ngx_conf_t *cf,
    ngx_http_prometheus_main_conf_t *pmcf);
void ngx_prom_source_init_zone(ngx_prom_sh_t *sh);
ngx_int_t ngx_prom_source_init_process(ngx_cycle_t *cycle);
void ngx_prom_source_exit_process(ngx_cycle_t *cycle);
ngx_int_t ngx_prom_source_post_read(ngx_http_request_t *r);
void ngx_prom_source_caller(ngx_http_request_t *r, ngx_str_t *mode,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace);
void ngx_prom_source_self(ngx_http_prometheus_main_conf_t *pmcf,
    ngx_prom_name_t *app, ngx_prom_name_t *namespace);
ngx_flag_t ngx_prom_source_own_host(ngx_http_prometheus_main_conf_t *pmcf,
    ngx_str_t *host, ngx_prom_name_t *service);

#endif /* NGX_HTTP_PROMETHEUS_H */
