/*
 * Caller and own identity for ngx_http_prometheus_module. See the header.
 */

#include <stdio.h>
#include <string.h>
#include "ngx_http_prometheus_identity.h"


#define DNS_TYPE_PTR    12
#define DNS_CLASS_IN    1
#define DNS_HEADER_LEN  12


static int
is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}


int
ngx_prom_parse_ipv4(const unsigned char *s, size_t len, uint32_t *ip)
{
    size_t    i;
    uint32_t  octet, value;
    int       dots, digits;

    value = 0;
    octet = 0;
    dots = 0;
    digits = 0;

    for (i = 0; i < len; i++) {
        if (is_digit(s[i])) {
            octet = octet * 10 + (s[i] - '0');
            if (++digits > 3 || octet > 255) {
                return 0;
            }

        } else if (s[i] == '.' && digits > 0 && dots < 3) {
            value = (value << 8) | octet;
            octet = 0;
            digits = 0;
            dots++;

        } else {
            return 0;
        }
    }

    if (dots != 3 || digits == 0) {
        return 0;
    }

    *ip = (value << 8) | octet;

    return 1;
}


size_t
ngx_prom_dns_ptr_query(uint32_t ip, uint16_t id, unsigned char *buf,
    size_t cap)
{
    char    label[4];
    size_t  o, n;
    int     i;

    /* header + 4 labels of up to 3 digits + in-addr.arpa + type/class */
    if (cap < DNS_HEADER_LEN + 4 * 4 + 14 + 4) {
        return 0;
    }

    memset(buf, 0, DNS_HEADER_LEN);
    buf[0] = id >> 8;
    buf[1] = id & 0xff;
    buf[2] = 0x01;                 /* RD */
    buf[5] = 1;                    /* QDCOUNT */

    o = DNS_HEADER_LEN;

    for (i = 0; i < 4; i++) {
        n = (size_t) snprintf(label, sizeof(label), "%u",
                              (unsigned) ((ip >> (8 * i)) & 0xff));
        buf[o++] = (unsigned char) n;
        memcpy(buf + o, label, n);
        o += n;
    }

    memcpy(buf + o, "\7in-addr\4arpa\0", 14);
    o += 14;

    buf[o++] = 0;
    buf[o++] = DNS_TYPE_PTR;
    buf[o++] = 0;
    buf[o++] = DNS_CLASS_IN;

    return o;
}


/*
 * Reads a (possibly compressed) name at *off into out as dotted text, and
 * moves *off past it. Returns the text length, or -1.
 */
static long
dns_name(const unsigned char *pkt, size_t len, size_t *off,
    unsigned char *out, size_t cap)
{
    size_t  p, o, n, end;
    int     jumped, hops;

    p = *off;
    o = 0;
    jumped = 0;
    hops = 0;
    end = 0;

    for ( ;; ) {
        if (p >= len) {
            return -1;
        }

        n = pkt[p];

        if ((n & 0xc0) == 0xc0) {
            if (p + 1 >= len || ++hops > 16) {
                return -1;
            }

            if (!jumped) {
                end = p + 2;
                jumped = 1;
            }

            p = ((n & 0x3f) << 8) | pkt[p + 1];
            continue;
        }

        if (n & 0xc0) {
            return -1;
        }

        p++;

        if (n == 0) {
            break;
        }

        if (p + n > len || o + n + 1 > cap) {
            return -1;
        }

        if (o) {
            out[o++] = '.';
        }

        memcpy(out + o, pkt + p, n);
        o += n;
        p += n;
    }

    *off = jumped ? end : p;

    return (long) o;
}


