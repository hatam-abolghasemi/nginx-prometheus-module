/*
 * A DNS server that answers PTR queries from a table, for tests/module_test.sh.
 *
 *   fake_dns <port> <table>
 *
 * Each line of the table: an IPv4 address, then the PTR names to answer
 * with, or NXDOMAIN, or SERVFAIL. Addresses not in the table get NXDOMAIN.
 * Every query is answered after 0 ms; a line "<ip> SLOW <name>..." answers
 * after 3 s.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_ROWS  64

static struct {
    char  ip[16];
    char  names[8][256];
    int   n;
    int   rcode;
    int   slow;
} rows[MAX_ROWS];

static int  nrows;


static void
load(const char *path)
{
    char   line[2048], *tok;
    FILE  *f = fopen(path, "r");

    if (f == NULL) {
        perror(path);
        exit(1);
    }

    while (fgets(line, sizeof(line), f) && nrows < MAX_ROWS) {
        tok = strtok(line, " \t\n");
        if (tok == NULL || tok[0] == '#') {
            continue;
        }

        snprintf(rows[nrows].ip, sizeof(rows[nrows].ip), "%s", tok);

        while ((tok = strtok(NULL, " \t\n")) && rows[nrows].n < 8) {
            if (strcmp(tok, "NXDOMAIN") == 0) {
                rows[nrows].rcode = 3;
            } else if (strcmp(tok, "SERVFAIL") == 0) {
                rows[nrows].rcode = 2;
            } else if (strcmp(tok, "SLOW") == 0) {
                rows[nrows].slow = 1;
            } else {
                snprintf(rows[nrows].names[rows[nrows].n++], 256, "%s", tok);
            }
        }

        nrows++;
    }

    fclose(f);
}


static size_t
put_name(unsigned char *out, const char *s)
{
    size_t       o = 0, l;
    const char  *dot;

    while (*s) {
        dot = strchr(s, '.');
        l = dot ? (size_t) (dot - s) : strlen(s);
        out[o++] = (unsigned char) l;
        memcpy(out + o, s, l);
        o += l;
        s += l + (dot ? 1 : 0);
    }

    out[o++] = 0;

    return o;
}


int
main(int argc, char **argv)
{
    int                 fd, i, k, rcode;
    char                ip[16];
    unsigned char       q[512], r[4096];
    unsigned            oct[4];
    ssize_t             n;
    size_t              o, qend, at;
    socklen_t           alen;
    struct sockaddr_in  sin, from;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> <table>\n", argv[0]);
        return 1;
    }

    load(argv[2]);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    i = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short) atoi(argv[1]));
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) == -1) {
        perror("bind");
        return 1;
    }

    for ( ;; ) {
        alen = sizeof(from);
        n = recvfrom(fd, q, sizeof(q), 0, (struct sockaddr *) &from, &alen);
        if (n < 12) {
            continue;
        }

        /* the question name, as text: d.c.b.a.in-addr.arpa */
        o = 12;
        k = 0;
        while (o < (size_t) n && q[o] && k < 4) {
            char  label[8] = { 0 };
            memcpy(label, q + o + 1, q[o] < 7 ? q[o] : 7);
            oct[k++] = (unsigned) atoi(label);
            o += q[o] + 1;
        }
        while (o < (size_t) n && q[o]) {
            o += q[o] + 1;
        }
        qend = o + 1 + 4;
        if (k != 4 || qend > (size_t) n) {
            continue;
        }

        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", oct[3], oct[2], oct[1],
                 oct[0]);

        rcode = 3;
        i = -1;
        for (k = 0; k < nrows; k++) {
            if (strcmp(rows[k].ip, ip) == 0) {
                i = k;
                rcode = rows[k].rcode;
                break;
            }
        }

        if (i >= 0 && rows[i].slow) {
            sleep(3);
        }

        memcpy(r, q, qend);
        r[2] |= 0x80;
        r[3] = (unsigned char) (0x80 | rcode);
        r[6] = 0;
        r[7] = (rcode == 0 && i >= 0) ? (unsigned char) rows[i].n : 0;
        o = qend;

        for (k = 0; rcode == 0 && i >= 0 && k < rows[i].n; k++) {
            r[o++] = 0xc0; r[o++] = 12;
            r[o++] = 0; r[o++] = 12;
            r[o++] = 0; r[o++] = 1;
            r[o++] = 0; r[o++] = 0; r[o++] = 0; r[o++] = 30;
            at = o;
            o += 2;
            o += put_name(r + o, rows[i].names[k]);
            r[at] = (unsigned char) ((o - at - 2) >> 8);
            r[at + 1] = (unsigned char) (o - at - 2);
        }

        sendto(fd, r, o, 0, (struct sockaddr *) &from, alen);
    }
}
