# sidecar — the nginx image in front of your app

The [nginx image](../images.md#nginx) as a sidecar: clients reach your pod on
`:80`, nginx forwards every request to the app on `127.0.0.1:$APP_PORT` as the
client sent it, and every request is recorded. Nothing in the app changes.

Turn on with one variable, `APP_PORT`. It's the base for
[caller identity](identity.md) (one more variable, `POD_CIDR`) and
[outbound](outbound.md) (on with it; the app sets `HTTP(S)_PROXY`).

---

## What you get

```
nginx_http_requests_total{method="GET",route="/orders/$param",status="200",host="api.internal",mode="0",protocol="http"} 42
nginx_http_request_duration_seconds{...} 0.031
nginx_http_upstream_response_time_seconds{...} 0.029
```

`mode="0"` is inbound traffic. Request duration is what the client saw;
upstream response time is what the app took. Every metric and label:
[module.md](README.md#metrics).

---

## Step by step

### 1. Add the container

**Kubernetes:** a second container in your pod.

```yaml
- name: nginx
  image: ghcr.io/hatam-abolghasemi/nginx:mainline-alpine  # pin a tag, docs/images.md; mainline for HTTPS outbound
  env:
    - { name: APP_PORT, value: "8080" }            # the port your app listens on
  ports:
    - { name: http,    containerPort: 80 }
    - { name: metrics, containerPort: 9145 }
  readinessProbe:
    httpGet: { path: /healthz, port: metrics }
  resources:
    requests: { cpu: 10m, memory: 16Mi }
    limits:   { cpu: 500m, memory: 64Mi }
```

Then point your Service's `targetPort` at `http` (`:80`) instead of the app's
port, so client traffic goes through nginx. A full example (Deployment,
Service, ServiceMonitor): [`examples/nginx-sidecar/`](../../examples/nginx-sidecar/).

**Docker:** share the app's network namespace, so `127.0.0.1:$APP_PORT`
reaches it.

```bash
docker run -d --name app -p 80:80 -p 9145:9145 your-app:latest
docker run -d --name nginx --network container:app -e APP_PORT=8080 \
  ghcr.io/hatam-abolghasemi/nginx:mainline-alpine
```

### 2. Scrape `:9145/metrics`

With the Prometheus Operator, the example's ServiceMonitor already does it.
By hand:

```yaml
scrape_configs:
  - job_name: my-app-nginx
    static_configs:
      - targets: ["my-app:9145"]
```

### 3. Check it

```bash
curl -s localhost:9145/healthz                                   # ok
curl -s localhost:9145/metrics | grep '^nginx_http_requests_total'
```

The container's log says what it set up:

```
/docker-entrypoint.d/40-prometheus-sidecar.sh: sidecar for 127.0.0.1:8080
```

---

## How it's wired

At each start, `/docker-entrypoint.d/40-prometheus-sidecar.sh` sees `APP_PORT`
and writes `/etc/nginx/conf.d/sidecar.conf`:

- an `upstream app { server 127.0.0.1:$APP_PORT; keepalive 64; }`;
- a `:80` server that proxies to it **transparently**: the `Host` header, the
  raw URI, bodies of any size (no buffering) and WebSocket upgrades pass
  through; nothing is added (no `X-Forwarded-*`), so the app sees `127.0.0.1`
  as its peer;
- `prometheus_identity on`, so the pod finds its own Service name
  ([own identity](identity.md#own-identity));
- the [outbound](outbound.md) forward proxy on `127.0.0.1:3128`, with
  the pod's DNS server from `/etc/resolv.conf`.

It removes `conf.d/default.conf`, which also listens on `:80`. Metrics are
served by the module itself on `:9145`, never `:80`: a scrape is not app
traffic, and the sidecar's own paths never shadow the app's.

Without `APP_PORT` the script does nothing but `PROMETHEUS_CONFIG`, and the
image is stock nginx with metrics, unless you ask for the outbound side:
see [your own config](#your-own-config-outbound-only).

| Variable | Default | |
|---|---|---|
| `APP_PORT` | — | Turns the sidecar on: the app's port |
| `POD_CIDR` | — | [Caller identity](identity.md): the pod ranges, space separated, e.g. `10.233.0.0/16` |
| `CLUSTER_IP_PREFIX` | — | A `Host` that is an IP starting with this (e.g. `10.233.`) is labelled `kubernetes` |
| `OUTBOUND` | on (off without `APP_PORT`) | `off`: no forward proxy on `127.0.0.1:3128`; `on` without `APP_PORT`: [outbound only](#your-own-config-outbound-only) |
| `INTERCEPT_TLS` | `off` | [https interception](interception.md): `on` with a CA at `/etc/nginx/intercept/`; more variables [there](interception.md#with-the-sidecar-image). Without `APP_PORT` it turns outbound on too |
| `OUTBOUND_TIMEOUT` | `300s` | How long outbound waits on a quiet destination: the proxy, gRPC and tunnel read/send timeouts, and a relayed tunnel's idle time (`60s`, `5m`, ...). Inbound keeps its own `300s` |
| `IPV6` | `off` | `on`: outbound and interception resolve IPv6 (AAAA) too, IPv4 preferred |
| `STREAM` | — | [Stream](stream.md): `<name>:<local port>:<host>:<port>` entries, space separated |
| `PROMETHEUS_CONFIG` | — | Module directives for `http{}`, any image, e.g. `prometheus_metric request_duration_histogram on; prometheus_label client_subnet on;` |
| `PROMETHEUS_STREAM_CONFIG` | — | The same for `stream{}` (with `STREAM`), e.g. `prometheus_label source_service on;` |

A setting is one variable, no file to mount: `PROMETHEUS_CONFIG` takes any
[directive](README.md#directives) of the module. If `nginx -t` rejects it,
it's left out and the reason logged; a typo in a metrics setting never stops
nginx, which also carries the app's traffic.

For more, it's plain nginx: mount `.conf` files into `/etc/nginx/conf.d/`
(included at the `http{}` level), or `stream{}` blocks into
`/etc/nginx/main.d/`.

### Your own config, outbound only

The sidecar's `:80` speaks HTTP to the app. An app behind FastCGI (php-fpm),
uwsgi or anything else nginx has to translate for keeps its own
`default.conf` and leaves `APP_PORT` unset; the image adds the outbound side
alone:

```yaml
- name: nginx
  image: ghcr.io/hatam-abolghasemi/nginx:1.31.10-alpine-slim
  env:
    - { name: INTERCEPT_TLS, value: "on" }    # or OUTBOUND=on, without interception
  volumeMounts:
    - { name: default-conf, mountPath: /etc/nginx/conf.d/default.conf, subPath: default.conf, readOnly: true }
    - { name: egress-ca,    mountPath: /etc/nginx/intercept, readOnly: true }
```

The script writes the forward proxy on `127.0.0.1:3128` (and, with
`INTERCEPT_TLS=on`, the terminators on `:3129` / `:3130`) into
`conf.d/prometheus-outbound.conf`, with the pod's DNS server, exactly as the
sidecar has them; `default.conf` stays. `OUTBOUND_TIMEOUT`, `IPV6` and every
`INTERCEPT_*` variable work the same, CA rotation included (mount the CA
Secret as a directory, [not with `subPath`](interception.md#limits)). `STREAM` still needs `APP_PORT`.
Your `default.conf` needs only its own servers, plus for caller identity
`prometheus_source_ptr <pod CIDR>;`. The log line:

```
/docker-entrypoint.d/40-prometheus-sidecar.sh: outbound on 127.0.0.1:3128 with https interception
```

### On upstream's own image, no build

To run `nginx:<version>-alpine-slim` (or any upstream tag) itself, with no
image of your own, an init container copies the module and this script out of
the matching image of this repo into an `emptyDir`; Kubernetes 1.33+ could
mount the image as a volume instead, older clusters need the copy. The module
package itself can't be used for this: it's `FROM scratch`, with no `cp`.

```yaml
initContainers:
  - name: nginx-prometheus
    image: ghcr.io/hatam-abolghasemi/nginx:1.31.10-alpine-slim   # the SAME tag as below
    command:
      - sh
      - -c
      - >-
        cp /usr/lib/nginx/modules/ngx_http_prometheus_module.so
        /usr/lib/nginx/modules/ngx_stream_prometheus_module.so
        /docker-entrypoint.d/40-prometheus-sidecar.sh /prometheus/
        && chmod 755 /prometheus/40-prometheus-sidecar.sh
    volumeMounts:
      - { name: nginx-prometheus, mountPath: /prometheus }
containers:
  - name: nginx
    image: nginx:1.31.10-alpine-slim
    volumeMounts:
      - { name: nginx-prometheus, mountPath: /etc/nginx/prometheus, readOnly: true }
      - { name: nginx-prometheus, mountPath: /docker-entrypoint.d/40-prometheus-sidecar.sh,
          subPath: 40-prometheus-sidecar.sh, readOnly: true }
      - { name: nginx-conf, mountPath: /etc/nginx/nginx.conf, subPath: nginx.conf, readOnly: true }
volumes:
  - { name: nginx-prometheus, emptyDir: { sizeLimit: 16Mi } }
```

- **The two tags must name the same nginx version.** nginx refuses a module
  built for another version, and doesn't start. Bump them together, as one
  value.
- Upstream's `nginx.conf` has no `load_module`: mount one that starts with
  `load_module /etc/nginx/prometheus/ngx_http_prometheus_module.so;` (and the
  stream one, for `STREAM`, plus `include /etc/nginx/main.d/*.conf;`, which
  matches nothing until `STREAM` writes there).
- It pulls this repo's image anyway, for the init container: the gain is a
  long-running container that is upstream's, byte for byte, not smaller pulls.

**Read-only root filesystem?** The script writes into `/etc/nginx/conf.d/`;
mount an `emptyDir` there, or mount your own `sidecar.conf` and leave
`APP_PORT` unset.

## Limits

- The app must listen on `127.0.0.1` or `0.0.0.0` inside the pod.
- `Connection: close` from the app, or a slow app, shows up in
  `nginx_http_upstream_response_time_seconds`, not as an nginx problem.
- HTTP on `:80`. Redis, databases and other TCP traffic go through
  [stream](stream.md), with `STREAM`.