int
ngx_prom_dns_parse(const unsigned char *pkt, size_t len,
    ngx_prom_dns_answer_t *answer)
{
    size_t         off, rdlen, qd, an, i;
    long           n;
    uint16_t       type;
    uint32_t       ip;
    unsigned char  name[NGX_PROM_DNS_NAME_MAX];
    unsigned char  octets[4];
    const unsigned char  *p, *end;
    int            k;

    if (len < DNS_HEADER_LEN || !(pkt[2] & 0x80)) {
        return -1;                 /* not a response */
    }

    answer->id = (uint16_t) ((pkt[0] << 8) | pkt[1]);
    answer->rcode = pkt[3] & 0x0f;
    answer->n = 0;

    qd = (pkt[4] << 8) | pkt[5];
    an = (pkt[6] << 8) | pkt[7];

    if (qd != 1) {
        return -1;
    }

    off = DNS_HEADER_LEN;

    n = dns_name(pkt, len, &off, name, sizeof(name));
    if (n < 0 || off + 4 > len) {
        return -1;
    }

    if (((pkt[off] << 8) | pkt[off + 1]) != DNS_TYPE_PTR) {
        return -1;
    }

    off += 4;

    /* the question: d.c.b.a.in-addr.arpa */

    if ((size_t) n < 13
        || memcmp(name + n - 13, ".in-addr.arpa", 13) != 0)
    {
        return -1;
    }

    p = name;
    end = name + n - 13;
    k = 0;

    while (p < end && k < 4) {
        unsigned  v = 0, d = 0;

        while (p < end && *p != '.') {
            if (!is_digit(*p) || ++d > 3) {
                return -1;
            }
            v = v * 10 + (*p++ - '0');
        }

        if (d == 0 || v > 255) {
            return -1;
        }

        octets[k++] = (unsigned char) v;

        if (p < end) {
            p++;
        }
    }

    if (k != 4 || p != end) {
        return -1;
    }

    ip = ((uint32_t) octets[3] << 24) | ((uint32_t) octets[2] << 16)
         | ((uint32_t) octets[1] << 8) | octets[0];
    answer->ip = ip;

    for (i = 0; i < an; i++) {
        if (dns_name(pkt, len, &off, name, sizeof(name)) < 0
            || off + 10 > len)
        {
            return -1;
        }

        type = (uint16_t) ((pkt[off] << 8) | pkt[off + 1]);
        rdlen = (pkt[off + 8] << 8) | pkt[off + 9];
        off += 10;

        if (off + rdlen > len) {
            return -1;
        }

        if (type == DNS_TYPE_PTR && answer->n < NGX_PROM_DNS_ANSWERS) {
            size_t  r = off;

            n = dns_name(pkt, len, &r, answer->name[answer->n],
                         NGX_PROM_DNS_NAME_MAX);
            if (n >= 0) {
                answer->name_len[answer->n++] = (size_t) n;
            }
        }

        off += rdlen;
    }

    return 0;
}


static int
set_name(ngx_prom_name_t *to, const unsigned char *s, size_t len)
{
    if (len == 0 || len > NGX_PROM_NAME_MAX) {
        return 0;
    }

    memcpy(to->data, s, len);
    to->len = len;

    return 1;
}


int
ngx_prom_ptr_parse(const unsigned char *name, size_t len,
    const unsigned char *domain, size_t domain_len, ngx_prom_name_t *service,
    ngx_prom_name_t *namespace)
{
    const unsigned char  *label[8];
    size_t                label_len[8], i, n, start;

    if (len && name[len - 1] == '.') {
        len--;
    }

    /* strip ".<domain>" */
    if (len <= domain_len + 1
        || memcmp(name + len - domain_len, domain, domain_len) != 0
        || name[len - domain_len - 1] != '.')
    {
        return 0;
    }

    len -= domain_len + 1;

    /* split what's left; at most 4 labels are meaningful */
    n = 0;
    start = 0;

    for (i = 0; i <= len; i++) {
        if (i == len || name[i] == '.') {
            if (n == 8) {
                return 0;
            }
            label[n] = name + start;
            label_len[n++] = i - start;
            start = i + 1;
        }
    }

    /* the first label is the pod IP, dashed: [0-9-]+ */
    if (n < 3 || label_len[0] == 0) {
        return 0;
    }

    for (i = 0; i < label_len[0]; i++) {
        if (!is_digit(label[0][i]) && label[0][i] != '-') {
            return 0;
        }
    }

    if (n == 4 && label_len[3] == 3 && memcmp(label[3], "svc", 3) == 0) {
        if (set_name(service, label[1], label_len[1])
            && set_name(namespace, label[2], label_len[2]))
        {
            return 2;
        }
        return 0;
    }

    if (n == 3 && label_len[2] == 3 && memcmp(label[2], "pod", 3) == 0) {
        return set_name(namespace, label[1], label_len[1]) ? 1 : 0;
    }

    return 0;
}


static int
name_cmp(const ngx_prom_name_t *a, const ngx_prom_name_t *b)
{
    size_t  n;
    int     c;

    n = a->len < b->len ? a->len : b->len;
    c = memcmp(a->data, b->data, n);

    if (c) {
        return c;
    }

    return (a->len > b->len) - (a->len < b->len);
}


