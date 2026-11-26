#!/bin/sh
# Integration tests for ngx_http_prometheus_module against a real nginx.
#
#   tests/module_test.sh <nginx binary> <ngx_http_prometheus_module.so>
#
# Starts nginx on 127.0.0.1:18080 (traffic), :18081 (an upstream) and :19145
# (metrics) in a temporary prefix, then checks what /metrics reports: labels,
# route sanitizing, gauges and their expiry, histograms, upstream timing,
# reloads, a full zone, and that the exposition parses.

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
hasnt(){ if printf '%s\n' "$metrics" | grep -qF -- "$2"; then fail "$1: unexpected: $2"; else pass "$1"; fi; }
value(){ printf '%s\n' "$metrics" | grep -v '^#' | grep -F -- "$1" | head -n1 | awk '{print $NF}'; }

conf() {
    # $1: extra http{} directives
    cat > "$PREFIX/conf/nginx.conf" <<EOF
load_module $MODULE;
worker_processes 2;
pid $PREFIX/nginx.pid;
error_log $PREFIX/logs/error.log warn;
events { worker_connections 128; }
http {
    access_log off;
    $1
    upstream app { server 127.0.0.1:18081; keepalive 4; }
    server {
        listen 127.0.0.1:18081;
        location / { return 200 "app\n"; }
    }
    server {
        listen 127.0.0.1:18080;
        prometheus_record on;
        location / { proxy_pass http://app; proxy_http_version 1.1; proxy_set_header Connection ""; }
        location /static/ { return 404; }
        location /quiet/ { prometheus_record off; return 204; }
        location /out/ { prometheus_mode 1; return 200; }
    }
    server {
        listen 127.0.0.1:19145;
        prometheus_record off;
        location = /metrics { prometheus; }
    }
}
EOF
}

get()    { curl -s -o /dev/null "$@" || true; }
scrape() { metrics=$(curl -fsS http://127.0.0.1:19145/metrics); }

conf "prometheus_zone 1m;
    prometheus_gauge_expiry 2s;
    prometheus_cluster_ip_prefix 10.233.;
    prometheus_metric request_duration_histogram on;
    prometheus_metric upstream_response_time_histogram on;
    prometheus_metric upstream_connect_time on;
    prometheus_buckets request_duration_histogram 0.005 0.5 5;"

"$NGINX" -p "$PREFIX" -c conf/nginx.conf -t -q
"$NGINX" -p "$PREFIX" -c conf/nginx.conf
sleep 0.5

get http://127.0.0.1:18080/orders/8231
get http://127.0.0.1:18080/orders/8231
get 'http://127.0.0.1:18080/orders/550e8400-e29b-41d4-a716-446655440000/items?x=1'
get http://127.0.0.1:18080/static/app.js
get http://127.0.0.1:18080/quiet/x
get http://127.0.0.1:18080/out/x
get -H 'Host: 10.233.1.2' http://127.0.0.1:18080/h
get -H 'Host: 8.8.8.8' http://127.0.0.1:18080/h
get -H 'Host: api.example.com' http://127.0.0.1:18080/h
get -X POST -H 'Content-Type: application/grpc' --data-binary x http://127.0.0.1:18080/pkg.Svc/Call
scrape

L='method="GET",route="/orders/$param",status="200",host="localhost",mode="0",protocol="http"'
has "counter with every label"     "nginx_http_requests_total{$L} 2"
has "uuid and query sanitized"     'route="/orders/$param/items"'
has "status from return"           'route="/static/$param",status="404"'
has "prometheus_record off"        'nginx_metric_errors_total 0'
hasnt "location not recorded"      'route="/quiet'
has "prometheus_mode"              'route="/out/x",status="200",host="localhost",mode="1"'
has "host: cluster ip"             'host="kubernetes"'
has "host: other ip"               'host="$ip"'
has "host: name"                   'host="api.example.com"'
has "protocol: grpc"               'protocol="grpc"'
has "duration gauge"               "nginx_http_request_duration_seconds{$L}"
has "request size gauge"           "nginx_http_request_size_bytes{$L}"
has "response size gauge"          "nginx_http_response_size_bytes{$L}"
has "upstream response gauge"      "nginx_http_upstream_response_time_seconds{$L}"
has "upstream connect gauge"       "nginx_http_upstream_connect_time_seconds{$L}"
hasnt "upstream header gauge off"  "nginx_http_upstream_header_time_seconds{"
hasnt "no upstream, no upstream gauge" 'nginx_http_upstream_response_time_seconds{method="GET",route="/static'
has "histogram custom buckets"     "nginx_http_request_duration_seconds_histogram_bucket{$L,le=\"0.005\"}"
has "histogram +Inf"               "nginx_http_request_duration_seconds_histogram_bucket{$L,le=\"+Inf\"} 2"
has "histogram count"              "nginx_http_request_duration_seconds_histogram_count{$L} 2"
has "upstream histogram"           "nginx_http_upstream_response_time_seconds_histogram_count{$L} 2"
hasnt "size histogram off"         "nginx_http_request_size_bytes_histogram"
has "connections"                  'nginx_http_connections{state="active"}'
has "connection requests"          'nginx_connection_requests '
has "connection time"              'nginx_connection_time_seconds '
has "slots limit"                  'nginx_connection_slots_limit 256'
hasnt "metrics location not recorded" 'route="/metrics"'

# promtool exits 1 when the exposition doesn't parse and 3 for lint findings;
# the names are the sidecar's (e.g. *_histogram), kept for its dashboard and
# queries, so only a parse failure fails the test
if command -v promtool > /dev/null 2>&1; then
    rc=0
    printf '%s\n' "$metrics" | promtool check metrics || rc=$?
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 3 ]; then pass "promtool parses it"; else fail "promtool: exit $rc"; fi
fi

# gauges expire, counters stay
sleep 3
scrape
hasnt "gauge expired"              "nginx_http_request_duration_seconds{$L}"
hasnt "upstream gauge expired"     "nginx_http_upstream_response_time_seconds{$L}"
has "counter kept after expiry"    "nginx_http_requests_total{$L} 2"
has "histogram kept after expiry"  "nginx_http_request_duration_seconds_histogram_count{$L} 2"

# a reload with the same metrics keeps the counts
"$NGINX" -p "$PREFIX" -c conf/nginx.conf -s reload
sleep 1
scrape
has "counts survive a reload"      "nginx_http_requests_total{$L} 2"

# a reload with a different series layout starts over
conf "prometheus_zone 1m;
    prometheus_gauge_expiry 2s;
    prometheus_metric request_size_histogram on;"
"$NGINX" -p "$PREFIX" -c conf/nginx.conf -s reload
sleep 1
get http://127.0.0.1:18080/orders/1
scrape
L2='method="GET",route="/orders/$param",status="200",host="localhost",mode="0",protocol="http"'
has "layout change resets"         "nginx_http_requests_total{$L2} 1"
has "new histogram on"             "nginx_http_request_size_bytes_histogram_count{$L2} 1"
hasnt "old histogram off"          "nginx_http_request_duration_seconds_histogram"

# a full zone drops new label sets and counts them, nothing else breaks
# (letters only, so the route sanitizer keeps every route distinct)
i=0
while [ $i -lt 3000 ]; do
    printf 'url = "http://127.0.0.1:18080/r%s/x"\noutput = /dev/null\n' "$(echo $i | tr 0-9 a-j)"
    i=$((i + 1))
done > "$PREFIX/urls"
curl -s -K "$PREFIX/urls" || true
scrape
errors=$(value 'nginx_metric_errors_total ')
if [ "${errors:-0}" -gt 0 ]; then pass "full zone counted ($errors)"; else fail "full zone not counted"; fi
has "still recording known series" "nginx_http_requests_total{$L2}"

# HEAD and a rejected method on /metrics
code=$(curl -s -o /dev/null -w '%{http_code}' -I http://127.0.0.1:19145/metrics)
[ "$code" = 200 ] && pass "HEAD /metrics" || fail "HEAD /metrics: $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:19145/metrics)
[ "$code" = 405 ] && pass "POST /metrics is 405" || fail "POST /metrics: $code"
ct=$(curl -s -o /dev/null -w '%{content_type}' http://127.0.0.1:19145/metrics)
case $ct in "text/plain; version=0.0.4"*) pass "content type" ;; *) fail "content type: $ct" ;; esac

if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$PREFIX/logs/error.log"; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

echo
[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
