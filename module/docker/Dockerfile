# One Dockerfile for every official nginx image flavor. NGINX_TAG is the full
# upstream tag, flavor included: 1.31.10, 1.31.10-alpine, 1.31.10-perl, ...
#
# The module is built against the exact nginx version inside that image, with
# --with-compat, then copied into a clean copy of the same image. Nothing else
# in the image changes except two config files, load_module at the top of
# nginx.conf and conf.d/prometheus.conf (zone + :9145/metrics), and one
# entrypoint script that does nothing unless APP_PORT is set (the sidecar).
#
#   docker build -f module/docker/Dockerfile --build-arg NGINX_TAG=1.31.10-alpine .
#
# `--target module --output type=local,dest=out` gives only the .so.
ARG BASE_IMAGE=nginx
ARG NGINX_TAG

FROM ${BASE_IMAGE}:${NGINX_TAG} AS build

# The nginx source of the version this image runs (nginx -v), from GitHub.
# Build tools come from the flavor's own package manager, so the .so links
# against the same libc as the nginx binary it is loaded into.
RUN set -eux; \
    version=$(nginx -v 2>&1 | sed -n 's#^nginx version: nginx/##p'); \
    if command -v apk > /dev/null; then \
        apk add --no-cache gcc libc-dev make openssl-dev pcre2-dev zlib-dev linux-headers curl; \
    elif command -v apt-get > /dev/null; then \
        apt-get update; \
        apt-get install -y --no-install-recommends gcc libc6-dev make libssl-dev libpcre2-dev zlib1g-dev curl ca-certificates; \
    else \
        echo "no apk or apt-get in this flavor" >&2; exit 1; \
    fi; \
    mkdir -p /usr/src/nginx; \
    curl -fsSL "https://github.com/nginx/nginx/archive/refs/tags/release-${version}.tar.gz" \
        | tar -xz -C /usr/src/nginx --strip-components=1

COPY module/ /usr/src/ngx_http_prometheus_module/

# --with-compat makes the .so loadable by any nginx of this version built with
# --with-compat (every official image is); stub_status and ssl only switch on
# the module's connection-state and TLS code paths.
RUN set -eux; \
    cd /usr/src/nginx; \
    ./auto/configure --with-compat --with-http_stub_status_module --with-http_ssl_module \
        --add-dynamic-module=/usr/src/ngx_http_prometheus_module; \
    make -j"$(nproc)" modules; \
    mkdir -p /out; \
    cp objs/ngx_http_prometheus_module.so /out/

FROM scratch AS module
COPY --from=build /out/ngx_http_prometheus_module.so /

FROM ${BASE_IMAGE}:${NGINX_TAG}

COPY --from=build /out/ngx_http_prometheus_module.so /usr/lib/nginx/modules/
COPY module/docker/prometheus.conf /etc/nginx/conf.d/prometheus.conf
# with APP_PORT set, makes the image a sidecar in front of the app
COPY --chmod=755 module/docker/40-prometheus-sidecar.sh /docker-entrypoint.d/

# load_module must sit in nginx.conf's main context; the stock nginx.conf has
# no include there, so the line is put at its top.
RUN set -eux; \
    sed -i '1i load_module modules/ngx_http_prometheus_module.so;' /etc/nginx/nginx.conf; \
    nginx -t

EXPOSE 9145
