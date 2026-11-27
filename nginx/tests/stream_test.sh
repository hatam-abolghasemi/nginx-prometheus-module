#!/bin/sh
# Integration tests for ngx_stream_prometheus_module: TCP sessions recorded
# by destination, upstream and status, served on the http module's /metrics;
# then the caller labels (source_service / source_namespace), resolved by the
# http module: PTR answers from tests/fake_dns.c, callers faked with loopback
# addresses (curl --interface / nc -s 127.0.0.x).
#
#   tests/stream_test.sh <nginx binary> <modules dir>
#
# <modules dir> holds ngx_http_prometheus_module.so and
# ngx_stream_prometheus_module.so. Ports: 18050 and 18060 (backends), 18051,
# 18052, 18061 and 18062 (stream listeners), 19151 and 19152 (metrics),
# 15354 (DNS).

set -eu

NGINX=${1:?usage: $0 <nginx> <modules dir>}
MODULES=${2:?usage: $0 <nginx> <modules dir>}
case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
case $MODULES in /*) ;; *) MODULES=$(pwd)/$MODULES ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)

PREFIX=$(mktemp -d)
P2=$(mktemp -d)
cleanup() {
    kill "$(cat "$PREFIX/nginx.pid" 2>/dev/null)" 2>/dev/null || true
    kill "$(cat "$P2/nginx.pid" 2>/dev/null)" 2>/dev/null || true
    [ -n "${DNS_PID:-}" ] && kill "$DNS_PID" 2>/dev/null || true
    [ -n "${KEEP:-}" ] || rm -rf "$PREFIX" "$P2"
}
trap cleanup EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf" "$P2/logs" "$P2/conf"

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

# --- who opened the session: source_service / source_namespace ---

cc -O1 -o "$P2/fake_dns" "$HERE/fake_dns.c"
cat > "$P2/ptr" <<'EOT'
127.0.0.11 127-0-0-11.checkout-metrics.shop.svc.cluster.local 127-0-0-11.checkout.shop.svc.cluster.local
127.0.0.12 NXDOMAIN
EOT
"$P2/fake_dns" 15354 "$P2/ptr" &
DNS_PID=$!

conf2() {
    cat > "$P2/conf/nginx.conf" <<EOF
load_module $MODULES/ngx_http_prometheus_module.so;
load_module $MODULES/ngx_stream_prometheus_module.so;
worker_processes 2;
pid $P2/nginx.pid;
error_log $P2/logs/error.log notice;
events { worker_connections 128; }
http {
    access_log off;
    prometheus_listen 127.0.0.1:19152;
    prometheus_label source_service on;
    prometheus_label source_namespace on;
    prometheus_identity_fallback myapp myns;
    prometheus_source_ptr 127.0.0.8/29;
    prometheus_source_static 127.0.1.0/24 internal zone_a;
    prometheus_dns 127.0.0.1:15354;
    server { listen 127.0.0.1:18060; location / { return 200 "backend\n"; } }
}
stream {
    prometheus_label source_service $1;
    prometheus_label source_namespace $1;
    server {
        listen 127.0.0.1:18061;
        prometheus_destination backend;
        proxy_pass 127.0.0.1:18060;
    }
    server {
        listen 127.0.0.1:18062;
        prometheus_destination warm;
        proxy_pass 127.0.0.1:18060;
    }
}
EOF
}

conf2 on
"$NGINX" -p "$P2" -c conf/nginx.conf -t -q
"$NGINX" -p "$P2" -c conf/nginx.conf
sleep 0.5

s2() { curl -s -o /dev/null --interface "$1" "http://127.0.0.1:$2/" || true; }

# the first session from an address starts its lookup
s2 127.0.0.11 18062
s2 127.0.0.12 18062
sleep 0.5

s2 127.0.0.11 18061
s2 127.0.0.11 18061
s2 127.0.0.12 18061
s2 127.0.1.5  18061
s2 127.0.0.1  18061

# open sessions, held for 2s
sleep 2 | nc -q 1 -s 127.0.0.11 127.0.0.1 18061 > /dev/null 2>&1 &
NC1=$!
sleep 2 | nc -q 1 -s 127.0.0.1 127.0.0.1 18061 > /dev/null 2>&1 &
NC2=$!
sleep 0.5
metrics=$(curl -s http://127.0.0.1:19152/metrics)

B='destination="backend",upstream_addr="127.0.0.1:18060",status="200"'
has "PTR caller, -metrics passed over" "nginx_stream_connections_total{$B,source_service=\"checkout\",source_namespace=\"shop\"} 2"
has "NXDOMAIN: unresolved"          "nginx_stream_connections_total{$B,source_service=\"health-probe\",source_namespace=\"kubernetes\"} 1"
has "static range"                  "nginx_stream_connections_total{$B,source_service=\"internal\",source_namespace=\"zone_a\"} 1"
has "loopback: this pod"            "nginx_stream_connections_total{$B,source_service=\"myapp\",source_namespace=\"myns\"} 1"
has "source labels on every metric" "nginx_stream_upstream_bytes_received_total{$B,source_service=\"checkout\",source_namespace=\"shop\"}"
has "active by caller (PTR)"        'nginx_stream_connections_active{state="active",source_service="checkout",source_namespace="shop"} 1'
has "active by caller (self)"       'nginx_stream_connections_active{state="active",source_service="myapp",source_namespace="myns"} 1'
hasnt "no caller-less active line"  'nginx_stream_connections_active{state="active"} '

if command -v promtool > /dev/null 2>&1; then
    rc=0
    printf '%s\n' "$metrics" | promtool check metrics || rc=$?
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 3 ]; then pass "promtool parses it (callers)"; else fail "promtool: exit $rc"; fi
fi

wait "$NC1" "$NC2" || true
sleep 0.5
metrics=$(curl -s http://127.0.0.1:19152/metrics)
has "closed: back to 0"             'nginx_stream_connections_active{state="active",source_service="checkout",source_namespace="shop"} 0'

# a reload that turns the labels off while a session is open: it ends in the
# old worker and must not drive the new, caller-less gauge below zero
sleep 3 | nc -q 1 -s 127.0.0.11 127.0.0.1 18061 > /dev/null 2>&1 &
NC1=$!
sleep 0.5
conf2 off
"$NGINX" -p "$P2" -c conf/nginx.conf -s reload
sleep 1
s2 127.0.0.11 18061
wait "$NC1" || true
sleep 1
metrics=$(curl -s http://127.0.0.1:19152/metrics)
has "labels off after reload"       'nginx_stream_connections_active{state="active"} 0'
has "plain series again"            "nginx_stream_connections_total{$B} 1"
# (the held session itself is counted by the old worker, with its labels)
has "the old worker's session"      "nginx_stream_connections_total{$B,source_service=\"checkout\",source_namespace=\"shop\"} 1"

if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$P2/logs/error.log"; then
    fail "error log is clean (callers)"
else
    pass "error log is clean (callers)"
fi

echo
[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
