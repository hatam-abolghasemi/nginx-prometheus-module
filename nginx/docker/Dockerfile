# One Dockerfile for every official nginx image flavor. NGINX_TAG is the full
# upstream tag, flavor included: 1.31.10, 1.31.10-alpine, 1.31.10-perl, ...
#
# The module is built against the exact nginx version inside that image, with
# --with-compat, then copied into a clean copy of the same image. Nothing else
# in the image changes except three lines at the top of nginx.conf (the two
# load_module lines, and an include of main.d/ for stream{} files; the module
# then serves :9145/metrics on its own, whatever the rest of the config is),
# and one entrypoint script that does nothing unless APP_PORT is set (the
# sidecar).
#
#   docker build -f nginx/docker/Dockerfile --build-arg NGINX_TAG=1.31.10-alpine nginx
#
# `--target module --output type=local,dest=out` gives only the two .so files.
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

COPY config /usr/src/ngx_http_prometheus_module/config
COPY src/ /usr/src/ngx_http_prometheus_module/src/

# --with-compat makes the .so files loadable by any nginx of this version built
# with --with-compat (every official image is); stub_status, ssl and stream
# only switch on the code paths that use them. Two .so files: http and stream.
RUN set -eux; \
    cd /usr/src/nginx; \
    ./auto/configure --with-compat --with-stream --with-http_stub_status_module \
        --with-http_ssl_module --add-dynamic-module=/usr/src/ngx_http_prometheus_module; \
    make -j"$(nproc)" modules; \
    mkdir -p /out; \
    cp objs/ngx_http_prometheus_module.so objs/ngx_stream_prometheus_module.so /out/

FROM scratch AS module
COPY --from=build /out/ /

FROM ${BASE_IMAGE}:${NGINX_TAG}

COPY --from=build /out/ /usr/lib/nginx/modules/
# with APP_PORT set, makes the image a sidecar in front of the app
COPY --chmod=755 docker/40-prometheus-sidecar.sh /docker-entrypoint.d/

# load_module and stream{} must sit in nginx.conf's main context, where the
# stock nginx.conf has no include: the two load_module lines and an include
# of main.d/ (stream{} files: the sidecar's, or your own) go at its top.
#
# https interception verifies destinations against the system CA bundle: a
# flavor without one gets the distro's, so INTERCEPT_TLS=on works on all.
RUN set -eux; \
    bundle=""; \
    for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt; do \
        if [ -s "$f" ]; then bundle=$f; break; fi; \
    done; \
    if [ -z "$bundle" ]; then \
        if command -v apk > /dev/null; then \
            apk add --no-cache ca-certificates; \
        elif command -v apt-get > /dev/null; then \
            apt-get update; \
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates; \
            rm -rf /var/lib/apt/lists/*; \
        fi; \
    fi; \
    mkdir -p /etc/nginx/main.d; \
    sed -i -e '1i load_module modules/ngx_http_prometheus_module.so;' \
           -e '1i load_module modules/ngx_stream_prometheus_module.so;' \
           -e '1i include /etc/nginx/main.d/*.conf;' /etc/nginx/nginx.conf; \
    nginx -t

EXPOSE 9145
