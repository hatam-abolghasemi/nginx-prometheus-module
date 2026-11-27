/*
 * What the http and the stream module share: histogram layout, and the
 * stream module's zone, which the http module reads to serve stream metrics
 * on the same /metrics. Core types only, so both modules can include it.
 *
 * The two are separate .so files (ngx_http_prometheus_module.so,
 * ngx_stream_prometheus_module.so), so an nginx without stream can load the
 * http one. They never call each other: the http module finds the stream
 * zone by name in the cycle's shared memory, and the zone carries a pointer
 * to the stream configuration it needs to render it. The other way round,
 * the stream module finds the http zone by name to ask who a caller is.
 */

#ifndef NGX_PROMETHEUS_SHARED_H
#define NGX_PROMETHEUS_SHARED_H

#include <ngx_config.h>
#include <ngx_core.h>

#include "ngx_http_prometheus_identity.h"


#define NGX_PROM_MAX_BUCKETS       32
#define NGX_PROM_STREAM_ZONE_NAME  "prometheus_stream"
#define NGX_PROM_STREAM_MAGIC      0x70726f6d      /* "prom" */


typedef struct {
    ngx_uint_t         n;
    double             le[NGX_PROM_MAX_BUCKETS];
    size_t             offset;      /* into a series' (or the header's) tail */
} ngx_prom_hist_conf_t;


/* stream metrics, in exposition order */
enum {
    NGX_PROM_S_CONNECTIONS = 0,
    NGX_PROM_S_BYTES_SENT,
    NGX_PROM_S_BYTES_RECEIVED,
    NGX_PROM_S_SESSION_HIST,
    NGX_PROM_S_SESSION,
    NGX_PROM_S_CONNECT,
    NGX_PROM_S_CONNECT_HIST,
    NGX_PROM_S_ACTIVE,
    NGX_PROM_S_UP_BYTES_SENT,
    NGX_PROM_S_UP_BYTES_RECEIVED,
    NGX_PROM_S_FIRST_BYTE_HIST,
    NGX_PROM_S_COUNT
};

/* per label set counters, gauges, histograms */
enum {
    NGX_PROM_SC_CONNECTIONS = 0,
    NGX_PROM_SC_BYTES_SENT,
    NGX_PROM_SC_BYTES_RECEIVED,
    NGX_PROM_SC_UP_BYTES_SENT,
    NGX_PROM_SC_UP_BYTES_RECEIVED,
    NGX_PROM_SC_COUNT
};

enum {
    NGX_PROM_SG_SESSION = 0,
    NGX_PROM_SG_CONNECT,
    NGX_PROM_SG_COUNT
};

enum {
    NGX_PROM_SH_SESSION = 0,
    NGX_PROM_SH_CONNECT,
    NGX_PROM_SH_FIRST_BYTE,
    NGX_PROM_SH_COUNT
};

/* stream labels, in label order (the Lua module's) */
enum {
    NGX_PROM_SL_DESTINATION = 0,
    NGX_PROM_SL_UPSTREAM_ADDR,
    NGX_PROM_SL_STATUS,
    NGX_PROM_SL_SOURCE_SERVICE,
    NGX_PROM_SL_SOURCE_NAMESPACE,
    NGX_PROM_SL_COUNT
};


typedef struct {
    const char        *name;        /* prometheus_metric name */
    const char        *metric;      /* exported name */
    const char        *help;
    const char        *type;
    ngx_flag_t         on;          /* default */
    ngx_int_t          counter;     /* NGX_PROM_SC_*, or -1 */
    ngx_int_t          gauge;       /* NGX_PROM_SG_*, or -1 */
    ngx_int_t          hist;        /* NGX_PROM_SH_*, or -1 */
} ngx_prom_stream_metric_t;


/* the Lua module's names, defaults and help texts; each module that needs
 * them defines its own copy: ngx_prom_stream_metrics[] = NGX_PROM_STREAM_METRICS */
