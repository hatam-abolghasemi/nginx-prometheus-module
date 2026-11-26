# ngx_http_prometheus_module

A native nginx module that keeps Prometheus metrics for every HTTP request
nginx serves and exposes them on a `prometheus;` location. Stock nginx, no
Lua, no exporter process, no access logs to ship.

- [Features](#features): what you can turn on, and what each one needs
- [Install](#install): the image, a prebuilt `.so`, or a build from source
- [Configure](#configure): three lines, then everything optional
- [Directives](#directives)
- [Metrics](#metrics) and [labels](#labels)
- [How it works](#how-it-works): memory, cost, reloads

## Features

```
the module (stock nginx, or the nginx image)
├── metrics              on with prometheus_zone; every request nginx serves
├── sidecar (inbound)    image: APP_PORT; own nginx: a proxy_pass server
│   ├── own identity     on with the sidecar; app / namespace labels
│   └── caller identity  image: POD_CIDR; own nginx: prometheus_source_ptr
├── outbound             coming in v2.2
│   └── https interception   coming in v2.4; needs outbound
└── stream (L4)          coming in v2.3
```

| Guide | What it gives you | Turn on with |
|---|---|---|
| [metrics](#configure) | Requests by method, route, status, host; latency, sizes, upstream timing, connections | `prometheus_zone` + a `prometheus;` location |
| [sidecar](module-sidecar.md) | The image in front of your app on Kubernetes or Docker, nothing in the app changes | `APP_PORT` |
| [caller identity](module-identity.md) | Which service and namespace called you (`source_service`, `source_namespace`), who this pod is (`app`, `namespace`), `client_subnet` | `POD_CIDR`, or the `prometheus_source_*` directives |

Caller identity works on any nginx, the sidecar only makes it one variable.
What the [OpenResty sidecar](README.md) has that the module doesn't yet:
outbound, https interception, stream.

## Install

### The image

Every official `nginx` image tag, rebuilt with the module loaded. Swap the
image name and keep the tag:

```bash
docker run -p 80:80 -p 9145:9145 ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:alpine
curl -s localhost:9145/metrics
```

The image is the upstream image plus two files: `load_module` at the top of
`/etc/nginx/nginx.conf`, and `/etc/nginx/conf.d/prometheus.conf` (the zone and
the `:9145` server). Your own `conf.d/*.conf` works as before. If you replace
`nginx.conf` itself, keep the `load_module` line. Tags: [images](images.md).

### A prebuilt `.so`

Each [release](https://github.com/hatam-abolghasemi/nginx-prometheus-module/releases)
has the module built for the newest mainline and stable nginx:

```
ngx_http_prometheus_module-<release>-nginx-<nginx-version>-linux-<amd64|arm64>-<glibc|musl>.so
```

A dynamic module loads only into the **exact** nginx version it was built for,
and only if that nginx was built with `--with-compat` (nginx.org packages and
the official images are). Check yours with `nginx -V`. Then:

```bash
cp ngx_http_prometheus_module-*.so /etc/nginx/modules/ngx_http_prometheus_module.so
```

### From source

For any other nginx version (a distro package, your own build). You need the
nginx source of the same version you run and a C compiler:

```bash
version=$(nginx -v 2>&1 | sed 's#.*nginx/##')
curl -fsSL "https://github.com/nginx/nginx/archive/refs/tags/release-$version.tar.gz" | tar -xz
git clone https://github.com/hatam-abolghasemi/nginx-prometheus-module.git
cd "nginx-release-$version"
./auto/configure --with-compat --with-http_stub_status_module --with-http_ssl_module \
    --add-dynamic-module=../nginx-prometheus-module/module
make modules
sudo cp objs/ngx_http_prometheus_module.so /etc/nginx/modules/
```

The build needs the PCRE2, zlib and OpenSSL headers (`libpcre2-dev zlib1g-dev
libssl-dev` on Debian/Ubuntu, `pcre2-dev zlib-dev openssl-dev` on Alpine).
Building the module takes about a second. If your nginx was built without
`--with-compat`, build the module with exactly the configure arguments that
`nginx -V` prints instead.

## Configure

```nginx
load_module modules/ngx_http_prometheus_module.so;   # main context

http {
    prometheus_zone 4m;

    server {
        listen 9145;
        location = /metrics { prometheus; }
    }

    # your servers, unchanged: every request in them is recorded
}
```

That's all. A location with `prometheus;` is not recorded itself; the metrics
cover every other request.

## Directives

All `http{}`-level directives apply to the whole nginx; the last three can be
set per server or location.

| Directive | Default | Context | |
|---|---|---|---|
| `prometheus_zone <size>;` | — | http | Shared memory for all workers. Required: without it nothing is recorded. 4m holds several thousand label sets with gauges; histograms take more |
| `prometheus_metric <name> on\|off;` | see [metrics](#metrics) | http | Turns one metric on or off |
| `prometheus_label <name> on\|off;` | see [labels](#labels) | http | Turns one request label on or off |
| `prometheus_buckets <histogram> <le>...;` | see [metrics](#metrics) | http | Histogram buckets, increasing, at most 32 |
| `prometheus_gauge_expiry <time>;` | `15s` | http | A gauge is left out of a scrape once its label set has had no request for this long. Set it to your scrape interval |
| `prometheus_route_rule <rule> on\|off;` | all on | http | One [route rule](#route) |
| `prometheus_route_min_len <n>;` | `4` | http | `long_with_digit`: a segment longer than this with a digit in it |
| `prometheus_route_max_segment_len <n>;` | `64` | http | A segment longer than this is always `$param` |
| `prometheus_cluster_ip_prefix <prefix>;` | — | http | IPv4 `Host`s starting with it are labelled `kubernetes` (e.g. `10.233.`) |
| `prometheus_source_ptr <cidr>...;` | — | http | [Caller identity](module-identity.md): callers in these ranges are named by reverse DNS |
| `prometheus_source_static <cidr> <service> <namespace>;` | — | http | Callers in this range get these labels; the longest range wins |
| `prometheus_source_default <service> <namespace>;` | `public unknown` | http | Callers in no range |
| `prometheus_source_unresolved <service> <namespace>;` | `health-probe kubernetes` | http | Kubelet probes, and PTR callers with no answer (yet) |
| `prometheus_source_probe <prefix>\|off;` | `kube-probe/` | http | A `User-Agent` starting with it is a kubelet probe |
| `prometheus_source_waypoint <service>\|off;` | `waypoint` | http | A caller resolved to this service is the mesh waypoint: the real caller is read from its `X-Forwarded-Client-Cert` |
| `prometheus_source_ignore_service <regex>...\|off;` | `-metrics$ -headless$` | http | Services passed over when an address has several PTR records |
| `prometheus_source_refresh <time>;` | `15s` | http | How often every known caller is resolved again |
| `prometheus_source_forget <time>;` | `300s` | http | A caller no request has come from for this long is forgotten |
| `prometheus_identity on\|off;` | `off` | http | Discover this pod's own Service by reverse DNS: the `app` / `namespace` labels |
| `prometheus_identity_fallback <app> <namespace>;` | `unknown` and the service account's namespace | http | `app` / `namespace` until (or unless) discovery succeeds |
| `prometheus_dns <ip>[:port]\|auto;` | `auto` | http | The DNS server for PTR lookups; `auto` is the first IPv4 `nameserver` in `/etc/resolv.conf` |
| `prometheus_dns_timeout <time>;` | `2s` | http | One PTR query |
| `prometheus_cluster_domain <domain>;` | `cluster.local` | http | The cluster DNS domain in PTR names |
| `prometheus_record on\|off;` | `on` | http, server, location | Record requests here or not |
| `prometheus_mode <value>;` | `0` | http, server, location | The `mode` label: `0` inbound, `1` outbound |
| `prometheus;` | — | location | Serves the metrics (GET and HEAD) |

A reload keeps every count as long as the metrics, labels, buckets and route
settings stay the same; changing any of them starts the counts over
(Prometheus handles the reset).

## Metrics

The same names as the [OpenResty sidecar](metrics-reference.md), so its
queries and dashboard carry over.

Per label set (the [labels](#labels) below):

| Metric | `prometheus_metric` | Type | Default |
|---|---|---|---|
| `nginx_http_requests_total` | `requests` | counter | on |
| `nginx_http_request_duration_seconds` | `request_duration` | gauge, last request | on |
| `nginx_http_request_duration_seconds_histogram` | `request_duration_histogram` | histogram | off |
| `nginx_http_request_size_bytes` | `request_size` | gauge, last request | on |
| `nginx_http_request_size_bytes_histogram` | `request_size_histogram` | histogram | off |
| `nginx_http_response_size_bytes` | `response_size` | gauge, last response | on |
| `nginx_http_response_size_bytes_histogram` | `response_size_histogram` | histogram | off |
| `nginx_http_upstream_response_time_seconds` | `upstream_response_time` | gauge, last proxied request | on |
| `nginx_http_upstream_response_time_seconds_histogram` | `upstream_response_time_histogram` | histogram | off |
| `nginx_http_upstream_connect_time_seconds` | `upstream_connect_time` | gauge, last proxied request | off |
| `nginx_http_upstream_header_time_seconds` | `upstream_header_time` | gauge, last proxied request | off |

Server-wide:

| Metric | `prometheus_metric` | Type | Default |
|---|---|---|---|
| `nginx_http_connections{state}` | `connections` | gauge: `active`, `reading`, `writing`, `waiting` | on |
| `nginx_connection_requests` | `connection_requests` | gauge, last request's connection | on |
| `nginx_connection_requests_histogram` | `connection_requests_histogram` | histogram | off |
| `nginx_connection_time_seconds` | `connection_time` | gauge, last request's connection | on |
| `nginx_connection_time_seconds_histogram` | `connection_time_histogram` | histogram | off |
| `nginx_connection_slots_limit` | `connection_slots_limit` | gauge: `worker_connections` × workers | on |
| `nginx_metric_errors_total` | — | counter: requests not recorded because the zone was full | always |

**Why gauges by default.** With `route` as a label, a histogram multiplies
every label set by its bucket count (16 + 3 series for the default duration
buckets). The last-value gauge keeps one series per label set, so latency and
sizes are there even when storage is tight. Turn on the histograms you take
percentiles from.

**Upstream times** are those of the attempt that answered (the last entry of
`$upstream_response_time`). A label set that was never proxied has no
upstream series.

Default buckets: durations `0.01 0.025 0.05 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8
0.9 1 2.5 5 10`; sizes `64 256 1024 4096 16384 65536 262144 1048576`;
connection requests `1 2 5 10 25 50 100 250`; connection time `0.1 0.5 1 5 15
30 60 300`.

## Labels

In this order on every per-request series:

| Label | Example | From | Default |
|---|---|---|---|
| `method` | `GET` | request method | on |
| `route` | `/orders/$param` | the request URI, [sanitized](#route) | on |
| `status` | `200`, `499` | response status | on |
| `client_subnet` | `10.233.4.0`, `8.8.0.0` | the first `X-Forwarded-For` address, else the peer: /24 for private ranges, /16 for public | off |
| `host` | `api.example.com`, `localhost`, `kubernetes`, `$ip`, `checkout` | `$host`, classified: IP literals collapse so each address is not a series; this pod's own IP shows as its own Service | on |
| `app` / `namespace` | `checkout` / `shop` | [own identity](module-identity.md#own-identity) | off |
| `mode` | `0` | `prometheus_mode` | on |
| `source_service` / `source_namespace` | `cart` / `shop` | [caller identity](module-identity.md) | off |
| `protocol` | `http`, `https`, `grpc`, `grpcs`, `tunnel` | the connection's TLS, gRPC by `Content-Type`, `tunnel` for CONNECT | on |

Turn any of them on or off with `prometheus_label <name> on|off;`.

Label values are escaped for the exposition format; control bytes and invalid
UTF-8 become `_`.

### Route

The query string is dropped, empty segments go away, and a segment matching any
rule becomes `$param` (adjacent ones collapse into one):

| Rule | Matches |
|---|---|
| `pure_numeric` | `123` |
| `uuid` | `550e8400-e29b-41d4-a716-446655440000` |
| `hex` | 8+ hex characters: `a1b2c3d4` |
| `slug_suffix_digit` | ends in `-<digits>`: `order-12345` |
| `long_with_digit` | longer than `prometheus_route_min_len` with a digit: `abc12` |
| `file_extension` | ends in `.` and 2+ letters: `app.js` |

Any segment longer than `prometheus_route_max_segment_len` is `$param`.
`/orders/8231/items/550e8400-...?x=1` → `/orders/$param/items/$param`.
These are the OpenResty sidecar's rules, with the same results.

## How it works

- **Recording** runs in the log phase, after the response is sent: the label
  set is built on the stack, then looked up in the zone's red-black tree and
  updated under the zone's mutex (the pattern `limit_req` uses). No allocation
  per request once a label set exists.
- **A scrape** copies the zone under the mutex, then formats the copy without
  holding it, so requests wait only for a `memcpy`.
- **Memory** is the zone you configure, shared by all workers, plus the scrape
  response while it is being sent. A full zone never blocks a request: new
  label sets are dropped and counted in `nginx_metric_errors_total`, known
  ones keep counting.
- **Gauge expiry** happens at scrape time: a quiet label set's gauges are left
  out, its counters and histograms stay.
