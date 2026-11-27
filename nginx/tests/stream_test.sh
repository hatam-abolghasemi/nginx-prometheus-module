#!/bin/sh
# Integration tests for ngx_stream_prometheus_module: TCP sessions recorded
# by destination, upstream and status, served on the http module's /metrics.
#
#   tests/stream_test.sh <nginx binary> <modules dir>
#
# <modules dir> holds ngx_http_prometheus_module.so and
# ngx_stream_prometheus_module.so. Ports: 18050 (a backend), 18051 and 18052
# (stream listeners), 19151 (metrics).

set -eu

NGINX=${1:?usage: $0 <nginx> <modules dir>}
MODULES=${2:?usage: $0 <nginx> <modules dir>}
case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
case $MODULES in /*) ;; *) MODULES=$(pwd)/$MODULES ;; esac

PREFIX=$(mktemp -d)
trap 'kill "$(cat "$PREFIX/nginx.pid" 2>/dev/null)" 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$PREFIX"' EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf"

fails=0
pass() { printf 'ok    %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }
has()  { if printf '%s\n' "$metrics" | grep -qF -- "$2"; then pass "$1"; else fail "$1: no line with: $2"; fi; }
hasnt(){ if printf '%s\n' "$metrics" | grep -qF -- "$2"; then fail "$1: unexpected: $2"; else pass "$1"; fi; }

cat > "$PREFIX/conf/nginx.conf" <<EOF
load_module $MODULES/ngx_http_prometheus_module.so;
load_module $MODULES/ngx_stream_prometheus_module.so;
pid $PREFIX/nginx.pid;
error_log $PREFIX/logs/error.log warn;
events { worker_connections 128; }
http {
    access_log off;
    prometheus_listen 127.0.0.1:19151;
    server { listen 127.0.0.1:18050; location / { return 200 "backend\n"; } }
}
stream {
    prometheus_gauge_expiry 2s;
    prometheus_metric session_duration_histogram on;
    prometheus_metric upstream_connect_time_histogram on;
    server {
        listen 127.0.0.1:18051;
        prometheus_destination backend;
        proxy_pass 127.0.0.1:18050;
    }
    server {
        listen 127.0.0.1:18052;
        prometheus_destination nowhere;
        proxy_pass 127.0.0.1:1;
    }
}
EOF

"$NGINX" -p "$PREFIX" -c conf/nginx.conf -t -q
"$NGINX" -p "$PREFIX" -c conf/nginx.conf
sleep 0.5

[ "$(curl -s http://127.0.0.1:18051/x)" = backend ] && pass "proxied through stream" || fail "stream proxy"
curl -s -o /dev/null http://127.0.0.1:18051/y || true
curl -s -o /dev/null http://127.0.0.1:18052/ || true

# a session that stays open shows as active
# (nothing is sent, so the backend just waits; nc closes 1s after its stdin)
sleep 2 | nc -q 1 127.0.0.1 18051 > /dev/null 2>&1 &
sleep 0.5
metrics=$(curl -s http://127.0.0.1:19151/metrics)
has "an open session is active"     'nginx_stream_connections_active{state="active"} 1'

L='destination="backend",upstream_addr="127.0.0.1:18050",status="200"'
has "connections by destination"    "nginx_stream_connections_total{$L} 2"
has "bytes to the client"           "nginx_stream_bytes_sent_total{$L}"
has "bytes from the client"         "nginx_stream_bytes_received_total{$L}"
has "bytes to the upstream"         "nginx_stream_upstream_bytes_sent_total{$L}"
has "bytes from the upstream"       "nginx_stream_upstream_bytes_received_total{$L}"
has "session gauge"                 "nginx_stream_session_duration_seconds{$L}"
has "connect gauge"                 "nginx_stream_upstream_connect_time_seconds{$L}"
has "session histogram"             "nginx_stream_session_duration_seconds_histogram_count{$L} 2"
has "connect histogram"             "nginx_stream_upstream_connect_time_seconds_histogram_count{$L} 2"
hasnt "first byte histogram off"    "nginx_stream_upstream_first_byte_time_seconds"
has "refused upstream: 502"         'nginx_stream_connections_total{destination="nowhere",upstream_addr="127.0.0.1:1",status="502"} 1'
hasnt "refused: no connect gauge"   'nginx_stream_upstream_connect_time_seconds{destination="nowhere"'
has "http metrics still there"      'nginx_http_connections{state="active"}'
has "one errors counter"            'nginx_metric_errors_total 0'

wait
sleep 3
metrics=$(curl -s http://127.0.0.1:19151/metrics)
has "the session ended"             'nginx_stream_connections_active{state="active"} 0'
has "counted when it ended"         "nginx_stream_connections_total{$L} 3"
hasnt "gauges expire"               "nginx_stream_session_duration_seconds{$L}"

if command -v promtool > /dev/null 2>&1; then
    rc=0
    printf '%s\n' "$metrics" | promtool check metrics || rc=$?
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 3 ]; then pass "promtool parses it"; else fail "promtool: exit $rc"; fi
fi

# a reload keeps the counts
"$NGINX" -p "$PREFIX" -c conf/nginx.conf -s reload
sleep 1
metrics=$(curl -s http://127.0.0.1:19151/metrics)
has "counts survive a reload"       "nginx_stream_connections_total{$L} 3"

if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$PREFIX/logs/error.log"; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

echo
[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