#define NGX_PROM_STREAM_METRICS { \
    { "connections", "nginx_stream_connections_total", \
      "Total TCP/UDP connections proxied", "counter", 1, \
      NGX_PROM_SC_CONNECTIONS, -1, -1 }, \
    { "bytes_sent", "nginx_stream_bytes_sent_total", \
      "Bytes sent to client", "counter", 1, \
      NGX_PROM_SC_BYTES_SENT, -1, -1 }, \
    { "bytes_received", "nginx_stream_bytes_received_total", \
      "Bytes received from client", "counter", 1, \
      NGX_PROM_SC_BYTES_RECEIVED, -1, -1 }, \
    { "session_duration_histogram", \
      "nginx_stream_session_duration_seconds_histogram", \
      "Duration of proxied TCP/UDP session", "histogram", 0, \
      -1, -1, NGX_PROM_SH_SESSION }, \
    { "session_duration", "nginx_stream_session_duration_seconds", \
      "Duration of the most recently completed session (last value)", \
      "gauge", 1, -1, NGX_PROM_SG_SESSION, -1 }, \
    { "upstream_connect_time", "nginx_stream_upstream_connect_time_seconds", \
      "Time to establish the last upstream connection", "gauge", 1, \
      -1, NGX_PROM_SG_CONNECT, -1 }, \
    { "upstream_connect_time_histogram", \
      "nginx_stream_upstream_connect_time_seconds_histogram", \
      "Upstream connect time histogram", "histogram", 0, \
      -1, -1, NGX_PROM_SH_CONNECT }, \
    { "connections_active", "nginx_stream_connections_active", \
      "Currently open proxied stream connections", "gauge", 1, -1, -1, -1 }, \
    { "upstream_bytes_sent", "nginx_stream_upstream_bytes_sent_total", \
      "Bytes sent to the upstream (destination)", "counter", 1, \
      NGX_PROM_SC_UP_BYTES_SENT, -1, -1 }, \
    { "upstream_bytes_received", "nginx_stream_upstream_bytes_received_total", \
      "Bytes received from the upstream (destination)", "counter", 1, \
      NGX_PROM_SC_UP_BYTES_RECEIVED, -1, -1 }, \
    { "upstream_first_byte_time_histogram", \
      "nginx_stream_upstream_first_byte_time_seconds", \
      "Time to first byte from upstream", "histogram", 0, \
      -1, -1, NGX_PROM_SH_FIRST_BYTE }, \
}


/* what rendering the stream zone needs from the stream configuration */
typedef struct {
    ngx_flag_t             metric[NGX_PROM_S_COUNT];
    ngx_flag_t             label[NGX_PROM_SL_COUNT];
    ngx_prom_hist_conf_t   hist[NGX_PROM_SH_COUNT];
    ngx_msec_t             gauge_expiry;
    size_t                 tail;        /* histogram bytes per series */
    uint32_t               layout;
} ngx_prom_stream_conf_t;


typedef struct {
    ngx_str_node_t     sn;              /* key: the rendered label set */
    ngx_queue_t        queue;
    uint64_t           counter[NGX_PROM_SC_COUNT];
    double             gauge[NGX_PROM_SG_COUNT];
    ngx_msec_t         touched;         /* last session of this label set */
    ngx_msec_t         up_touched;      /* last session that connected */
    /* label text, then histograms: uint64_t only for the alignment */
    uint64_t           data[1];
} ngx_prom_stream_series_t;


/*
 * Sessions open now from one caller, when the source labels are on: the key
 * is the rendered source_service / source_namespace labels.
 */
typedef struct {
    ngx_str_node_t     sn;
    ngx_queue_t        queue;
    int64_t            count;
    u_char             data[1];
} ngx_prom_stream_active_t;


typedef struct {
    ngx_rbtree_t       rbtree;
    ngx_rbtree_node_t  sentinel;
    ngx_queue_t        queue;
    ngx_uint_t         nseries;
    size_t             bytes;
    uint32_t           layout;
    uint64_t           errors;
    ngx_atomic_t       active;          /* sessions open now */

    /* sessions open now, by caller (source labels on) */
    ngx_rbtree_t       active_rbtree;
    ngx_rbtree_node_t  active_sentinel;
    ngx_queue_t        active_queue;
    size_t             active_bytes;
    ngx_uint_t         generation;      /* bumped when the series are reset */
} ngx_prom_stream_sh_t;


typedef struct {
    uint32_t                 magic;
    ngx_prom_stream_sh_t    *sh;
    ngx_slab_pool_t         *shpool;
    ngx_prom_stream_conf_t  *conf;
} ngx_prom_stream_ctx_t;


#define ngx_prom_stream_active_size(len)                                      \
    (offsetof(ngx_prom_stream_active_t, data) + (len))


/*
 * Caller identity, which only the http module has (its PTR cache and DNS
 * client): the first member of the http zone's ctx, so the stream module can
 * find it by the zone's name (NGX_PROM_HTTP_ZONE_NAME) and ask through it.
 *
 * source() fills the caller of a connection from sockaddr: a configured
 * static or PTR range, then loopback as this pod's own identity, then the
 * default. A PTR caller with no answer yet gets the unresolved labels, and
 * a lookup is started.
 */
#define NGX_PROM_IDENTITY_MAGIC    0x70726964      /* "prid" */
#define NGX_PROM_HTTP_ZONE_NAME    "prometheus"

typedef void (*ngx_prom_source_pt)(void *data, struct sockaddr *sa,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace);

typedef struct {
    uint32_t             magic;
    void                *data;
    ngx_prom_source_pt   source;
} ngx_prom_identity_t;


#define ngx_prom_stream_size(len, tail)                                       \
    (offsetof(ngx_prom_stream_series_t, data)                                 \
     + ngx_align(len, sizeof(uint64_t)) + (tail))

#endif /* NGX_PROMETHEUS_SHARED_H */
