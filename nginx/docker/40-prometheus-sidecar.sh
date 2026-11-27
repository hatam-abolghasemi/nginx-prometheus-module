#!/bin/sh
# Run by the nginx image's entrypoint (/docker-entrypoint.d/) at each start.
# Without APP_PORT it does nothing but PROMETHEUS_CONFIG: the image is stock
# nginx plus metrics.
#
#   PROMETHEUS_CONFIG="prometheus_metric request_duration_histogram on; prometheus_label client_subnet on;"
#                            module directives for http{}, without mounting a
#                            file (any image, sidecar or not). If nginx -t
#                            rejects them they are left out and logged: a typo
#                            in a metrics setting never stops nginx.
#   PROMETHEUS_STREAM_CONFIG the same for stream{} (with STREAM), e.g.
#                            "prometheus_label source_service on;"
#
# With APP_PORT set, the image becomes a sidecar in front of an app:
#
#   APP_PORT=8080            inbound: :80 forwards to 127.0.0.1:$APP_PORT,
#                            transparently; conf.d/default.conf is removed
#                            (it listens on :80 too). Own identity
#                            (prometheus_identity) is on.
#   POD_CIDR="10.233.0.0/16" caller identity: source_service and
#                            source_namespace on, callers in these ranges
#                            (space separated) resolved by reverse DNS.
#   CLUSTER_IP_PREFIX=10.233.  Host headers that are IPs in it are labelled
#                            "kubernetes" instead of "$ip".
#   OUTBOUND=off             no outbound proxy. On by default: a forward
#                            proxy on 127.0.0.1:3128 for the app's egress
#                            (HTTP_PROXY / HTTPS_PROXY), recorded as mode="1".
#                            HTTPS goes through as CONNECT on nginx 1.31+ (the
#                            tunnel module); on older nginx, plain HTTP only.
#   INTERCEPT_TLS=on         https interception (nginx 1.31+): the CONNECTs
#                            that carry TLS or plaintext gRPC are decoded and
#                            recorded with their real method, route and status.
#                            Needs a CA the app trusts, at
#                            /etc/nginx/intercept/ca.crt and ca.key
#                            (INTERCEPT_CA_CERT / INTERCEPT_CA_KEY).
#                            Destinations are verified against the system
#                            trust store (INTERCEPT_TRUSTED_CA).
#   INTERCEPT_BYPASS="pinned.example.com,.bank.example"
#                            hosts never decrypted (certificate pinning,
#                            mTLS): relayed as CONNECT. ".x" is x and every
#                            name under it, "*.x" every name under it.
#   INTERCEPT_CA_RELOAD=60   seconds between checks of the CA files; nginx is
#                            reloaded when they change (a rotated Secret).
#                            0: never.
#   INTERCEPT_TLS_PORT=3129, INTERCEPT_H2C_PORT=3130
#                            the loopback ports of the two terminators, if the
#                            app needs those.
#   IPV6=on                  outbound and interception resolve IPv6 (AAAA)
#                            too; IPv4 is preferred. Off by default.
#   STREAM="redis:16379:redis.data.svc:6379 postgres:15432:db.data.svc:5432"
#                            stream (L4): for each <name>:<port>:<host>:<port>,
#                            a TCP proxy on 127.0.0.1:<port> to <host>:<port>,
#                            recorded with destination="<name>". Point the
#                            app's client at 127.0.0.1:<port>.
#
# Everything else is plain nginx config: mount more files into conf.d/.
set -eu

extra=/etc/nginx/conf.d/prometheus-env.conf
stream_extra=/etc/nginx/main.d/prometheus-env-stream.include

# $1: the file, $2: its directives, $3: the variable's name. Written as is,
# then dropped again if nginx -t says no.
env_config() {
    if ! { echo "# Generated at start from $3"; printf '%s\n' "$2"; } > "$1" 2>/dev/null; then
        echo "$0: $3 is set but $1 can't be written" >&2
        return 0
    fi
}

check_env_config() {
    [ -s "$extra" ] || [ -s "$stream_extra" ] || return 0

    if ! err=$(nginx -t -q 2>&1); then
        rm -f "$extra"
        : > "$stream_extra" 2> /dev/null || true
        echo "$0: PROMETHEUS_CONFIG / PROMETHEUS_STREAM_CONFIG left out, nginx -t: $err" >&2
    fi
}