int
ngx_prom_ptr_pick(const ngx_prom_dns_answer_t *answer,
    const unsigned char *domain, size_t domain_len,
    int (*ignore)(void *data, const ngx_prom_name_t *service), void *data,
    ngx_prom_name_t *service, ngx_prom_name_t *namespace)
{
    size_t           i;
    int              rc, kept, ignored, have_pod;
    ngx_prom_name_t  svc, ns, pod_ns;
    ngx_prom_name_t  best_svc[2], best_ns[2];   /* [0] kept, [1] ignored */
    int              have[2];

    have[0] = have[1] = 0;
    have_pod = 0;

    for (i = 0; i < answer->n; i++) {
        rc = ngx_prom_ptr_parse(answer->name[i], answer->name_len[i],
                                domain, domain_len, &svc, &ns);

        if (rc == 2) {
            kept = (ignore && ignore(data, &svc)) ? 1 : 0;

            if (!have[kept]
                || name_cmp(&svc, &best_svc[kept]) < 0
                || (name_cmp(&svc, &best_svc[kept]) == 0
                    && name_cmp(&ns, &best_ns[kept]) < 0))
            {
                best_svc[kept] = svc;
                best_ns[kept] = ns;
                have[kept] = 1;
            }

        } else if (rc == 1 && !have_pod) {
            pod_ns = ns;
            have_pod = 1;
        }
    }

    ignored = have[0] ? 0 : 1;

    if (have[ignored]) {
        *service = best_svc[ignored];
        *namespace = best_ns[ignored];
        return 2;
    }

    if (have_pod) {
        *namespace = pod_ns;
        return 1;
    }

    return 0;
}


int
ngx_prom_xfcc_parse(const unsigned char *value, size_t len,
    ngx_prom_name_t *namespace, ngx_prom_name_t *service_account)
{
    static const char     uri[] = "URI=spiffe://";
    const unsigned char  *p, *end, *s;
    size_t                i;

    end = value + len;

    for (i = 0; i + sizeof(uri) - 1 <= len; i++) {
        if (memcmp(value + i, uri, sizeof(uri) - 1) != 0) {
            continue;
        }

        /* trust domain */
        p = value + i + sizeof(uri) - 1;
        while (p < end && *p != '/') {
            if (*p == ';' || *p == ',') {
                goto next;
            }
            p++;
        }

        if (end - p < 4 || memcmp(p, "/ns/", 4) != 0) {
            goto next;
        }

        p += 4;
        s = p;
        while (p < end && *p != '/' && *p != ';' && *p != ',') {
            p++;
        }

        if (!set_name(namespace, s, p - s)
            || end - p < 4 || memcmp(p, "/sa/", 4) != 0)
        {
            goto next;
        }

        p += 4;
        s = p;
        while (p < end && *p != ';' && *p != ',') {
            p++;
        }

        if (set_name(service_account, s, p - s)) {
            return 1;
        }

    next:
        continue;
    }

    return 0;
}


int
ngx_prom_hosts_find(const unsigned char *content, size_t len,
    const unsigned char *hostname, size_t hostname_len, uint32_t *ip)
{
    const unsigned char  *line, *eol, *p, *s, *end;
    uint32_t              addr;

    if (hostname_len == 0) {
        return 0;
    }

    end = content + len;

    for (line = content; line < end; line = eol + 1) {
        eol = memchr(line, '\n', end - line);
        if (eol == NULL) {
            eol = end;
        }

        p = memchr(line, '#', eol - line);
        if (p == NULL) {
            p = eol;
        }

        /* strip the comment, then: <ip> <name>... */
        s = line;
        while (s < p && (*s == ' ' || *s == '\t')) {
            s++;
        }

        {
            const unsigned char  *ip_end = s;

            while (ip_end < p && *ip_end != ' ' && *ip_end != '\t') {
                ip_end++;
            }

            if (!ngx_prom_parse_ipv4(s, ip_end - s, &addr)
                || (addr >> 24) == 127)
            {
                continue;
            }

            s = ip_end;
        }

        while (s < p) {
            const unsigned char  *w;

            while (s < p && (*s == ' ' || *s == '\t' || *s == '\r')) {
                s++;
            }

            w = s;
            while (s < p && *s != ' ' && *s != '\t' && *s != '\r') {
                s++;
            }

            if ((size_t) (s - w) == hostname_len
                && memcmp(w, hostname, hostname_len) == 0)
            {
                *ip = addr;
                return 1;
            }
        }

        if (eol == end) {
            break;
        }
    }

    return 0;
}


size_t
ngx_prom_client_subnet(uint32_t ip, unsigned char *out)
{
    unsigned  a, b, c;

    a = ip >> 24;
    b = (ip >> 16) & 0xff;
    c = (ip >> 8) & 0xff;

    if (a == 10 || a == 192 || (a == 172 && b >= 16 && b <= 31)) {
        return (size_t) snprintf((char *) out, 16, "%u.%u.%u.0", a, b, c);
    }

    return (size_t) snprintf((char *) out, 16, "%u.%u.0.0", a, b);
}
