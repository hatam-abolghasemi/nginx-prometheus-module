#!/bin/sh
# Integration tests for the image's sidecar script
# (docker/40-prometheus-sidecar.sh), without Docker: each case runs in its own
# mount and network namespace with the image's layout (/etc/nginx with the
# stock nginx.conf plus the Dockerfile's three lines), a DNS server that
# answers 127.0.0.1 for every name (tests/fake_a_dns.py), an app on :8080, and
# origins for any name on 127.0.0.1:8081 (HTTP) and :8443 (HTTPS, from a
# "public" CA that is the system trust store in there).
#
#   sudo env "ASAN_OPTIONS=$ASAN_OPTIONS" "UBSAN_OPTIONS=$UBSAN_OPTIONS" sh tests/sidecar_test.sh <nginx> <modules dir>
#
# Needs root (unshare -mn). Cases: the stock image (no APP_PORT); the sidecar
# with outbound and STREAM; on nginx 1.31+, https interception, then with
# INTERCEPT_BYPASS, custom terminator ports, gRPC, and a CA rotated in place
# (INTERCEPT_CA_RELOAD).

set -u

if [ "${1:-}" = --case ]; then
    shift
    NGINX=$1; MODULES=$2; CASE=$3; shift 3
else
    NGINX=${1:?usage: $0 <nginx> <modules dir>}
    MODULES=${2:?usage: $0 <nginx> <modules dir>}
    case $NGINX in /*) ;; *) NGINX=$(pwd)/$NGINX ;; esac
    case $MODULES in /*) ;; *) MODULES=$(pwd)/$MODULES ;; esac
    SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

    [ "$(id -u)" -eq 0 ] || { echo "skip  sidecar: needs root (sudo)"; exit 0; }

    version=$("$NGINX" -v 2>&1 | sed -n 's#^nginx version: nginx/##p')
    minor=$(echo "$version" | cut -d. -f2)
    cases="stock sidecar"
    [ "$(echo "$version" | cut -d. -f1)" -gt 1 ] || [ "$minor" -ge 31 ] \
        && cases="$cases intercept intercept-more"

    total=0
    for c in $cases; do
        timeout -k 5 120 unshare -mn sh "$SELF" --case "$NGINX" "$MODULES" "$c" < /dev/null
        rc=$?
        [ "$rc" -eq 0 ] || total=$((total + 1))
    done

    echo
    [ "$total" -eq 0 ] && echo "sidecar: all passed" || echo "sidecar: $total case(s) failed"
    exit "$total"
fi

# ---- one case, inside its own namespaces ----

HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/../docker/40-prometheus-sidecar.sh
W=$(mktemp -d)
chmod 755 "$W"
fails=0
pids=""
pass() { printf 'ok    [%s] %s\n' "$CASE" "$1"; }
fail() { printf 'FAIL  [%s] %s\n' "$CASE" "$1"; fails=$((fails + 1)); }
has()  { if printf '%s\n' "$metrics" | grep -qF -- "$2"; then pass "$1"; else fail "$1: no line with: $2"; fi; }
hasnt(){ if printf '%s\n' "$metrics" | grep -qF -- "$2"; then fail "$1: has: $2"; else pass "$1"; fi; }
is()   { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: got \"$2\", want \"$3\""; fi; }

# loopback up
python3 -c '
import socket, fcntl, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
flags = struct.unpack("16sH", fcntl.ioctl(s, 0x8913, struct.pack("16sH", b"lo", 0)))[1]
fcntl.ioctl(s, 0x8914, struct.pack("16sH", b"lo", flags | 1))'

# the image: /etc/nginx as the official image has it, plus the Dockerfile's
# lines; `nginx` on PATH runs with that prefix and config
mkdir -p /etc/nginx
mount -t tmpfs tmpfs /etc/nginx
mkdir -p /etc/nginx/conf.d /etc/nginx/main.d /etc/nginx/html "$W/bin" "$W/logs"
ln -s "$MODULES" /etc/nginx/modules
echo hello > /etc/nginx/html/index.html
cat > /etc/nginx/nginx.conf <<EOF
user  nobody $(id -gn nobody);
worker_processes  2;
error_log  $W/logs/error.log notice;
pid        $W/nginx.pid;
events { worker_connections 1024; }
http {
    default_type  application/octet-stream;
    access_log    off;
    keepalive_timeout  65;
    include /etc/nginx/conf.d/*.conf;
}
EOF
cat > /etc/nginx/conf.d/default.conf <<'EOF'
server {
    listen       80;
    server_name  localhost;
    location / { root /etc/nginx/html; index index.html; }
}
EOF
sed -i -e '1i load_module modules/ngx_http_prometheus_module.so;' \
       -e '1i load_module modules/ngx_stream_prometheus_module.so;' \
       -e '1i include /etc/nginx/main.d/*.conf;' /etc/nginx/nginx.conf
printf '#!/bin/sh\nexec %s -p /etc/nginx/ -c /etc/nginx/nginx.conf -e %s "$@"\n' \
    "$NGINX" "$W/logs/error.log" > "$W/bin/nginx"
chmod +x "$W/bin/nginx"
export PATH="$W/bin:$PATH"

# DNS, the trust store, the origins, the app
printf 'nameserver 127.0.0.1\n' > "$W/resolv.conf"
mount --bind "$W/resolv.conf" /etc/resolv.conf
python3 "$HERE/fake_a_dns.py" 53 &
pids="$pids $!"

ec() { openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes "$@" 2> /dev/null; }
ec -x509 -days 1 -subj /CN=public-ca -keyout "$W/pca.key" -out "$W/pca.crt"
ec -subj /CN=origin -keyout "$W/o.key" -out "$W/o.csr"
printf 'subjectAltName=DNS:example.com,DNS:pinned.test,DNS:grpc.test\n' > "$W/ext"
openssl x509 -req -in "$W/o.csr" -CA "$W/pca.crt" -CAkey "$W/pca.key" \
    -CAcreateserial -days 1 -extfile "$W/ext" -out "$W/o.crt" 2> /dev/null
for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt; do
    [ -e "$f" ] && mount --bind "$W/pca.crt" "$f"
done
[ -e /etc/ssl/certs/ca-certificates.crt ] || { mkdir -p /etc/ssl/certs; mount -t tmpfs tmpfs /etc/ssl/certs; cp "$W/pca.crt" /etc/ssl/certs/ca-certificates.crt; }

mkdir -p "$W/o/logs"
cat > "$W/o/nginx.conf" <<EOF
pid $W/o/nginx.pid;
error_log $W/o/logs/error.log;
events {}
http {
    access_log off;
    server { listen 127.0.0.1:8081; return 200 "origin \$host\n"; }
    server {
        listen 127.0.0.1:8443 ssl;
        http2 on;
        ssl_certificate $W/o.crt;
        ssl_certificate_key $W/o.key;
        return 200 "origin-tls \$host\n";
    }
}
EOF
"$NGINX" -p "$W/o" -c "$W/o/nginx.conf" -e "$W/o/logs/error.log"

mkdir -p "$W/www/orders" && echo app-ok > "$W/www/orders/42"
python3 -m http.server 8080 --bind 127.0.0.1 --directory "$W/www" > /dev/null 2>&1 &
pids="$pids $!"

if python3 -c 'import grpc' 2> /dev/null; then
    python3 "$HERE/grpc_echo.py" server 18443 18080 "$W/o.crt" "$W/o.key" &
    pids="$pids $!"
    grpc=true
else
    grpc=false
fi

case $CASE in intercept*)
    mkdir -p /etc/nginx/intercept
    ec -x509 -days 1 -subj /CN=intercept-ca -keyout /etc/nginx/intercept/ca.key \
        -out /etc/nginx/intercept/ca.crt
    chmod 644 /etc/nginx/intercept/ca.key
esac

# the entrypoint, with this case's environment
case $CASE in
    stock)          set -- "PROMETHEUS_CONFIG=prometheus_bogus on;" ;;
    sidecar)        set -- APP_PORT=8080 STREAM=web:18081:127.0.0.1:8080 \
                           "PROMETHEUS_CONFIG=prometheus_label client_subnet on;" \
                           "PROMETHEUS_STREAM_CONFIG=prometheus_label source_service on;" ;;
    intercept)      set -- APP_PORT=8080 INTERCEPT_TLS=on ;;
    intercept-more) set -- APP_PORT=8080 INTERCEPT_TLS=on INTERCEPT_BYPASS=Pinned.test,.other.test \
                           INTERCEPT_TLS_PORT=4129 INTERCEPT_H2C_PORT=4130 INTERCEPT_CA_RELOAD=1 ;;
esac

(
    for kv in "$@"; do export "$kv"; done
    sh "$SCRIPT"
) > "$W/entry.log" 2>&1 || fail "the sidecar script: $(cat "$W/entry.log")"

nginx -t -q 2> "$W/t.log" || fail "nginx -t: $(cat "$W/t.log")"
nginx
sleep 1

p()  { curl -sS -m 10 --noproxy '*' "$@"; }
px() { curl -sS -m 10 --noproxy '' -x http://127.0.0.1:3128 "$@"; }

case $CASE in
stock)
    p -o /dev/null http://127.0.0.1/
    p -o /dev/null http://127.0.0.1/orders/8231 2> /dev/null || true
    is "healthz" "$(p http://127.0.0.1:9145/healthz)" ok
    grep -q 'PROMETHEUS_CONFIG / PROMETHEUS_STREAM_CONFIG left out, nginx -t: .*prometheus_bogus' "$W/entry.log" \
        && pass "a bad PROMETHEUS_CONFIG is left out, nginx still starts" || fail "bad config: $(cat "$W/entry.log")"
    metrics=$(p http://127.0.0.1:9145/metrics)
    has "the stock server is recorded" 'route="/",status="200"'
    has "a 404, its route sanitized" 'route="/orders/$param",status="404"'
    has "no errors" 'nginx_metric_errors_total 0'
    hasnt "histograms are off" '_histogram_bucket'
    hasnt "the metrics server isn't recorded" 'route="/metrics"'
    ;;

sidecar)
    is "inbound to the app" "$(p http://127.0.0.1/orders/42)" app-ok
    is "through stream" "$(p http://127.0.0.1:18081/orders/42)" app-ok
    grep -q 'sidecar for 127.0.0.1:8080' "$W/entry.log" && pass "says what it does" || fail "start line"
    is "outbound http" "$(px http://example.com:8081/)" "origin example.com"
    tunnel=false
    if grep 'outbound on 127.0.0.1:3128' "$W/entry.log" | grep -qv 'HTTP only'; then
        tunnel=true
        is "outbound CONNECT" "$(px --cacert "$W/pca.crt" https://example.com:8443/)" "origin-tls example.com"
    fi
    metrics=$(p http://127.0.0.1:9145/metrics)
    has "inbound route" 'route="/orders/$param",status="200"'
    has "PROMETHEUS_CONFIG applied" 'client_subnet="127.0.0.0"'
    has "PROMETHEUS_STREAM_CONFIG applied" 'status="200",source_service="unknown"}'
    has "stream" 'nginx_stream_connections_total{destination="web",upstream_addr="127.0.0.1:8080",status="200",source_service="unknown"}'
    has "outbound recorded" 'method="GET",route="/",status="200",client_subnet="127.0.0.0",host="example.com",mode="1"'
    if $tunnel; then
        has "CONNECT recorded" 'method="CONNECT",route="example.com:8443",status="200",client_subnet="127.0.0.0",host="example.com",mode="1",protocol="tunnel"'
    fi
    has "no errors" 'nginx_metric_errors_total 0'
    ;;

intercept)
    ca=/etc/nginx/intercept/ca.crt
    grep -q 'with https interception' "$W/entry.log" && pass "says interception is on" || fail "start line: $(cat "$W/entry.log")"
    is "https decrypted, sent on verified" "$(px --cacert "$ca" https://example.com:8443/a/1)" "origin-tls example.com"
    if $grpc; then
        g() { no_proxy= NO_PROXY= python3 "$HERE/grpc_echo.py" client "$@"; }
        is "grpcs" "$(g grpc.test:18443 http://127.0.0.1:3128 Say "$ca")" "0 hi"
        is "grpcs, an error after data" "$(g grpc.test:18443 http://127.0.0.1:3128 Broken "$ca")" "14 went away"
        is "plaintext gRPC" "$(g grpc.test:18080 http://127.0.0.1:3128 Missing)" "5 no such thing"
    fi
    metrics=$(p http://127.0.0.1:9145/metrics)
    has "a decrypted request" 'method="GET",route="/a/$param",status="200",host="example.com",mode="1",protocol="https"'
    hasnt "not as a CONNECT" 'route="example.com:8443"'
    if $grpc; then
        has "grpcs recorded" 'route="/test.Echo/Say",status="grpc_0",host="grpc.test",mode="1",protocol="grpcs"'
        has "grpcs error from trailers" 'route="/test.Echo/Broken",status="grpc_14",host="grpc.test",mode="1",protocol="grpcs"'
        has "h2c gRPC recorded" 'route="/test.Echo/Missing",status="grpc_5",host="grpc.test",mode="1",protocol="grpc"'
    else
        echo "skip  [$CASE] gRPC: python3 has no grpcio"
    fi
    has "no errors" 'nginx_metric_errors_total 0'
    ;;

intercept-more)
    ca=/etc/nginx/intercept/ca.crt
    grep -q 'bypassed: pinned.test .other.test' "$W/entry.log" && pass "says what is bypassed" || fail "start line: $(cat "$W/entry.log")"
    grep -q 'listen      127.0.0.1:4129 ssl proxy_protocol' /etc/nginx/conf.d/sidecar.conf \
        && grep -q 'tls=127.0.0.1:4129 h2c=127.0.0.1:4130' /etc/nginx/conf.d/sidecar.conf \
        && pass "terminators on INTERCEPT_TLS_PORT / _H2C_PORT" || fail "terminator ports"
    is "decrypted through the custom ports" "$(px --cacert "$ca" https://example.com:8443/)" "origin-tls example.com"
    is "a bypassed host keeps its own certificate" "$(px --cacert "$W/pca.crt" https://pinned.test:8443/)" "origin-tls pinned.test"
    metrics=$(p http://127.0.0.1:9145/metrics)
    has "bypassed: a CONNECT" 'method="CONNECT",route="pinned.test:8443",status="200",host="pinned.test",mode="1",protocol="tunnel"'
    has "the rest: decrypted" 'method="GET",route="/",status="200",host="example.com",mode="1",protocol="https"'

    # a CA rotated in place, as a Secret update does
    cp "$ca" "$W/old-ca.crt"
    ec -x509 -days 1 -subj /CN=rotated-ca -keyout "$W/new.key" -out "$W/new.crt"
    chmod 644 "$W/new.key"
    cp "$W/new.key" /etc/nginx/intercept/ca.key
    cp "$W/new.crt" "$ca"
    sleep 4
    grep -q 'CA changed; nginx reloaded' "$W/entry.log" && pass "the watcher reloads" || fail "no reload: $(cat "$W/entry.log")"
    is "the new CA signs" "$(px --cacert "$ca" https://example.com:8443/r)" "origin-tls example.com"
    if px --cacert "$W/old-ca.crt" https://example.com:8443/o > /dev/null 2>&1; then
        fail "the old CA still signs"
    else
        pass "the old CA no longer signs"
    fi
    ;;
esac

# nginx core (http/2) trips UBSan by itself: not counted
if grep -E '\[(emerg|alert|crit)\]|AddressSanitizer|runtime error' "$W/logs/error.log" \
   | grep -v 'prometheus-env.conf' \
   | grep -v 'src/http/v2/[a-z_0-9]*\.c:[0-9]*:[0-9]*: runtime error\|src/core/ngx_string.c:[0-9]*:[0-9]*: runtime error'; then
    fail "error log is clean"
else
    pass "error log is clean"
fi

nginx -s stop 2> /dev/null
kill "$(cat "$W/o/nginx.pid")" $pids 2> /dev/null
sleep 0.3
pkill -f "$SCRIPT" 2> /dev/null    # the CA watcher
pkill -f "$W" 2> /dev/null
for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/certs; do
    umount "$f" 2> /dev/null
done
[ -n "${KEEP:-}" ] || rm -rf "$W"
exit "$fails"
