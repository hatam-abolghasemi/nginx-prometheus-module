#!/bin/sh
# Integration tests for https interception (nginx 1.31+): CONNECTs whose
# first bytes are TLS are decrypted on :3129 with a minted certificate,
# plaintext HTTP/2 goes to :3130, anything else is relayed as is; connections
# to the destination are kept per name, never shared across names.
#
#   tests/intercept_test.sh <nginx binary> <ngx_http_prometheus_module.so>
#
# Ports: 18070 (TLS origin), 18071 (plain origin), 15353 (DNS), 13128 (the
# proxy), 3129 and 3130 (the terminators, fixed), 19152.

set -eu

NGINX=${1:?usage: $0 <nginx> <module.so>}
MODULE=${2:?usage: $0 <nginx> <module.so>}
case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
case $MODULE in /*) ;; *) MODULE=$(pwd)/$MODULE ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)

version=$("$NGINX" -v 2>&1 | sed -n 's#^nginx version: nginx/##p')
if [ "$(echo "$version" | cut -d. -f1)" -eq 1 ] && [ "$(echo "$version" | cut -d. -f2)" -lt 31 ]; then
    echo "skip  https interception needs nginx 1.31+ (this is $version)"
    exit 0
fi

PREFIX=$(mktemp -d)
cleanup() {
    kill "$(cat "$PREFIX/nginx.pid" 2>/dev/null)" 2>/dev/null || true
    kill "$dns" 2>/dev/null || true
    [ -n "${KEEP:-}" ] || rm -rf "$PREFIX"
}
dns=""
trap cleanup EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf"
C=$PREFIX/conf

fails=0
pass() { printf 'ok    %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }
has()  { if printf '%s\n' "$metrics" | grep -qF -- "$2"; then pass "$1"; else fail "$1: no line with: $2"; fi; }
hasnt() { if printf '%s\n' "$metrics" | grep -qF -- "$2"; then fail "$1: has: $2"; else pass "$1"; fi; }

# the CA the app trusts, and a "public" CA the origin's certificate is from
ec() { openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes "$@" 2> /dev/null; }
ec -x509 -days 1 -subj /CN=intercept-ca -keyout "$C/ca.key" -out "$C/ca.crt"
ec -x509 -days 1 -subj /CN=public-ca -keyout "$C/pca.key" -out "$C/pca.crt"
ec -subj /CN=origin -keyout "$C/o.key" -out "$C/o.csr"
printf 'subjectAltName=DNS:a.test,DNS:b.test,IP:127.0.0.1\n' > "$C/ext"
openssl x509 -req -in "$C/o.csr" -CA "$C/pca.crt" -CAkey "$C/pca.key" \
    -CAcreateserial -days 1 -extfile "$C/ext" -out "$C/o.crt" 2> /dev/null

python3 "$HERE/fake_a_dns.py" 15353 &
dns=$!

# the generated sidecar config, with test ports
cat > "$C/nginx.conf" <<EOF
load_module $MODULE;
pid $PREFIX/nginx.pid;
error_log $PREFIX/logs/error.log info;
worker_processes 1;   # the pool is per worker
events { worker_connections 256; }
http {
    access_log off;
    prometheus_listen 19152;
    prometheus_intercept_ca $C/ca.crt $C/ca.key;
    resolver 127.0.0.1:15353 ipv6=off valid=10s;

    server {
        listen 127.0.0.1:18070 ssl;
        http2 on;
        ssl_certificate $C/o.crt;
        ssl_certificate_key $C/o.key;
        prometheus_record off;
        location / { return 200 "tls \$request_uri c=\$connection r=\$connection_requests\n"; }
    }

    server {
        listen 127.0.0.1:18071;
        http2 on;
        prometheus_record off;
        location / { return 200 "plain \$request_uri c=\$connection\n"; }
    }

    server {
        listen 127.0.0.1:13128;
        prometheus_mode 1;
        location / {
            if (\$request_method = CONNECT) { prometheus_intercept_connect; }
            proxy_pass http://\$http_host\$request_uri;
        }
    }

    upstream prometheus_intercept_https { server 0.0.0.1; prometheus_intercept_origin 16; }
    upstream prometheus_intercept_http  { server 0.0.0.1; prometheus_intercept_origin 16; }

    server {
        listen 127.0.0.1:3129 ssl proxy_protocol;
        http2 on;
        prometheus_mode 1;
        prometheus_intercept_resolve on;
        ssl_certificate data:\$prometheus_intercept_cert;
        ssl_certificate_key data:\$prometheus_intercept_key;
        proxy_ssl_server_name on;
        proxy_ssl_name \$host;
        proxy_ssl_verify on;
        proxy_ssl_trusted_certificate $C/pca.crt;
        location / {
            proxy_pass https://prometheus_intercept_https;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_set_header Host \$http_host;
        }
    }

    server {
        listen 127.0.0.1:3130 proxy_protocol;
        http2 on;
        prometheus_mode 1;
        prometheus_intercept_resolve on;
        location / {
            proxy_pass http://prometheus_intercept_http;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_set_header Host \$http_host;
        }
    }
}
EOF

"$NGINX" -p "$PREFIX" -c "$C/nginx.conf"
sleep 1

px() { curl -s -m 10 --noproxy '' -x http://127.0.0.1:13128 "$@"; }

# HTTPS through the proxy: decrypted, with a certificate from our CA
body=$(px --cacert "$C/ca.crt" https://a.test:18070/orders/1)
case $body in "tls /orders/1 "*) pass "https is decrypted and forwarded" ;; *) fail "https: got \"$body\"" ;; esac

# the destination connection is kept: same name, same upstream connection
b1=$(px --cacert "$C/ca.crt" https://a.test:18070/k/1 | sed 's/.*c=\([0-9]*\).*/\1/')
b2=$(px --cacert "$C/ca.crt" https://a.test:18070/k/2 | sed 's/.*c=\([0-9]*\).*/\1/')
[ -n "$b1" ] && [ "$b1" = "$b2" ] && pass "a connection to a.test is reused" || fail "no reuse: $b1 $b2"

# ... but never across names on the same address
b3=$(px --cacert "$C/ca.crt" https://b.test:18070/k/3 | sed 's/.*c=\([0-9]*\).*/\1/')
[ -n "$b3" ] && [ "$b3" != "$b1" ] && pass "b.test gets its own connection" || fail "b.test shared a.test's: $b3"

# a name the destination's certificate doesn't have: refused, not reused
code=$(px --cacert "$C/ca.crt" -o /dev/null -w '%{http_code}' https://c.test:18070/x)
[ "$code" = 502 ] && pass "an unverified destination is a 502" || fail "c.test: $code"

# HTTP/2 inside TLS, and an IP literal (no SNI)
body=$(px --cacert "$C/ca.crt" --http2 https://127.0.0.1:18070/h2)
case $body in "tls /h2 "*) pass "h2 and an IP target" ;; *) fail "h2/IP: got \"$body\"" ;; esac

# plaintext HTTP/2 (what a gRPC client sends) goes to :3130
body=$(px -p --http2-prior-knowledge http://a.test:18071/h2c)
case $body in "plain /h2c "*) pass "h2c is decoded" ;; *) fail "h2c: got \"$body\"" ;; esac

# anything else is relayed as is
body=$(px -p http://a.test:18071/raw)
case $body in "plain /raw "*) pass "other tunnels are relayed" ;; *) fail "relay: got \"$body\"" ;; esac

