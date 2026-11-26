#!/bin/sh
# Plug and play: load_module alone gives metrics on :9145, whatever the rest of
# the configuration is.
#
#   tests/plug_test.sh <nginx binary> <ngx_http_prometheus_module.so>
#
# Uses ports 18070 and 9145 (the default).

set -eu

NGINX=${1:?usage: $0 <nginx> <module.so>}
MODULE=${2:?usage: $0 <nginx> <module.so>}
case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
case $MODULE in /*) ;; *) MODULE=$(pwd)/$MODULE ;; esac

PREFIX=$(mktemp -d)
stop() { "$NGINX" -p "$PREFIX" -c conf/nginx.conf -s stop 2>/dev/null || true; sleep 0.3; }
trap 'stop; [ -n "${KEEP:-}" ] || rm -rf "$PREFIX"' EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf"

fails=0
pass() { printf 'ok    %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }

conf() {
    printf 'load_module %s;\npid %s/nginx.pid;\nerror_log %s/logs/error.log warn;\nevents { worker_connections 64; }\n%s\n' \
        "$MODULE" "$PREFIX" "$PREFIX" "$1" > "$PREFIX/conf/nginx.conf"
}
start() { "$NGINX" -p "$PREFIX" -c conf/nginx.conf; sleep 0.5; }
code()  { curl -s -o /dev/null -w '%{http_code}' "$@" || true; }

# nothing but a server: metrics appear on :9145 anyway
conf 'http { access_log off; server { listen 127.0.0.1:18070; location / { return 200 "ok\n"; } } }'
start
curl -s -o /dev/null http://127.0.0.1:18070/orders/12
metrics=$(curl -s http://127.0.0.1:9145/metrics)
if printf '%s\n' "$metrics" | grep -qF 'nginx_http_requests_total{method="GET",route="/orders/$param",status="200"'; then
    pass "load_module alone: :9145/metrics"
else
    fail "load_module alone: no request series on :9145"
fi
[ "$(curl -s http://127.0.0.1:9145/healthz)" = "ok" ] && pass "healthz" || fail "healthz"
[ "$(code http://127.0.0.1:9145/nope)" = 404 ] && pass "unknown path is 404" || fail "unknown path"
[ "$(code -I http://127.0.0.1:9145/metrics)" = 200 ] && pass "HEAD" || fail "HEAD"
[ "$(code 'http://127.0.0.1:9145/metrics?x=1')" = 200 ] && pass "query string ignored" || fail "query string"
ct=$(curl -s -o /dev/null -w '%{content_type}' http://127.0.0.1:9145/metrics)
case $ct in "text/plain; version=0.0.4"*) pass "content type" ;; *) fail "content type: $ct" ;; esac
if printf '%s\n' "$metrics" | grep -qF ':9145'; then fail "scrapes recorded"; else pass "scrapes not recorded"; fi

# a slow, partial request times out without hurting anything
( printf 'GET /met' ; sleep 1 ) | nc -q 2 127.0.0.1 9145 > /dev/null 2>&1 || true
[ "$(curl -s http://127.0.0.1:9145/healthz)" = "ok" ] && pass "partial request" || fail "partial request"

# a big exposition goes out whole
i=0
while [ $i -lt 2000 ]; do printf 'url = "http://127.0.0.1:18070/r%s/x"\noutput = /dev/null\n' "$(echo $i | tr 0-9 a-j)"; i=$((i + 1)); done > "$PREFIX/urls"
curl -s -K "$PREFIX/urls" || true
n=$(curl -s http://127.0.0.1:9145/metrics | grep -c '^nginx_http_requests_total' || true)
[ "$n" -ge 2000 ] && pass "large scrape ($n series)" || fail "large scrape: $n series"

# a reload keeps the listener and the counts
"$NGINX" -p "$PREFIX" -c conf/nginx.conf -s reload
sleep 1
curl -s http://127.0.0.1:9145/metrics | grep -qF 'route="/orders/$param"' && pass "reload" || fail "reload"
stop

# a `prometheus;` location: no listener of its own
conf 'http { server { listen 127.0.0.1:18070; location = /m { prometheus; } } }'
start
[ "$(code http://127.0.0.1:9145/metrics)" = 000 ] && pass "own location: no :9145" || fail "own location: :9145 still open"
[ "$(code http://127.0.0.1:18070/m)" = 200 ] && pass "own location serves" || fail "own location"
stop

# a server already on 9145: nginx starts, the module stays out of the way
conf 'http { server { listen 127.0.0.1:9145; location / { return 200 "mine\n"; } } }'
start
[ "$(curl -s http://127.0.0.1:9145/metrics)" = "mine" ] && pass "port taken: the server keeps it" || fail "port taken"
grep -q 'a server already listens on port 9145' "$PREFIX/logs/error.log" && pass "port taken: warned" || fail "port taken: no warning"
stop

# prometheus_listen and prometheus_zone off
conf 'http { prometheus_listen 127.0.0.1:18071; server { listen 127.0.0.1:18070; } }'
start
[ "$(code http://127.0.0.1:18071/metrics)" = 200 ] && pass "prometheus_listen" || fail "prometheus_listen"
stop
conf 'http { prometheus_zone off; server { listen 127.0.0.1:18070; location / { return 200; } } }'
start
[ "$(code http://127.0.0.1:9145/metrics)" = 000 ] && pass "zone off: nothing" || fail "zone off"
[ "$(code http://127.0.0.1:18070/)" = 200 ] && pass "zone off: nginx works" || fail "zone off: nginx"
stop

# no http{} at all
conf ''
start
[ -f "$PREFIX/nginx.pid" ] && pass "no http{}: starts" || fail "no http{}"
stop

if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$PREFIX/logs/error.log"; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

echo
[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
