#!/bin/sh
# Run by the nginx image's entrypoint (/docker-entrypoint.d/) at each start.
# Without APP_PORT it does nothing: the image is stock nginx plus metrics.
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
#
# Everything else is plain nginx config: mount more files into conf.d/.
set -eu

[ -n "${APP_PORT:-}" ] || exit 0

case $APP_PORT in
    ''|*[!0-9]*) echo "$0: APP_PORT must be a port number, got \"$APP_PORT\"" >&2; exit 1 ;;
esac

conf=/etc/nginx/conf.d/sidecar.conf

if ! : > "$conf" 2>/dev/null; then
    echo "$0: APP_PORT is set but $conf can't be written (read-only conf.d?)" >&2
    exit 1
fi

rm -f /etc/nginx/conf.d/default.conf

{
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
} > "$conf"

echo "$0: sidecar for 127.0.0.1:$APP_PORT${POD_CIDR:+, callers in $POD_CIDR}"
