#!/bin/sh
# Integration tests for the outbound leg: a forward proxy recorded as
# mode="1", plain HTTP and CONNECT (nginx 1.31+, the tunnel module).
#
#   tests/outbound_test.sh <nginx binary> <ngx_http_prometheus_module.so>
#
# Ports: 18060 (a target), 18061 (a TLS target), 13128 (the proxy), 19150.

set -eu

NGINX=${1:?usage: $0 <nginx> <module.so>}
MODULE=${2:?usage: $0 <nginx> <module.so>}
case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
case $MODULE in /*) ;; *) MODULE=$(pwd)/$MODULE ;; esac

PREFIX=$(mktemp -d)
trap 'kill "$(cat "$PREFIX/nginx.pid" 2>/dev/null)" 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$PREFIX"' EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf"

fails=0
pass() { printf 'ok    %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }
has()  { if printf '%s\n' "$metrics" | grep -qF -- "$2"; then pass "$1"; else fail "$1: no line with: $2"; fi; }

version=$("$NGINX" -v 2>&1 | sed -n 's#^nginx version: nginx/##p')
minor=$(echo "$version" | cut -d. -f2)
tunnel=""
[ "$(echo "$version" | cut -d. -f1)" -gt 1 ] || [ "$minor" -ge 31 ] && tunnel=1

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=target \
    -keyout "$PREFIX/conf/k.pem" -out "$PREFIX/conf/c.pem" 2> /dev/null

connect=""
[ -n "$tunnel" ] && connect='if ($request_method = CONNECT) { tunnel_pass; }'

cat > "$PREFIX/conf/nginx.conf" <<EOF
load_module $MODULE;
pid $PREFIX/nginx.pid;
error_log $PREFIX/logs/error.log warn;
events { worker_connections 128; }
http {
    access_log off;
    prometheus_listen 127.0.0.1:19150;
    server { listen 127.0.0.1:18060; prometheus_record off; location / { return 200 "target\n"; } }
    server {
        listen 127.0.0.1:18061 ssl;
        prometheus_record off;
        ssl_certificate c.pem;
        ssl_certificate_key k.pem;
        location / { return 200 "tls\n"; }
    }
    server {
        listen 127.0.0.1:13128;
        server_name outbound;
        resolver 127.0.0.1:1 ipv6=off;
        prometheus_mode 1;
        location / {
            $connect
            proxy_pass http://\$http_host\$request_uri;
        }
    }
}
EOF

"$NGINX" -p "$PREFIX" -c conf/nginx.conf
sleep 0.5

px() { curl -s --noproxy '' -x http://127.0.0.1:13128 "$@" || true; }

[ "$(px http://127.0.0.1:18060/orders/5)" = target ] && pass "plain HTTP through the proxy" || fail "plain HTTP"
[ -n "$tunnel" ] && { [ "$(px -k https://127.0.0.1:18061/a)" = tls ] && pass "HTTPS through the proxy" || fail "HTTPS"; }
[ -n "$tunnel" ] && px -o /dev/null https://127.0.0.1:1/ > /dev/null
metrics=$(curl -s http://127.0.0.1:19150/metrics)

has "plain HTTP is mode 1 with its route" 'nginx_http_requests_total{method="GET",route="/orders/$param",status="200",host="localhost",mode="1",protocol="http"} 1'
has "plain HTTP upstream time"           'nginx_http_upstream_response_time_seconds{method="GET",route="/orders/$param",status="200",host="localhost",mode="1"'
if [ -n "$tunnel" ]; then
    has "CONNECT: route is host:port"    'nginx_http_requests_total{method="CONNECT",route="localhost:18061",status="200",host="localhost",mode="1",protocol="tunnel"} 1'
    has "CONNECT refused: 502"           'method="CONNECT",route="localhost:1",status="502"'
else
    pass "CONNECT skipped: nginx $version has no tunnel module"
fi

# nginx itself (not the module) trips UBSan logging a refused CONNECT, in
# ngx_string.c; that one line is not counted
if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$PREFIX/logs/error.log" \
   | grep -v 'src/core/ngx_string.c:[0-9]*:[0-9]*: runtime error: null pointer passed'; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

echo
[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