rm -f "$extra"
[ -z "${PROMETHEUS_CONFIG:-}" ] || env_config "$extra" "$PROMETHEUS_CONFIG" PROMETHEUS_CONFIG

if [ -z "${APP_PORT:-}" ]; then
    check_env_config
    exit 0
fi

case $APP_PORT in
    ''|*[!0-9]*) echo "$0: APP_PORT must be a port number, got \"$APP_PORT\"" >&2; exit 1 ;;
esac

conf=/etc/nginx/conf.d/sidecar.conf

if ! : > "$conf" 2>/dev/null; then
    echo "$0: APP_PORT is set but $conf can't be written (read-only conf.d?)" >&2
    exit 1
fi

rm -f /etc/nginx/conf.d/default.conf

inbound() {
    echo "# Generated at start by /docker-entrypoint.d/40-prometheus-sidecar.sh"
    echo
    echo "prometheus_identity on;"

    if [ -n "${POD_CIDR:-}" ]; then
        echo "prometheus_label source_service on;"
        echo "prometheus_label source_namespace on;"
        echo "prometheus_source_ptr $POD_CIDR;"
    fi

    if [ -n "${CLUSTER_IP_PREFIX:-}" ]; then
        echo "prometheus_cluster_ip_prefix $CLUSTER_IP_PREFIX;"
    fi

    cat <<EOF

client_max_body_size        0;
underscores_in_headers      on;
ignore_invalid_headers      off;
large_client_header_buffers 4 32k;

map \$http_upgrade \$prometheus_connection_upgrade {
    default  upgrade;
    ""       "";
}

upstream app {
    server 127.0.0.1:$APP_PORT;
    keepalive 64;
}

# inbound: requests reach the app as the client sent them (Host, raw URI,
# bodies of any size, WebSocket upgrades); nothing is added.
server {
    listen       80 default_server;
    server_name  _;

    location / {
        proxy_pass               http://app;
        proxy_http_version       1.1;
        proxy_set_header         Host       \$http_host;
        proxy_set_header         Upgrade    \$http_upgrade;
        proxy_set_header         Connection \$prometheus_connection_upgrade;
        proxy_pass_header        Server;
        proxy_pass_header        Date;
        proxy_redirect           off;
        proxy_buffering          off;
        proxy_request_buffering  off;
        proxy_buffer_size        32k;
        proxy_buffers            4 32k;
        proxy_next_upstream      off;
        proxy_connect_timeout    5s;
        proxy_send_timeout       300s;
        proxy_read_timeout       300s;
    }
}
EOF
}

# a port number, or exit
port_of() {
    case $2 in
        ''|*[!0-9]*) echo "$0: $1 must be a port number, got \"$2\"" >&2; exit 1 ;;
    esac
    if [ "$2" -lt 1 ] || [ "$2" -gt 65535 ]; then
        echo "$0: $1 must be a port number, got \"$2\"" >&2; exit 1
    fi
    echo "$2"
}

