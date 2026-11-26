#!/bin/sh
# Integration tests for caller and own identity in ngx_http_prometheus_module:
# source_service / source_namespace (PTR, static ranges, the default, kubelet
# probes, the mesh waypoint's XFCC), app / namespace, client_subnet.
#
#   tests/source_test.sh <nginx binary> <ngx_http_prometheus_module.so>
#
# Callers are faked with loopback addresses (curl --interface 127.0.0.x), PTR
# answers come from tests/fake_dns.c on 127.0.0.1:15353. Ports: 18090 and
# 19146.

set -eu

NGINX=${1:?usage: $0 <nginx> <module.so>}
MODULE=${2:?usage: $0 <nginx> <module.so>}
case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
case $MODULE in /*) ;; *) MODULE=$(pwd)/$MODULE ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)

PREFIX=$(mktemp -d)
cleanup() {
    kill "$(cat "$PREFIX/nginx.pid" 2>/dev/null)" 2>/dev/null || true
    [ -n "${DNS_PID:-}" ] && kill "$DNS_PID" 2>/dev/null || true
    [ -n "${KEEP:-}" ] || rm -rf "$PREFIX"
}
trap cleanup EXIT
mkdir -p "$PREFIX/logs" "$PREFIX/conf"

fails=0
pass() { printf 'ok    %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }
has()  { if printf '%s\n' "$metrics" | grep -qF -- "$2"; then pass "$1"; else fail "$1: no line with: $2"; fi; }
hasnt(){ if printf '%s\n' "$metrics" | grep -qF -- "$2"; then fail "$1: unexpected: $2"; else pass "$1"; fi; }

cc -O1 -o "$PREFIX/fake_dns" "$HERE/fake_dns.c"
cat > "$PREFIX/ptr" <<'EOF'
127.0.0.11 127-0-0-11.checkout-metrics.shop.svc.cluster.local 127-0-0-11.checkout.shop.svc.cluster.local
127.0.0.12 127-0-0-12.batch.pod.cluster.local
127.0.0.13 NXDOMAIN
127.0.0.14 127-0-0-14.waypoint.mesh.svc.cluster.local
EOF
"$PREFIX/fake_dns" 15353 "$PREFIX/ptr" &
DNS_PID=$!

cat > "$PREFIX/conf/nginx.conf" <<EOF
load_module $MODULE;
worker_processes 2;
pid $PREFIX/nginx.pid;
error_log $PREFIX/logs/error.log notice;
events { worker_connections 128; }
http {
    access_log off;
    prometheus_zone 1m;
    prometheus_label source_service on;
    prometheus_label source_namespace on;
    prometheus_label client_subnet on;
    prometheus_label app on;
    prometheus_label namespace on;
    prometheus_identity_fallback myapp myns;
    prometheus_source_ptr 127.0.0.0/24;
    prometheus_source_static 127.0.1.0/24 internal zone_a;
    prometheus_source_static 127.0.1.128/25 internal zone_b;
    prometheus_dns 127.0.0.1:15353;
    prometheus_dns_timeout 1s;
    prometheus_source_refresh 1s;
    server {
        listen 18090;
        location / { return 200 "ok\n"; }
        location /out/ { prometheus_mode 1; return 200; }
    }
    server {
        listen 127.0.0.1:19146;
        location = /metrics { prometheus; }
    }
}
EOF

"$NGINX" -p "$PREFIX" -c conf/nginx.conf -t -q
"$NGINX" -p "$PREFIX" -c conf/nginx.conf
sleep 0.5

get() { iface=$1; path=$2; shift 2; curl -s -o /dev/null --interface "$iface" "http://127.0.0.1:18090$path" "$@" || true; }

# a first request starts the lookup; the answer is in for the next ones
for ip in 127.0.0.11 127.0.0.12 127.0.0.13 127.0.0.14; do get "$ip" /warm; done
sleep 0.5
get 127.0.0.11 /a
get 127.0.0.12 /a
get 127.0.0.13 /a
get 127.0.0.14 /a -H 'X-Forwarded-Client-Cert: By=spiffe://cluster.local/ns/mesh/sa/waypoint;URI=spiffe://cluster.local/ns/shop/sa/cart'
get 127.0.0.14 /b
get 127.0.1.5 /a
get 127.0.1.200 /a
get 127.0.0.1 /a -A 'kube-probe/1.30'
get 127.0.0.1 /x -H 'X-Forwarded-For: 8.8.4.4, 10.0.0.1'
get 127.0.0.11 /out/x
metrics=$(curl -s http://127.0.0.1:19146/metrics)

has "PTR service, -metrics passed over" 'route="/a",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="0",source_service="checkout",source_namespace="shop"'
has "PTR pod only: its namespace"      'source_service="health-probe",source_namespace="batch"'
has "NXDOMAIN: unresolved"            'route="/a",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="0",source_service="health-probe",source_namespace="kubernetes"'
has "waypoint + XFCC: the real caller" 'source_service="cart",source_namespace="shop"'
has "waypoint without XFCC stays"     'route="/b",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="0",source_service="waypoint",source_namespace="mesh"'
has "static range"                    'source_service="internal",source_namespace="zone_a"'
has "longest static range wins"       'source_service="internal",source_namespace="zone_b"'
has "kubelet probe"                   'route="/a",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="0",source_service="health-probe",source_namespace="kubernetes"'
has "client_subnet from XFF, public /16" 'route="/x",status="200",client_subnet="8.8.0.0"'
has "outbound: the caller is this pod" 'route="/out/x",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="1",source_service="myapp",source_namespace="myns"'
has "first request is unresolved"     'route="/warm",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="0",source_service="health-probe"'
has "nothing dropped"                 'nginx_metric_errors_total 0'

# a refresh relabels an IP that changed hands
kill "$DNS_PID"
echo '127.0.0.11 127-0-0-11.orders.shop.svc.cluster.local' > "$PREFIX/ptr"
"$PREFIX/fake_dns" 15353 "$PREFIX/ptr" &
DNS_PID=$!
sleep 2.5
get 127.0.0.11 /c
metrics=$(curl -s http://127.0.0.1:19146/metrics)
has "refresh relabels"                'route="/c",status="200",client_subnet="127.0.0.0",host="localhost",app="myapp",namespace="myns",mode="0",source_service="orders",source_namespace="shop"'

if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$PREFIX/logs/error.log"; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

echo
[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
