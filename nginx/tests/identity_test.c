/*
 * Unit tests for src/ngx_http_prometheus_identity.c: the PTR wire format,
 * Kubernetes PTR names, the pick among several, XFCC, /etc/hosts and client
 * subnets. No nginx needed:
 *
 *   cc -Wall -Werror -Isrc -o identity_test tests/identity_test.c \
 *       src/ngx_http_prometheus_identity.c && ./identity_test
 */

#include <stdio.h>
#include <string.h>
#include "ngx_http_prometheus_identity.h"

static int  failed, passed;

#define D  ((const unsigned char *) "cluster.local")
#define DL (sizeof("cluster.local") - 1)


static void
ok(const char *name, int cond)
{
    if (cond) {
        passed++;
        return;
    }

    failed++;
    printf("FAIL %s\n", name);
}


static int
is(const ngx_prom_name_t *n, const char *want)
{
    return n->len == strlen(want) && memcmp(n->data, want, n->len) == 0;
}


/* a response: the query, then one PTR answer per name (pointer to qname) */
static size_t
response(const unsigned char *query, size_t qlen, int rcode,
    const char **names, int n, unsigned char *out)
{
    size_t  o;
    int     i;

    memcpy(out, query, qlen);
    out[2] |= 0x80;
    out[3] = (unsigned char) rcode;
    out[6] = 0;
    out[7] = (unsigned char) n;
    o = qlen;

    for (i = 0; i < n; i++) {
        const char  *s = names[i], *dot;
        size_t       rd_at;

        out[o++] = 0xc0;
        out[o++] = 12;                     /* owner: the question name */
        out[o++] = 0; out[o++] = 12;       /* PTR */
        out[o++] = 0; out[o++] = 1;        /* IN */
        out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 5;
        rd_at = o;
        o += 2;

        while (*s) {
            dot = strchr(s, '.');
            size_t  l = dot ? (size_t) (dot - s) : strlen(s);
            out[o++] = (unsigned char) l;
            memcpy(out + o, s, l);
            o += l;
            s += l + (dot ? 1 : 0);
        }
        out[o++] = 0;

        out[rd_at] = (unsigned char) ((o - rd_at - 2) >> 8);
        out[rd_at + 1] = (unsigned char) (o - rd_at - 2);
    }

    return o;
}


static int
ignore_metrics(void *data, const ngx_prom_name_t *svc)
{
    (void) data;
    return svc->len > 8
           && memcmp(svc->data + svc->len - 8, "-metrics", 8) == 0;
}


