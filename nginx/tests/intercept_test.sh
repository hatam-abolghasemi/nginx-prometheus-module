#!/bin/sh
# Integration tests for https interception (nginx 1.31+): CONNECTs whose
# first bytes are TLS are decrypted on the TLS terminator with a minted certificate,
# plaintext HTTP/2 goes to the h2c one, anything else is relayed as is; connections
# to the destination are kept per name, never shared across names.
#
#   tests/intercept_test.sh <nginx binary> <ngx_http_prometheus_module.so>
#
# Real gRPC too, when python3 has grpcio (tests/grpc_echo.py): over TLS
# and plaintext, through the two terminators, unary and streaming, with the
# status from grpc-status (trailers, or headers for a trailers-only answer).
#
# Ports: 18070 (TLS origin), 18071 (plain origin), 18074 (TLS origin, a CA
# outside the trusted bundle), 18072 / 18073 (gRPC origin,
# TLS / plaintext), 15353 (DNS), 13128 (the proxy), 13129 (the TLS
# terminator; the h2c one is a unix socket), 19152.

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
    [ -n "$grpcd" ] && kill "$grpcd" 2>/dev/null || true
    [ -n "${KEEP:-}" ] || rm -rf "$PREFIX"
}
dns=""
grpcd=""
trap cleanup EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf"
chmod 711 "$PREFIX"   # the workers reach the unix socket in it
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
printf 'subjectAltName=DNS:a.test,DNS:b.test,IP:127.0.0.1,IP:::1\n' > "$C/ext"
openssl x509 -req -in "$C/o.csr" -CA "$C/pca.crt" -CAkey "$C/pca.key" \
    -CAcreateserial -days 1 -extfile "$C/ext" -out "$C/o.crt" 2> /dev/null

# destinations are verified on demand (prometheus_intercept_trusted): a bundle
# with the public CA among others, readable by root only (the workers use the
# master's open file), and a CA that isn't in it for a destination to refuse
ec -x509 -days 1 -subj /CN=anchor-ca -keyout "$C/anchor.key" -out "$C/anchor.crt"
ec -x509 -days 1 -subj /CN=other-ca -keyout "$C/other.key" -out "$C/other.crt"
ec -x509 -days 1 -subj /CN=rogue-ca -keyout "$C/rca.key" -out "$C/rca.crt"
cat "$C/anchor.crt" "$C/other.crt" "$C/pca.crt" > "$C/bundle.crt"
chmod 600 "$C/bundle.crt"
ec -subj /CN=rogue -keyout "$C/r.key" -out "$C/r.csr"
openssl x509 -req -in "$C/r.csr" -CA "$C/rca.crt" -CAkey "$C/rca.key" \
    -CAcreateserial -days 1 -extfile "$C/ext" -out "$C/r.crt" 2> /dev/null

python3 "$HERE/fake_a_dns.py" 15353 &
dns=$!

# IPv6, where the host has it
if python3 -c 'import socket; socket.socket(socket.AF_INET6).bind(("::1", 0))' 2> /dev/null; then
    v6=true
    V6_TLS="listen [::1]:18070 ssl;"
    V6_PLAIN="listen [::1]:18071;"
else
    v6=false
    V6_TLS=""
    V6_PLAIN=""
fi

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
    prometheus_intercept_trusted $C/bundle.crt;
    resolver 127.0.0.1:15353 ipv6=off valid=10s;

    server {
        listen 127.0.0.1:18070 ssl;
        $V6_TLS
        http2 on;
        ssl_certificate $C/o.crt;
        ssl_certificate_key $C/o.key;
        prometheus_record off;
        location / { return 200 "tls \$request_uri c=\$connection r=\$connection_requests\n"; }
    }

    server {
        listen 127.0.0.1:18071;
        $V6_PLAIN
        http2 on;
        prometheus_record off;
        location / { return 200 "plain \$request_uri c=\$connection\n"; }
    }

    server {
        listen 127.0.0.1:18074 ssl;
        ssl_certificate $C/r.crt;
        ssl_certificate_key $C/r.key;
        prometheus_record off;
        location / { return 200 "rogue\n"; }
    }

    server {
        listen 127.0.0.1:13128;
        prometheus_mode 1;
        location / {
            if (\$request_method = CONNECT) {
                prometheus_intercept_connect tls=127.0.0.1:13129 h2c=unix:$PREFIX/h2c.sock;
            }
            proxy_pass http://\$http_host\$request_uri;
        }
    }

    upstream prometheus_intercept_https { server 0.0.0.1; prometheus_intercept_origin 16; }
    upstream prometheus_intercept_grpcs { server 0.0.0.1; prometheus_intercept_origin 16; }
    upstream prometheus_intercept_grpc  { server 0.0.0.1; prometheus_intercept_origin 16; }
    upstream prometheus_intercept_http  { server 0.0.0.1; prometheus_intercept_origin 16; }

    server {
        listen 127.0.0.1:13129 ssl proxy_protocol;
        http2 on;
        prometheus_mode 1;
        prometheus_intercept_resolve on;
        ssl_certificate data:\$prometheus_intercept_cert;
        ssl_certificate_key data:\$prometheus_intercept_key;
        proxy_ssl_server_name on;
        proxy_ssl_name \$host;
        proxy_ssl_verify on;
        proxy_ssl_trusted_certificate $C/anchor.crt;
        grpc_ssl_server_name on;
        grpc_ssl_name \$host;
        grpc_ssl_verify on;
        grpc_ssl_trusted_certificate $C/anchor.crt;
        location / {
            if (\$content_type ~* "^application/grpc(\$|[+;\s])") {
                grpc_pass grpcs://prometheus_intercept_grpcs;
            }
            grpc_set_header Host \$http_host;
            proxy_pass https://prometheus_intercept_https;
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_set_header Host \$http_host;
        }
    }

    server {
        listen unix:$PREFIX/h2c.sock proxy_protocol;
        http2 on;
        prometheus_mode 1;
        prometheus_intercept_resolve on;
        location / {
            if (\$content_type ~* "^application/grpc(\$|[+;\s])") {
                grpc_pass grpc://prometheus_intercept_grpc;
            }
            grpc_set_header Host \$http_host;
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

# a certificate from a CA the bundle doesn't have: refused
code=$(px --cacert "$C/ca.crt" -o /dev/null -w '%{http_code}' https://a.test:18074/rogue)
[ "$code" = 502 ] && pass "a CA outside the trusted bundle is a 502" || fail "rogue CA: $code"

# HTTP/2 inside TLS, and an IP literal (no SNI)
body=$(px --cacert "$C/ca.crt" --http2 https://127.0.0.1:18070/h2)
case $body in "tls /h2 "*) pass "h2 and an IP target" ;; *) fail "h2/IP: got \"$body\"" ;; esac

# plaintext HTTP/2 (what a gRPC client sends) goes to the h2c terminator
body=$(px -p --http2-prior-knowledge http://a.test:18071/h2c)
case $body in "plain /h2c "*) pass "h2c is decoded" ;; *) fail "h2c: got \"$body\"" ;; esac

# anything else is relayed as is
body=$(px -p http://a.test:18071/raw)
case $body in "plain /raw "*) pass "other tunnels are relayed" ;; *) fail "relay: got \"$body\"" ;; esac

