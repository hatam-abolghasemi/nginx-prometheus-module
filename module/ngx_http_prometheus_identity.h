/*
 * Caller and own identity for ngx_http_prometheus_module: the DNS PTR wire
 * format, Kubernetes PTR names, XFCC, /etc/hosts and client subnets. Plain C
 * with no nginx dependency, so tests/identity_test.c runs it on its own.
 */

#ifndef NGX_HTTP_PROMETHEUS_IDENTITY_H
#define NGX_HTTP_PROMETHEUS_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#define NGX_PROM_NAME_MAX      64     /* a service or namespace label value */
#define NGX_PROM_DNS_NAME_MAX  256
#define NGX_PROM_DNS_ANSWERS   16

typedef struct {
    unsigned char  data[NGX_PROM_NAME_MAX];
    size_t         len;
} ngx_prom_name_t;


/* the DNS side of one PTR lookup */
typedef struct {
    uint16_t       id;
    int            rcode;          /* 0 ok, 3 NXDOMAIN, others: an error */
    uint32_t       ip;             /* from the question, host order */
    size_t         n;              /* PTR answers */
    unsigned char  name[NGX_PROM_DNS_ANSWERS][NGX_PROM_DNS_NAME_MAX];
    size_t         name_len[NGX_PROM_DNS_ANSWERS];
} ngx_prom_dns_answer_t;


/*
 * Writes a PTR query for `ip` (host order) with `id` into buf; returns its
 * length, 0 if cap is too small.
 */
size_t ngx_prom_dns_ptr_query(uint32_t ip, uint16_t id, unsigned char *buf,
    size_t cap);

/*
 * Parses a DNS response to a PTR query. Returns 0 on success (answer filled,
 * rcode may still be non-zero), -1 if the packet is not a usable response
 * to a PTR query for an IPv4 address.
 */
int ngx_prom_dns_parse(const unsigned char *pkt, size_t len,
    ngx_prom_dns_answer_t *answer);

/*
 * A PTR name in Kubernetes cluster DNS:
 *   <pod-ip>.<service>.<namespace>.svc.<domain>  -> returns 2
 *   <pod-ip>.<namespace>.pod.<domain>             -> returns 1 (no service)
 *   anything else                                 -> returns 0
 * A trailing dot is allowed. `domain` is e.g. "cluster.local".
 */
int ngx_prom_ptr_parse(const unsigned char *name, size_t len,
    const unsigned char *domain, size_t domain_len, ngx_prom_name_t *service,
    ngx_prom_name_t *namespace);

/*
 * Picks one identity out of a PTR answer set, the way
 * lib/source_attribution.lua's pick_ptr does: Services the `ignore` callback
 * rejects are passed over unless nothing else is left, ties go to the
 * alphabetically first service (then namespace). Returns 2 (service and
 * namespace), 1 (only a pod namespace) or 0 (nothing usable).
 */
int ngx_prom_ptr_pick(const ngx_prom_dns_answer_t *answer,
    const unsigned char *domain, size_t domain_len,
    int (*ignore)(void *data, const ngx_prom_name_t *service), void *data,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace);

/*
 * The SPIFFE ID in an X-Forwarded-Client-Cert value:
 * URI=spiffe://<trust-domain>/ns/<namespace>/sa/<service-account>.
 * Returns 1 and fills both, or 0.
 */
int ngx_prom_xfcc_parse(const unsigned char *value, size_t len,
    ngx_prom_name_t *namespace, ngx_prom_name_t *service_account);

/*
 * The IPv4 that /etc/hosts maps to `hostname` (host order), skipping 127.x
 * and comments. Returns 1 and sets *ip, or 0.
 */
int ngx_prom_hosts_find(const unsigned char *content, size_t len,
    const unsigned char *hostname, size_t hostname_len, uint32_t *ip);

/*
 * An IPv4 address (host order) masked for the client_subnet label: /24 for
 * 10/8, 192/8 and 172.16/12, /16 for anything else. Writes "a.b.c.0" into
 * out (at least 16 bytes) and returns its length.
 */
size_t ngx_prom_client_subnet(uint32_t ip, unsigned char *out);

/*
 * Parses a dotted IPv4 (host order). Returns 1 and sets *ip, or 0.
 */
int ngx_prom_parse_ipv4(const unsigned char *s, size_t len, uint32_t *ip);

#endif /* NGX_HTTP_PROMETHEUS_IDENTITY_H */