int
main(void)
{
    unsigned char          q[512], r[2048], sub[16];
    size_t                 qlen, rlen, n;
    uint32_t               ip;
    ngx_prom_dns_answer_t  a;
    ngx_prom_name_t        svc, ns;

    /* ipv4 */
    ok("ipv4", ngx_prom_parse_ipv4((const unsigned char *) "10.233.4.12", 11,
                                   &ip) && ip == 0x0ae9040c);
    ok("ipv4 bad", !ngx_prom_parse_ipv4((const unsigned char *) "10.233.4",
                                        8, &ip));
    ok("ipv4 > 255", !ngx_prom_parse_ipv4((const unsigned char *) "1.2.3.256",
                                          9, &ip));

    /* query and response round trip */
    qlen = ngx_prom_dns_ptr_query(0x0ae9040c, 0x1234, q, sizeof(q));
    ok("query built", qlen > 12);
    ok("query name", memcmp(q + 12, "\00212\0014\003233\00210\7in-addr\4arpa",
                            26) == 0);

    {
        const char  *names[] = {
            "10-233-4-12.checkout.shop.svc.cluster.local",
            "10-233-4-12.checkout-metrics.shop.svc.cluster.local",
            "10-233-4-12.billing.shop.svc.cluster.local",
        };

        rlen = response(q, qlen, 0, names, 3, r);
        ok("parse", ngx_prom_dns_parse(r, rlen, &a) == 0);
        ok("parse id", a.id == 0x1234);
        ok("parse ip", a.ip == 0x0ae9040c);
        ok("parse rcode", a.rcode == 0);
        ok("parse answers", a.n == 3);

        ok("pick", ngx_prom_ptr_pick(&a, D, DL, ignore_metrics, NULL,
                                     &svc, &ns) == 2
                   && is(&svc, "billing") && is(&ns, "shop"));
    }

    {
        const char  *names[] = {
            "10-233-4-12.zeta-metrics.shop.svc.cluster.local",
            "10-233-4-12.alpha-metrics.shop.svc.cluster.local",
        };

        rlen = response(q, qlen, 0, names, 2, r);
        ngx_prom_dns_parse(r, rlen, &a);
        ok("pick only ignored left", ngx_prom_ptr_pick(&a, D, DL,
                                         ignore_metrics, NULL, &svc, &ns) == 2
                                     && is(&svc, "alpha-metrics"));
    }

    {
        const char  *names[] = { "10-233-4-12.shop.pod.cluster.local" };

        rlen = response(q, qlen, 0, names, 1, r);
        ngx_prom_dns_parse(r, rlen, &a);
        ok("pick pod only", ngx_prom_ptr_pick(&a, D, DL, NULL, NULL,
                                              &svc, &ns) == 1
                            && is(&ns, "shop"));
    }

    rlen = response(q, qlen, 3, NULL, 0, r);
    ok("nxdomain", ngx_prom_dns_parse(r, rlen, &a) == 0 && a.rcode == 3
                   && a.n == 0);
    ok("pick nothing", ngx_prom_ptr_pick(&a, D, DL, NULL, NULL, &svc, &ns)
                       == 0);

    ok("query is not a response", ngx_prom_dns_parse(q, qlen, &a) == -1);
    ok("truncated", ngx_prom_dns_parse(r, 20, &a) == -1);

    {
        /* a pointer loop never hangs */
        unsigned char  loop[64];

        memcpy(loop, q, 12);
        loop[2] |= 0x80;
        loop[12] = 0xc0;
        loop[13] = 12;
        ok("pointer loop", ngx_prom_dns_parse(loop, 14, &a) == -1);
    }

    /* PTR names */
#define PTR(s) ngx_prom_ptr_parse((const unsigned char *) s, strlen(s), D, \
                                  DL, &svc, &ns)
    ok("ptr svc", PTR("10-1-2-3.api.prod.svc.cluster.local.") == 2
                  && is(&svc, "api") && is(&ns, "prod"));
    ok("ptr pod", PTR("10-1-2-3.prod.pod.cluster.local") == 1
                  && is(&ns, "prod"));
    ok("ptr other domain", PTR("10-1-2-3.api.prod.svc.example.org") == 0);
    ok("ptr not a pod ip", PTR("web.api.prod.svc.cluster.local") == 0);
    ok("ptr too short", PTR("api.svc.cluster.local") == 0);
    ok("ptr too long", PTR("10-1-2-3.x.api.prod.svc.cluster.local") == 0);

    /* XFCC */
#define XFCC(s) ngx_prom_xfcc_parse((const unsigned char *) s, strlen(s), \
                                    &ns, &svc)
    ok("xfcc", XFCC("By=spiffe://cluster.local/ns/a/sa/b;Hash=x;"
                    "URI=spiffe://cluster.local/ns/shop/sa/checkout") == 1
               && is(&ns, "shop") && is(&svc, "checkout"));
    ok("xfcc several", XFCC("URI=spiffe://td/ns/n1/sa/s1,URI=spiffe://td/ns/"
                            "n2/sa/s2") == 1 && is(&svc, "s1"));
    ok("xfcc none", XFCC("Hash=abc") == 0);
    ok("xfcc malformed", XFCC("URI=spiffe://td/sa/x") == 0);

    /* /etc/hosts */
    {
        const char  *hosts =
            "# Kubernetes-managed hosts file.\n"
            "127.0.0.1\tlocalhost\n"
            "127.0.1.1\tweb-0\n"
            "10.233.7.9\tweb-0 web-0.web.prod.svc.cluster.local # pod\n";

        ok("hosts", ngx_prom_hosts_find((const unsigned char *) hosts,
                                        strlen(hosts),
                                        (const unsigned char *) "web-0", 5,
                                        &ip) && ip == 0x0ae90709);
        ok("hosts missing", !ngx_prom_hosts_find((const unsigned char *) hosts,
                                                 strlen(hosts),
                                                 (const unsigned char *) "x",
                                                 1, &ip));
    }

    /* client subnet */
#define SUBNET(i, want) (n = ngx_prom_client_subnet(i, sub), \
                         n == strlen(want) && memcmp(sub, want, n) == 0)
    ok("subnet private", SUBNET(0x0ae90411, "10.233.4.0"));
    ok("subnet 172.16", SUBNET(0xac100203, "172.16.2.0"));
    ok("subnet 172.32 is public", SUBNET(0xac200203, "172.32.0.0"));
    ok("subnet public", SUBNET(0x08080808, "8.8.0.0"));

    printf("%d passed, %d failed\n", passed, failed);

    return failed ? 1 : 0;
}