# IPv6 literal targets: decrypted with a certificate for the address, and
# relayed
if $v6; then
    body=$(px --cacert "$C/ca.crt" "https://[::1]:18070/v6")
    case $body in "tls /v6 "*) pass "IPv6 target decrypted" ;; *) fail "IPv6 https: got \"$body\"" ;; esac
    body=$(px -p "http://[::1]:18071/v6raw")
    case $body in "plain /v6raw "*) pass "IPv6 target relayed" ;; *) fail "IPv6 relay: got \"$body\"" ;; esac
else
    echo "skip  IPv6: no ::1 here"
fi

# an unreachable target
code=$(px -p -o /dev/null -w '%{http_code}' http://a.test:18079/ || true)
pass "an unreachable target closes the tunnel ($code)"

# real gRPC: over TLS and plaintext h2c
grpc=false
if python3 -c 'import grpc' 2> /dev/null; then
    grpc=true
    python3 "$HERE/grpc_echo.py" server 18072 18073 "$C/o.crt" "$C/o.key" &
    grpcd=$!
    sleep 1
    g() { no_proxy= NO_PROXY= python3 "$HERE/grpc_echo.py" client "$@"; }
    P=http://127.0.0.1:13128
    [ "$(g a.test:18072 $P Say "$C/ca.crt")" = "0 hi" ]       && pass "grpcs unary" || fail "grpcs unary"
    [ "$(g a.test:18072 $P Many "$C/ca.crt")" = "0 hihihi" ]  && pass "grpcs streaming" || fail "grpcs streaming"
    [ "$(g a.test:18072 $P Missing "$C/ca.crt")" = "5 no such thing" ] && pass "grpcs error status" || fail "grpcs error"
    [ "$(g a.test:18072 $P Broken "$C/ca.crt")" = "14 went away" ] && pass "grpcs error after data" || fail "grpcs error after data"
    [ "$(g a.test:18073 $P Say)" = "0 hi" ]                   && pass "h2c gRPC unary" || fail "h2c gRPC unary"
    [ "$(g a.test:18073 $P Missing)" = "5 no such thing" ]    && pass "h2c gRPC error status" || fail "h2c gRPC error"
else
    echo "skip  gRPC: python3 has no grpcio (pip install grpcio)"
fi

sleep 0.5
metrics=$(curl -s --noproxy '' http://127.0.0.1:19152/metrics)

if $grpc; then
    has "grpcs OK from trailers" \
        'nginx_http_requests_total{method="POST",route="/test.Echo/Say",status="grpc_0",host="a.test",mode="1",protocol="grpcs"} 1'
    has "grpcs streaming" \
        'route="/test.Echo/Many",status="grpc_0",host="a.test",mode="1",protocol="grpcs"} 1'
    has "grpcs trailers-only error" \
        'route="/test.Echo/Missing",status="grpc_5",host="a.test",mode="1",protocol="grpcs"} 1'
    has "grpcs error in trailers after data" \
        'route="/test.Echo/Broken",status="grpc_14",host="a.test",mode="1",protocol="grpcs"} 1'
    has "h2c gRPC OK" \
        'route="/test.Echo/Say",status="grpc_0",host="a.test",mode="1",protocol="grpc"} 1'
    has "h2c gRPC error" \
        'route="/test.Echo/Missing",status="grpc_5",host="a.test",mode="1",protocol="grpc"} 1'
    hasnt "gRPC tunnels are not CONNECTs" 'route="a.test:18072"'
fi

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