# an unreachable target
code=$(px -p -o /dev/null -w '%{http_code}' http://a.test:18079/ || true)
pass "an unreachable target closes the tunnel ($code)"

sleep 0.5
metrics=$(curl -s --noproxy '' http://127.0.0.1:19152/metrics)

has "decrypted https is recorded with its route" \
    'nginx_http_requests_total{method="GET",route="/orders/$param",status="200",host="a.test",mode="1",protocol="https"} 1'
has "h2c is recorded as http" \
    'route="/h2c",status="200",host="a.test",mode="1",protocol="http"} 1'
has "the unverified destination is recorded as 502" \
    'route="/x",status="502",host="c.test",mode="1",protocol="https"} 1'
has "a relayed tunnel is a CONNECT" \
    'nginx_http_requests_total{method="CONNECT",route="a.test:18071",status="200",host="a.test",mode="1",protocol="tunnel"} 1'
hasnt "decoded tunnels are not counted as CONNECTs" 'route="a.test:18070"'

# nginx core (http/2, a refused CONNECT) trips UBSan by itself: not counted
if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$PREFIX/logs/error.log" \
   | grep -v 'src/http/v2/[a-z_0-9]*\.c:[0-9]*:[0-9]*: runtime error\|src/core/ngx_string.c:[0-9]*:[0-9]*: runtime error'; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

[ "$fails" -eq 0 ] || { echo "$fails failed"; exit 1; }
echo "intercept: all passed"