# $1: the DNS server; $2: the CA certificate; $3: its key; $4: trusted CAs
intercept() {
    cat <<EOF

# https interception: TLS from a CONNECT is decrypted here with a certificate
# minted for its name and signed by the CA; its requests are recorded and sent
# on over verified TLS, on connections kept per destination name.
prometheus_intercept_ca  $2 $3;

upstream prometheus_intercept_https { server 0.0.0.1; prometheus_intercept_origin 256; }
upstream prometheus_intercept_grpcs { server 0.0.0.1; prometheus_intercept_origin 256; }
upstream prometheus_intercept_grpc  { server 0.0.0.1; prometheus_intercept_origin 256; }
upstream prometheus_intercept_http  { server 0.0.0.1; prometheus_intercept_origin 256; }

server {
    listen      127.0.0.1:$tls_port ssl proxy_protocol;
    http2       on;

    resolver          $1 ipv6=$ipv6 valid=10s;
    resolver_timeout  5s;

    prometheus_mode               1;
    prometheus_intercept_resolve  on;

    ssl_certificate      data:\$prometheus_intercept_cert;
    ssl_certificate_key  data:\$prometheus_intercept_key;
    ssl_protocols        TLSv1.2 TLSv1.3;

    proxy_ssl_server_name          on;
    proxy_ssl_name                 \$host;
    proxy_ssl_verify               on;
    proxy_ssl_verify_depth         3;
    proxy_ssl_trusted_certificate  $4;

    grpc_ssl_server_name           on;
    grpc_ssl_name                  \$host;
    grpc_ssl_verify                on;
    grpc_ssl_verify_depth          3;
    grpc_ssl_trusted_certificate   $4;

    client_max_body_size  0;
EOF
    cat <<'EOF'

    location / {
        # gRPC over TLS (not gRPC-Web): HTTP/2 to the destination
        if ($content_type ~* "^application/grpc($|[+;\s])") {
            grpc_pass  grpcs://prometheus_intercept_grpcs;
        }
        grpc_set_header          Host  $http_host;
        grpc_read_timeout        300s;
        grpc_send_timeout        300s;

        proxy_pass               https://prometheus_intercept_https;
        proxy_http_version       1.1;
        proxy_set_header         Host        $http_host;
        proxy_set_header         Connection  "";
        proxy_pass_header        Server;
        proxy_pass_header        Date;
        proxy_buffering          off;
        proxy_request_buffering  off;
        proxy_read_timeout       300s;
        proxy_send_timeout       300s;
    }
}
EOF
    cat <<EOF

# plaintext gRPC (HTTP/2 with prior knowledge) from a CONNECT
server {
    listen      127.0.0.1:$h2c_port proxy_protocol;
    http2       on;

    resolver          $1 ipv6=$ipv6 valid=10s;
    resolver_timeout  5s;

    prometheus_mode               1;
    prometheus_intercept_resolve  on;
    client_max_body_size          0;
EOF
    cat <<'EOF'

    location / {
        if ($content_type ~* "^application/grpc($|[+;\s])") {
            grpc_pass  grpc://prometheus_intercept_grpc;
        }
        grpc_set_header          Host  $http_host;
        grpc_read_timeout        300s;
        grpc_send_timeout        300s;

        proxy_pass               http://prometheus_intercept_http;
        proxy_http_version       1.1;
        proxy_set_header         Host        $http_host;
        proxy_set_header         Connection  "";
        proxy_buffering          off;
        proxy_request_buffering  off;
    }
}
EOF
}

# $1: the DNS server; $2: non-empty when CONNECT can be relayed; $3: non-empty
# when CONNECTs are intercepted
outbound() {
    if [ -n "$3" ] && [ -n "$bypass" ]; then
        echo
        echo "# INTERCEPT_BYPASS: these hosts are relayed, never decrypted"
        echo "map \$host \$prometheus_intercept_bypass {"
        echo "    hostnames;"
        echo "    default  0;"
        for h in $bypass; do
            echo "    $h  1;"
        done
        echo "}"
        cat <<'EOF'

map "$request_method:$prometheus_intercept_bypass" $prometheus_connect {
    default      "";
    "CONNECT:0"  intercept;
    "CONNECT:1"  relay;
}
EOF
    fi

    cat <<EOF

# outbound: a forward proxy for the app's egress, loopback only. Point the
# app's HTTP_PROXY / HTTPS_PROXY at http://127.0.0.1:3128.
server {
    listen       127.0.0.1:3128;
    server_name  outbound;

    resolver          $1 ipv6=$ipv6 valid=10s;
    resolver_timeout  5s;

    prometheus_mode 1;

    location / {
EOF

    if [ -n "$3" ] && [ -n "$bypass" ]; then
        cat <<EOF
        # HTTPS and gRPC: the tunnel's first bytes pick where it goes. TLS
        # and plaintext gRPC are decoded on :$tls_port / :$h2c_port, anything
        # else is relayed as is; INTERCEPT_BYPASS hosts are always relayed.
        if (\$prometheus_connect = intercept) {
            prometheus_intercept_connect  tls=127.0.0.1:$tls_port h2c=127.0.0.1:$h2c_port;
        }
        if (\$prometheus_connect = relay) {
            tunnel_pass;
        }

EOF
    elif [ -n "$3" ]; then
        cat <<EOF
        # HTTPS and gRPC: the tunnel's first bytes pick where it goes. TLS
        # and plaintext gRPC are decoded on :$tls_port / :$h2c_port, anything
        # else is relayed as is.
        if (\$request_method = CONNECT) {
            prometheus_intercept_connect  tls=127.0.0.1:$tls_port h2c=127.0.0.1:$h2c_port;
        }

EOF
    elif [ -n "$2" ]; then
        cat <<'EOF'
        # HTTPS: the tunnel is relayed as is; host and port are recorded
        if ($request_method = CONNECT) {
            tunnel_pass;
        }

EOF
    fi

    cat <<'EOF'
        proxy_pass               http://$http_host$request_uri;
        proxy_http_version       1.1;
        proxy_set_header         Host                $http_host;
        proxy_set_header         Upgrade             $http_upgrade;
        proxy_set_header         Connection          $prometheus_connection_upgrade;
        proxy_set_header         Proxy-Connection    "";
        proxy_set_header         Proxy-Authorization "";
        proxy_pass_header        Server;
        proxy_pass_header        Date;
        proxy_redirect           off;
        proxy_buffering          off;
        proxy_request_buffering  off;
        proxy_buffer_size        32k;
        proxy_buffers            4 32k;
        proxy_connect_timeout    10s;
        proxy_send_timeout       300s;
        proxy_read_timeout       300s;
    }
EOF

    if [ -n "$2" ]; then
        cat <<'EOF'

    tunnel_connect_timeout  10s;
    tunnel_read_timeout     300s;
    tunnel_send_timeout     300s;
EOF
    fi

    echo "}"
}

inbound > "$conf"

said=""

case ${IPV6:-off} in
    on)  ipv6=on ;;
    off) ipv6=off ;;
    *)   echo "$0: IPV6 must be on or off, got \"$IPV6\"" >&2; exit 1 ;;
