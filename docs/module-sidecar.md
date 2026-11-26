# sidecar — the nginx image in front of your app

The [nginx image](images.md#nginx) as a sidecar: clients reach your pod on
`:80`, nginx forwards every request to the app on `127.0.0.1:$APP_PORT` as the
client sent it, and every request is recorded. Nothing in the app changes.

Turn on with one variable, `APP_PORT`. It's the base for
[caller identity](module-identity.md) (one more variable, `POD_CIDR`) and
[outbound](module-outbound.md) (on with it; the app sets `HTTP(S)_PROXY`).

---

## What you get

```
nginx_http_requests_total{method="GET",route="/orders/$param",status="200",host="api.internal",mode="0",protocol="http"} 42
nginx_http_request_duration_seconds{...} 0.031
nginx_http_upstream_response_time_seconds{...} 0.029
```

`mode="0"` is inbound traffic. Request duration is what the client saw;
upstream response time is what the app took. Every metric and label:
[module.md](module.md#metrics).

---

## Step by step

### 1. Add the container

**Kubernetes:** a second container in your pod.

```yaml
- name: nginx
  image: ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:mainline-alpine  # pin a tag, docs/images.md; mainline for HTTPS outbound
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
Service, ServiceMonitor): [`examples/nginx-sidecar/`](../examples/nginx-sidecar/).

**Docker:** share the app's network namespace, so `127.0.0.1:$APP_PORT`
reaches it.

```bash
docker run -d --name app -p 80:80 -p 9145:9145 your-app:latest
docker run -d --name nginx --network container:app -e APP_PORT=8080 \
  ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:mainline-alpine
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
  ([own identity](module-identity.md#own-identity));
- the [outbound](module-outbound.md) forward proxy on `127.0.0.1:3128`, with
  the pod's DNS server from `/etc/resolv.conf`.

It removes `conf.d/default.conf`, which also listens on `:80`. Metrics are
served by the module itself on `:9145`, never `:80`: a scrape is not app
traffic, and the sidecar's own paths never shadow the app's.

Without `APP_PORT` the script does nothing, and the image is stock nginx with
metrics.

| Variable | Default | |
|---|---|---|
| `APP_PORT` | — | Turns the sidecar on: the app's port |
| `POD_CIDR` | — | [Caller identity](module-identity.md): the pod ranges, space separated, e.g. `10.233.0.0/16` |
| `CLUSTER_IP_PREFIX` | — | A `Host` that is an IP starting with this (e.g. `10.233.`) is labelled `kubernetes` |
| `OUTBOUND` | on | `off`: no forward proxy on `127.0.0.1:3128` |

Anything else is plain nginx: mount more `.conf` files into
`/etc/nginx/conf.d/` (they're included at the `http{}` level) to add
directives, e.g. `prometheus_metric request_duration_histogram on;`.

**Read-only root filesystem?** The script writes into `/etc/nginx/conf.d/`;
mount an `emptyDir` there, or mount your own `sidecar.conf` and leave
`APP_PORT` unset.

## Limits

- The app must listen on `127.0.0.1` or `0.0.0.0` inside the pod.
- `Connection: close` from the app, or a slow app, shows up in
  `nginx_http_upstream_response_time_seconds`, not as an nginx problem.
- Only HTTP. Redis, databases and other TCP traffic are [stream](module.md#features),
  coming in v2.3.
