/*
 * Label value shaping for ngx_http_prometheus_module: route sanitizing and
 * Host classification. Plain C with no nginx dependency, so tests/ can build
 * and run it on its own (tests/label_test.c).
 */

#ifndef NGX_HTTP_PROMETHEUS_LABEL_H
#define NGX_HTTP_PROMETHEUS_LABEL_H

#include <stddef.h>

#define NGX_PROM_RULE_PURE_NUMERIC       0x01  /* /users/123                 */
#define NGX_PROM_RULE_UUID               0x02  /* /users/550e8400-e29b-...   */
#define NGX_PROM_RULE_HEX                0x04  /* /obj/a1b2c3d4e5f6ab        */
#define NGX_PROM_RULE_SLUG_SUFFIX_DIGIT  0x08  /* /zone-7 /order-12345       */
#define NGX_PROM_RULE_LONG_WITH_DIGIT    0x10  /* /x/abc123xyz               */
#define NGX_PROM_RULE_FILE_EXTENSION     0x20  /* /report.pdf /file.js       */
#define NGX_PROM_RULE_ALL                0x3f

typedef struct {
    unsigned  rules;            /* NGX_PROM_RULE_* bits */
    size_t    min_len;          /* long_with_digit: segment longer than this */
    size_t    max_segment_len;  /* anything longer is always $param */
} ngx_prom_route_conf_t;

/*
 * Writes the sanitized route of `path` (a raw request URI; the query string,
 * if any, is dropped) into `out`, at most `cap` bytes, and returns its length.
 * Same result as lib/route_sanitizer.lua: segments matching a rule become
 * "$param", adjacent "$param"s collapse into one, empty segments go away.
 */
size_t ngx_prom_route_sanitize(const unsigned char *path, size_t len,
    const ngx_prom_route_conf_t *conf, unsigned char *out, size_t cap);

/*
 * Classifies a Host value (port already stripped or not) into a bounded set:
 * "localhost", "kubernetes" (an IPv4 under cluster_prefix), "$ip" (any other
 * IP literal), or the name itself. Returns a pointer and length into either a
 * static string or `host`.
 */
const unsigned char *ngx_prom_host_classify(const unsigned char *host,
    size_t len, const unsigned char *cluster_prefix, size_t prefix_len,
    size_t *out_len);

/*
 * Copies `len` bytes of `in` into `out` (at most `cap`) as a Prometheus label
 * value: \ " and newline escaped, other control bytes and invalid UTF-8
 * replaced with '_'. Returns the bytes written; never splits an escape.
 */
size_t ngx_prom_label_escape(const unsigned char *in, size_t len,
    unsigned char *out, size_t cap);

#endif /* NGX_HTTP_PROMETHEUS_LABEL_H */