esac

tls_port=$(port_of INTERCEPT_TLS_PORT "${INTERCEPT_TLS_PORT:-3129}")
h2c_port=$(port_of INTERCEPT_H2C_PORT "${INTERCEPT_H2C_PORT:-3130}")
ca_reload=${INTERCEPT_CA_RELOAD:-60}

case $ca_reload in
    ''|*[!0-9]*) echo "$0: INTERCEPT_CA_RELOAD must be seconds, got \"$ca_reload\"" >&2; exit 1 ;;
esac

# host names, lowercased, one per word; anything else is an error
bypass=""

for h in $(echo "${INTERCEPT_BYPASS:-}" | tr ',' ' ' | tr '[:upper:]' '[:lower:]'); do
    case $h in
        *[!a-z0-9.*-]*|'')
            echo "$0: INTERCEPT_BYPASS entry \"$h\" is not a host name" >&2; exit 1 ;;
    esac
    bypass="$bypass $h"
done

if [ "${OUTBOUND:-on}" != "off" ]; then
    # names are resolved the way the app resolves them: the pod's own DNS
    dns=$(awk '$1 == "nameserver" && $2 ~ /^[0-9.]+$/ { print $2; exit }' \
          /etc/resolv.conf 2>/dev/null || true)

    if [ -z "$dns" ]; then
        echo "$0: no IPv4 nameserver in /etc/resolv.conf; outbound is off" >&2
    else
        # CONNECT needs the tunnel module: built in since nginx 1.31
        version=$(nginx -v 2>&1 | sed -n 's#^nginx version: nginx/##p')
        major=${version%%.*}
        rest=${version#*.}
        minor=${rest%%.*}
        tunnel=""

        if [ "${major:-0}" -gt 1 ] \
           || { [ "${major:-0}" -eq 1 ] && [ "${minor:-0}" -ge 31 ]; }
        then
            if ! nginx -V 2>&1 | grep -q -- '--without-http_tunnel_module'; then
                tunnel=1
            fi
        fi

        icept=""

        if [ "${INTERCEPT_TLS:-off}" = "on" ]; then
            ca_cert=${INTERCEPT_CA_CERT:-/etc/nginx/intercept/ca.crt}
            ca_key=${INTERCEPT_CA_KEY:-/etc/nginx/intercept/ca.key}
            trusted=${INTERCEPT_TRUSTED_CA:-}

            if [ -z "$trusted" ]; then
                for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem \
                         /etc/pki/tls/certs/ca-bundle.crt; do
                    if [ -s "$f" ]; then trusted=$f; break; fi
                done
            fi

            if [ -z "$tunnel" ]; then
                echo "$0: INTERCEPT_TLS=on needs nginx 1.31+; interception is off" >&2
            elif [ ! -r "$ca_cert" ] || [ ! -r "$ca_key" ]; then
                echo "$0: INTERCEPT_TLS=on but no CA at $ca_cert / $ca_key" >&2
                exit 1
            elif [ -z "$trusted" ]; then
                echo "$0: INTERCEPT_TLS=on but no trusted CA bundle; set INTERCEPT_TRUSTED_CA" >&2
                exit 1
            else
                icept=1
            fi
        fi

        outbound "$dns" "$tunnel" "$icept" >> "$conf"

        if [ -n "$icept" ]; then
            intercept "$dns" "$ca_cert" "$ca_key" "$trusted" >> "$conf"
            said=", outbound on 127.0.0.1:3128 with https interception"
            [ -n "$bypass" ] && said="$said (bypassed:$bypass)"
        elif [ -n "$tunnel" ]; then
            said=", outbound on 127.0.0.1:3128"
        else
            said=", outbound on 127.0.0.1:3128 (HTTP only: HTTPS needs nginx 1.31+)"
        fi
    fi
fi

if [ -n "${STREAM:-}" ]; then
    main=/etc/nginx/main.d/sidecar-stream.conf

    if ! : > "$main" 2>/dev/null; then
        echo "$0: STREAM is set but $main can't be written" >&2
        exit 1
    fi

    {
        echo "# Generated at start by /docker-entrypoint.d/40-prometheus-sidecar.sh"
        echo
        echo "stream {"

        if [ -n "${PROMETHEUS_STREAM_CONFIG:-}" ]; then
            env_config "$stream_extra" "$PROMETHEUS_STREAM_CONFIG" PROMETHEUS_STREAM_CONFIG
            echo "    include $stream_extra;"
        fi

        for entry in $STREAM; do
            name=${entry%%:*}
            rest=${entry#*:}
            listen=${rest%%:*}
            target=${rest#*:}

            case $listen in
                ''|*[!0-9]*) listen="" ;;
            esac

            case $target in
                ?*:?*) ;;
                *) target="" ;;
            esac

            if [ -z "$name" ] || [ "$name" = "$entry" ] || [ -z "$listen" ] \
               || [ -z "$target" ]
            then
                echo "$0: STREAM entry \"$entry\" is not <name>:<port>:<host>:<port>" >&2
                exit 1
            fi

            cat <<EOF
    server {
        listen                  127.0.0.1:$listen;
        prometheus_destination  $name;
        proxy_pass              $target;
        proxy_connect_timeout   5s;
        proxy_timeout           1h;
    }
EOF
        done

        echo "}"
    } > "$main"

    said="$said, stream: $STREAM"
fi

if [ -n "${PROMETHEUS_STREAM_CONFIG:-}" ] && [ -z "${STREAM:-}" ]; then
    echo "$0: PROMETHEUS_STREAM_CONFIG is set but STREAM isn't; ignored" >&2
fi

check_env_config

echo "$0: sidecar for 127.0.0.1:$APP_PORT${POD_CIDR:+, callers in $POD_CIDR}$said"

# A rotated CA (a Secret updated in place) is read again: the master reads it
# at each load, so a reload is all it takes. Checked every INTERCEPT_CA_RELOAD
# seconds, from the background, for as long as the container runs.
if [ -n "${icept:-}" ] && [ "$ca_reload" -gt 0 ]; then
    (
        sum() { cat "$ca_cert" "$ca_key" 2> /dev/null | cksum; }
        last=$(sum)

        while sleep "$ca_reload"; do
            now=$(sum)
            [ "$now" = "$last" ] && continue

            if nginx -t -q 2> /dev/null; then
                nginx -s reload && last=$now \
                    && echo "$0: the interception CA changed; nginx reloaded" >&2
            else
                echo "$0: the interception CA changed but nginx -t fails; not reloaded" >&2
            fi
        done
    ) < /dev/null > /dev/null &
fi
