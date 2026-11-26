/*
 * Label value shaping for ngx_http_prometheus_module. See the header.
 */

#include <string.h>
#include "ngx_http_prometheus_label.h"


static const unsigned char  param[] = "$param";
#define PARAM_LEN  (sizeof(param) - 1)


static int
is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}


static int
is_hex(unsigned char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}


static int
is_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}


static int
is_uuid(const unsigned char *s, size_t len)
{
    size_t  i;

    if (len != 36) {
        return 0;
    }

    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') {
                return 0;
            }

        } else if (!is_hex(s[i])) {
            return 0;
        }
    }

    return 1;
}


static int
is_param_segment(const unsigned char *s, size_t len,
    const ngx_prom_route_conf_t *conf)
{
    size_t  i, n;
    int     all_digit, all_hex, has_digit;

    if (len > conf->max_segment_len) {
        return 1;
    }

    all_digit = 1;
    all_hex = 1;
    has_digit = 0;

    for (i = 0; i < len; i++) {
        if (is_digit(s[i])) {
            has_digit = 1;

        } else {
            all_digit = 0;
        }

        if (!is_hex(s[i])) {
            all_hex = 0;
        }
    }

    if ((conf->rules & NGX_PROM_RULE_PURE_NUMERIC) && all_digit) {
        return 1;
    }

    if ((conf->rules & NGX_PROM_RULE_UUID) && is_uuid(s, len)) {
        return 1;
    }

    if ((conf->rules & NGX_PROM_RULE_HEX) && len >= 8 && all_hex) {
        return 1;
    }

    if (conf->rules & NGX_PROM_RULE_SLUG_SUFFIX_DIGIT) {
        /* "-%d+$" */
        for (n = len; n > 0 && is_digit(s[n - 1]); n--) { /* void */ }

        if (n < len && n > 0 && s[n - 1] == '-') {
            return 1;
        }
    }

    if ((conf->rules & NGX_PROM_RULE_LONG_WITH_DIGIT)
        && len > conf->min_len && has_digit)
    {
        return 1;
    }

    if (conf->rules & NGX_PROM_RULE_FILE_EXTENSION) {
        /* "%.%a%a+$" */
        for (n = len; n > 0 && is_alpha(s[n - 1]); n--) { /* void */ }

        if (len - n >= 2 && n > 0 && s[n - 1] == '.') {
            return 1;
        }
    }

    return 0;
}


size_t
ngx_prom_route_sanitize(const unsigned char *path, size_t len,
    const ngx_prom_route_conf_t *conf, unsigned char *out, size_t cap)
{
    size_t                start, end, i, o, seg;
    int                   last_param;
    const unsigned char  *q;

    if (cap == 0) {
        return 0;
    }

    q = memchr(path, '?', len);
    if (q) {
        len = q - path;
    }

    o = 0;
    last_param = 0;
    i = 0;

    while (i < len) {
        while (i < len && path[i] == '/') {
            i++;
        }

        start = i;

        while (i < len && path[i] != '/') {
            i++;
        }

        end = i;
        seg = end - start;

        if (seg == 0) {
            break;
        }

        if (is_param_segment(path + start, seg, conf)) {
            if (last_param) {
                continue;
            }

            if (o + 1 + PARAM_LEN > cap) {
                break;
            }

            out[o++] = '/';
            memcpy(out + o, param, PARAM_LEN);
            o += PARAM_LEN;
            last_param = 1;

        } else {
            if (o + 1 + seg > cap) {
                break;
            }

            out[o++] = '/';
            memcpy(out + o, path + start, seg);
            o += seg;
            last_param = 0;
        }
    }

    if (o == 0) {
        out[o++] = '/';
    }

    return o;
}


static int
is_ipv4(const unsigned char *s, size_t len)
{
    size_t  i, digits, dots;

    digits = 0;
    dots = 0;

    for (i = 0; i < len; i++) {
        if (is_digit(s[i])) {
            digits++;

        } else if (s[i] == '.' && digits > 0) {
            dots++;
            digits = 0;

        } else {
            return 0;
        }
    }

    return dots == 3 && digits > 0;
}


static int
is_ipv6(const unsigned char *s, size_t len)
{
    size_t  i;
    int     colon;

    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
        s++;
        len -= 2;
    }

    colon = 0;

    for (i = 0; i < len; i++) {
        if (s[i] == ':') {
            colon = 1;

        } else if (!is_hex(s[i])) {
            return 0;
        }
    }

    return colon;
}


const unsigned char *
ngx_prom_host_classify(const unsigned char *host, size_t len,
    const unsigned char *cluster_prefix, size_t prefix_len, size_t *out_len)
{
    static const unsigned char  localhost[] = "localhost";
    static const unsigned char  kubernetes[] = "kubernetes";
    static const unsigned char  ip[] = "$ip";
    size_t                      i, n;

    /* "^([^:]+):?%d*$": drop a ":<digits>" port from a name or IPv4 */
    n = len;
    for (i = 0; i < len; i++) {
        if (host[i] == ':') {
            break;
        }
    }

    if (i < len && i > 0) {
        n = i;
        for (i = i + 1; i < len; i++) {
            if (!is_digit(host[i])) {
                n = len;
                break;
            }
        }
    }

    if ((n == 9 && memcmp(host, "127.0.0.1", 9) == 0)
        || (n == 9 && memcmp(host, "localhost", 9) == 0))
    {
        *out_len = sizeof(localhost) - 1;
        return localhost;
    }

    if (is_ipv4(host, n)) {
        if (prefix_len && n >= prefix_len
            && memcmp(host, cluster_prefix, prefix_len) == 0)
        {
            *out_len = sizeof(kubernetes) - 1;
            return kubernetes;
        }

        *out_len = sizeof(ip) - 1;
        return ip;
    }

    if (is_ipv6(host, len)) {
        *out_len = sizeof(ip) - 1;
        return ip;
    }

    *out_len = len;
    return host;
}


/* length of the valid UTF-8 sequence at s, or 0 */
static size_t
utf8_len(const unsigned char *s, size_t len)
{
    size_t  n, i;

    if (s[0] < 0x80) {
        return 1;

    } else if ((s[0] & 0xe0) == 0xc0 && s[0] >= 0xc2) {
        n = 2;

    } else if ((s[0] & 0xf0) == 0xe0) {
        n = 3;

    } else if ((s[0] & 0xf8) == 0xf0 && s[0] <= 0xf4) {
        n = 4;

    } else {
        return 0;
    }

    if (n > len) {
        return 0;
    }

    for (i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
    }

    return n;
}


size_t
ngx_prom_label_escape(const unsigned char *in, size_t len,
    unsigned char *out, size_t cap)
{
    size_t  i, o, n;

    o = 0;

    for (i = 0; i < len; /* void */) {
        switch (in[i]) {

        case '\\':
        case '"':
            if (o + 2 > cap) {
                return o;
            }
            out[o++] = '\\';
            out[o++] = in[i++];
            continue;

        case '\n':
            if (o + 2 > cap) {
                return o;
            }
            out[o++] = '\\';
            out[o++] = 'n';
            i++;
            continue;
        }

        if (in[i] < 0x20 || in[i] == 0x7f) {
            n = 0;

        } else {
            n = utf8_len(in + i, len - i);
        }

        if (n == 0) {
            if (o + 1 > cap) {
                return o;
            }
            out[o++] = '_';
            i++;
            continue;
        }

        if (o + n > cap) {
            return o;
        }

        memcpy(out + o, in + i, n);
        o += n;
        i += n;
    }

    return o;
}
