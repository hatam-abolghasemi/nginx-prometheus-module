/*
 * Unit tests for src/ngx_http_prometheus_label.c (route sanitizer, Host
 * classifier, label escaping). No nginx needed:
 *
 *   cc -Wall -Werror -Isrc -o label_test tests/label_test.c \
 *       src/ngx_http_prometheus_label.c && ./label_test
 */

#include <stdio.h>
#include <string.h>
#include "ngx_http_prometheus_label.h"

static int  failed, passed;

static const ngx_prom_route_conf_t  defaults = { NGX_PROM_RULE_ALL, 4, 64 };


static void
check(const char *name, const char *got, size_t len, const char *want)
{
    if (len == strlen(want) && memcmp(got, want, len) == 0) {
        passed++;
        return;
    }

    failed++;
    printf("FAIL %s: got \"%.*s\", want \"%s\"\n", name, (int) len, got, want);
}


static void
route(const char *name, const char *path, const ngx_prom_route_conf_t *conf,
    const char *want)
{
    unsigned char  out[1024];
    size_t         n;

    n = ngx_prom_route_sanitize((const unsigned char *) path, strlen(path),
                                conf ? conf : &defaults, out, sizeof(out));
    check(name, (char *) out, n, want);
}


static void
host(const char *name, const char *h, const char *prefix, const char *want)
{
    const unsigned char  *p;
    size_t                n;

    p = ngx_prom_host_classify((const unsigned char *) h, strlen(h),
                               (const unsigned char *) prefix,
                               prefix ? strlen(prefix) : 0, &n);
    check(name, (const char *) p, n, want);
}


static void
escape(const char *name, const char *in, size_t len, size_t cap,
    const char *want)
{
    unsigned char  out[256];
    size_t         n;

    n = ngx_prom_label_escape((const unsigned char *) in, len, out, cap);
    check(name, (char *) out, n, want);
}


int
main(void)
{
    ngx_prom_route_conf_t  c;

    /* the same cases as openresty/tests/lib_test.lua */
    route("numeric id", "/orders/8231", NULL, "/orders/$param");
    route("uuid", "/orders/8231/items/550e8400-e29b-41d4-a716-446655440000",
          NULL, "/orders/$param/items/$param");
    route("keeps real words", "/orders/new", NULL, "/orders/new");
    route("strips query, collapses slashes", "//a//b?x=1", NULL, "/a/b");
    route("adjacent params collapse", "/1/2/3", NULL, "/$param");
    route("empty is root", "", NULL, "/");
    route("root", "/", NULL, "/");
    route("query only", "/?a=1", NULL, "/");

    c = defaults;
    c.rules &= ~(NGX_PROM_RULE_PURE_NUMERIC | NGX_PROM_RULE_LONG_WITH_DIGIT);
    route("rules off", "/users/123", &c, "/users/123");

    c = defaults;
    c.rules &= ~(NGX_PROM_RULE_UUID | NGX_PROM_RULE_HEX
                 | NGX_PROM_RULE_LONG_WITH_DIGIT
                 | NGX_PROM_RULE_SLUG_SUFFIX_DIGIT);
    route("uuid off", "/u/550e8400-e29b-41d4-a716-446655440000", &c,
          "/u/550e8400-e29b-41d4-a716-446655440000");

    /* one per rule */
    route("hex", "/obj/a1b2c3d4", NULL, "/obj/$param");
    route("hex too short", "/obj/abcdef", NULL, "/obj/abcdef");
    route("slug suffix digit", "/zone-7", NULL, "/$param");
    route("dash without digits", "/zone-", NULL, "/zone-");
    route("long with digit", "/x/abc12", NULL, "/x/$param");
    route("short with digit", "/v1/x", NULL, "/v1/x");
    route("file extension", "/static/app.js", NULL, "/static/$param");
    route("one-letter extension", "/a.c", NULL, "/a.c");
    route("trailing dot", "/a.", NULL, "/a.");
    route("max segment len",
          "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          NULL, "/$param");
    route("trailing slash", "/a/b/", NULL, "/a/b");
    route("param then word", "/1/2/x/3", NULL, "/$param/x/$param");

    /* the output never overflows */
    {
        unsigned char  out[8];
        size_t         n;

        n = ngx_prom_route_sanitize((const unsigned char *) "/abc/def/ghi",
                                    12, &defaults, out, sizeof(out));
        check("bounded", (char *) out, n, "/abc/def");
    }

    host("cluster ip", "10.233.4.12", "10.233.", "kubernetes");
    host("other ip", "8.8.8.8:443", "10.233.", "$ip");
    host("no prefix", "10.233.4.12", NULL, "$ip");
    host("localhost", "localhost:80", "10.233.", "localhost");
    host("loopback", "127.0.0.1", NULL, "localhost");
    host("ipv6", "::1", "10.233.", "$ip");
    host("ipv6 bracketed", "[fe80::1]", NULL, "$ip");
    host("name passes", "api.example.com", "10.233.", "api.example.com");
    host("name keeps odd port", "host:abc", NULL, "host:abc");
    host("not an ip", "1.2.3", NULL, "1.2.3");

    escape("plain", "abc", 3, 256, "abc");
    escape("quote and backslash", "a\"b\\c", 5, 256, "a\\\"b\\\\c");
    escape("newline", "a\nb", 3, 256, "a\\nb");
    escape("control", "a\tb", 3, 256, "a_b");
    escape("utf-8 kept", "caf\xc3\xa9", 5, 256, "caf\xc3\xa9");
    escape("bad utf-8", "a\xff" "b", 3, 256, "a_b");
    escape("truncated utf-8", "a\xc3", 2, 256, "a_");
    escape("escape not split", "ab\"", 3, 3, "ab");

    printf("%d passed, %d failed\n", passed, failed);

    return failed ? 1 : 0;
}
