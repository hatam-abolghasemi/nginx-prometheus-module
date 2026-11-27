# nginx-prometheus-module

[![nginx tests](https://github.com/hatam-abolghasemi/nginx-prometheus-module/actions/workflows/nginx-tests.yml/badge.svg)](https://github.com/hatam-abolghasemi/nginx-prometheus-module/actions/workflows/nginx-tests.yml)
[![OpenResty tests](https://github.com/hatam-abolghasemi/nginx-prometheus-module/actions/workflows/openresty-tests.yml/badge.svg)](https://github.com/hatam-abolghasemi/nginx-prometheus-module/actions/workflows/openresty-tests.yml)
[![nginx image](https://github.com/hatam-abolghasemi/nginx-prometheus-module/actions/workflows/nginx-image.yml/badge.svg)](docs/images.md)
[![OpenResty image](https://github.com/hatam-abolghasemi/nginx-prometheus-module/actions/workflows/openresty-image.yml/badge.svg)](docs/images.md)
[![Release](https://img.shields.io/github/v/release/hatam-abolghasemi/nginx-prometheus-module)](https://github.com/hatam-abolghasemi/nginx-prometheus-module/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Stars](https://img.shields.io/github/stars/hatam-abolghasemi/nginx-prometheus-module?style=flat)](https://github.com/hatam-abolghasemi/nginx-prometheus-module/stargazers)

**The metrics you'd expect nginx to expose on `/metrics` — now it does.**

A Prometheus module for nginx: every request and every proxied TCP/UDP
session is recorded by nginx itself, in shared memory, and served on
`:9145/metrics`. No exporter process, no access logs to ship and parse, no
mesh.

```nginx
load_module modules/ngx_http_prometheus_module.so;
```

That line is the whole setup.

```
nginx_http_requests_total{method="GET",route="/orders/$param",status="200",host="api.example.com",mode="0",protocol="https"} 1027
nginx_http_request_duration_seconds{method="GET",route="/orders/$param",status="200",host="api.example.com",mode="0",protocol="https"} 0.031
nginx_http_upstream_response_time_seconds{method="GET",route="/orders/$param",status="200",host="api.example.com",mode="0",protocol="https"} 0.029
nginx_stream_connections_total{destination="redis",upstream_addr="10.233.12.7:6379",status="200"} 418
```

- [Get it](#get-it)
- [Metrics](#metrics) and [labels](#labels)
- [Features](#features): sidecar, caller identity, outbound, stream
- [Two implementations](#two-implementations): C for stock nginx, Lua for OpenResty
- [Images and releases](#images-and-releases)

## Get it

**The image**: the official `nginx` image, rebuilt with the module loaded
(mainline and stable, debian and alpine; [any other flavor](docs/images.md#any-other-flavor)
is one `docker build`). Swap the image name, keep the tag:

```bash
docker run -p 80:80 -p 9145:9145 ghcr.io/hatam-abolghasemi/nginx:alpine
curl -s localhost:9145/metrics
```

**Your own nginx**: a prebuilt `.so` from the
[releases](https://github.com/hatam-abolghasemi/nginx-prometheus-module/releases)
for the newest mainline and stable nginx, or built from source for any
version in about a second ([install](docs/nginx/README.md#install)). Load it
at the top of `nginx.conf` and every `http{}` server is recorded; the module
opens `:9145` by itself, whatever the rest of the configuration is.

## Metrics

Per label set (the [labels](#labels) below), for every request nginx serves:

| Metric | Type | Default | |
|---|---|---|---|
| `nginx_http_requests_total` | counter | on | Requests |
| `nginx_http_request_duration_seconds` | gauge | on | Duration of the last request, as the client saw it |
| `nginx_http_request_size_bytes` | gauge | on | Size of the last request |
| `nginx_http_response_size_bytes` | gauge | on | Size of the last response body |
| `nginx_http_upstream_response_time_seconds` | gauge | on | Upstream time of the last proxied request: what the app took |
| `nginx_http_upstream_connect_time_seconds` | gauge | off | Upstream connect time of the last proxied request |
| `nginx_http_upstream_header_time_seconds` | gauge | off | Upstream time to response headers |
| `…_histogram` of duration, sizes and upstream response time | histogram | off | For percentiles |

For the whole server:

| Metric | Type | |
|---|---|---|
| `nginx_http_connections{state}` | gauge | `active`, `reading`, `writing`, `waiting` |
| `nginx_connection_requests` | gauge | Requests on the last request's connection (keepalive reuse) |
| `nginx_connection_time_seconds` | gauge | How long the last request's connection had been open |
| `nginx_connection_slots_limit` | gauge | `worker_connections` × workers |
| `nginx_metric_errors_total` | counter | Requests not recorded because the zone was full: should stay 0 |

For every TCP/UDP session the stream module proxies (with
`ngx_stream_prometheus_module.so`), by `destination`, `upstream_addr` and
`status`:

| Metric | Type | Default | |
|---|---|---|---|
| `nginx_stream_connections_total` | counter | on | Sessions |
| `nginx_stream_bytes_sent_total`, `_received_total` | counter | on | Bytes to and from the client |
| `nginx_stream_upstream_bytes_sent_total`, `_received_total` | counter | on | Bytes to and from the upstream |
| `nginx_stream_session_duration_seconds` | gauge | on | Duration of the last session |
| `nginx_stream_upstream_connect_time_seconds` | gauge | on | Upstream connect time of the last session |
| `nginx_stream_connections_active{state="active"}` | gauge | on | Sessions open now |
| `…_histogram` of session duration, connect time, first byte time | histogram | off | For percentiles |

**Why gauges by default:** with `route` as a label, a histogram multiplies
every label set by its bucket count, about 20 series where a gauge is one.
The last-value gauge keeps latency and sizes when storage is tight, and is
left out of a scrape once its label set has gone quiet
(`prometheus_gauge_expiry`, 15s). Turn on the histograms you take percentiles
from: `prometheus_metric request_duration_histogram on;`.

Every metric, directive and default: [docs/nginx/README.md](docs/nginx/README.md#metrics).

## Labels

| Label | Example | Default | |
|---|---|---|---|
| `method` | `GET` | on | |
| `route` | `/orders/$param` | on | The URI, [sanitized](#routes): IDs, UUIDs and hashes become `$param`, the query string is dropped. For `CONNECT`, `host:port` |
| `status` | `200`, `499` | on | |
| `host` | `api.example.com`, `localhost`, `$ip` | on | `$host`; IP literals collapse to `$ip` (or `kubernetes`), so an address is never a series |
| `mode` | `0`, `1` | on | `0` inbound, `1` outbound (the app's egress) |
| `protocol` | `http`, `https`, `grpc`, `grpcs`, `tunnel` | on | |
| `source_service`, `source_namespace` | `cart`, `shop` | off | Who called: [caller identity](docs/nginx/identity.md), by reverse DNS of the pod IP |
| `app`, `namespace` | `checkout`, `shop` | off | Who this nginx is: [own identity](docs/nginx/identity.md#own-identity) |
| `client_subnet` | `10.233.4.0`, `8.8.0.0` | off | The client's /24 (private) or /16 (public) |

On stream series: `destination` (`prometheus_destination`, e.g. `redis`),
`upstream_addr` and `status` (`200`, or `502` when the upstream couldn't be
reached).

### Routes

```
/orders/8231                                     → /orders/$param
/orders/8231/items/550e8400-e29b-41d4-a716-…     → /orders/$param/items/$param
/static/app.3f9a1c.js?v=2                        → /static/$param
/users/42/avatar                                 → /users/$param/avatar
/orders/new                                      → /orders/new
```

A segment becomes `$param` when it is a number, a UUID, 8+ hex characters,
ends in `-<digits>`, is longer than 4 characters with a digit in it, or ends
in a file extension. Each rule can be turned off
([route rules](docs/nginx/README.md#route)).

## Features

```
the module
├── metrics              on as soon as it's loaded: :9145/metrics
├── sidecar              image: APP_PORT; in front of your app on Kubernetes or Docker
│   ├── own identity     on with the sidecar: app / namespace
│   ├── caller identity  POD_CIDR: source_service / source_namespace
│   └── outbound         on with the sidecar; the app sets HTTP(S)_PROXY
└── stream (L4)          the stream .so; image: STREAM
```

| Guide | What it gives you |
|---|---|
| [The module](docs/nginx/README.md) | Install, configure, every directive, metric and label |
| [sidecar](docs/nginx/sidecar.md) | The image in front of your app with `APP_PORT`; nothing in the app changes |
| [caller identity](docs/nginx/identity.md) | Which Service and namespace called you, and who this pod is |
| [outbound](docs/nginx/outbound.md) | The calls your app makes, through `127.0.0.1:3128`: plain HTTP in full, HTTPS as `CONNECT host:port` |
| [stream](docs/nginx/stream.md) | Redis, databases, brokers: sessions, bytes, duration, connect time by destination |

A Kubernetes example with all of it: [`examples/nginx-sidecar/`](examples/nginx-sidecar/).

![Grafana dashboard: inbound and outbound requests per second, 5xx ratio and latency, and a table of sanitized routes with status, latency and the calling namespace/service](docs/assets/dashboard-summary.png)

<sub>The [Grafana dashboard](grafana/) on a real workload: inbound and
outbound side by side, IDs collapsed to `$param`, and the caller's namespace
and service on every row. It works with either implementation.</sub>

## Two implementations

The same module twice, with the same metric names, labels and route rules, so
queries carry over:

| | [The C module](docs/nginx/README.md) | [The Lua module](docs/openresty/README.md) |
|---|---|---|
| Runs on | stock nginx, any version built `--with-compat` | OpenResty, or nginx with `lua-nginx-module` |
| Setup | `load_module` | the image, or the Lua files in your OpenResty |
| Inbound, sidecar, caller identity, outbound | ✓ | ✓ |
| Stream (L4) | ✓, on `:9145` | ✓, on `:9254`, with caller identity |
| Decrypted outbound HTTPS and gRPC | coming in v2.4 | ✓ ([https interception](docs/openresty/https-interception.md)) |
| Status | where development is focused | maintained in parallel |

The Lua module runs as an OpenResty sidecar; try it with Prometheus and Grafana
in one command in the [quickstart](examples/quickstart/), and see its
[guides](docs/openresty/README.md) and [Kubernetes example](examples/openresty-sidecar/).

## Images and releases

```
ghcr.io/hatam-abolghasemi/nginx:<nginx-tag>
ghcr.io/hatam-abolghasemi/openresty:<openresty-tag>-<flavor>
```

Each image is its upstream rebuilt, smoke-tested and published under
upstream's own tags. Tags and pinning: [docs/images.md](docs/images.md).

GitHub **Releases** (`vX.Y.Z`) are this project's own versions: they carry
the prebuilt `.so` files, and their notes are the [changelog](CHANGELOG.md)
section, with what to check when upgrading.

## Documentation

- [Documentation index](docs/README.md): both implementations side by side
- [FAQ](docs/openresty/faq.md): why not Istio, OpenTelemetry or eBPF, limits, costs
- [Changelog](CHANGELOG.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Discussions](https://github.com/hatam-abolghasemi/nginx-prometheus-module/discussions)

| Path | |
|---|---|
| [`nginx/`](nginx/) | The C module: `config`, `src/`, its image (`docker/`), `tests/` |
| [`openresty/`](openresty/) | The Lua module: `lib/`, `vendor/`, its image (`docker/`), `tests/` |
| [`docs/`](docs/) | `nginx/`, `openresty/`, and what both share |
| [`examples/`](examples/) | `nginx-sidecar/`, `openresty-sidecar/`, the Docker `quickstart/` |
| [`grafana/`](grafana/) | The dashboard |

## Built on

The C module is plain C against nginx's own API. In the Lua module, the
Prometheus client internals (counter/gauge/histogram storage and the
exposition-format serializer) are vendored **unmodified** from
[knyar/nginx-lua-prometheus](https://github.com/knyar/nginx-lua-prometheus)
(MIT, © Anton Tolchanov), under
[`openresty/vendor/nginx-lua-prometheus/`](openresty/vendor/nginx-lua-prometheus/);
the Lua module adds route sanitization, source attribution, the proxies and
https interception on top of that library's public API.

## License

MIT ([`LICENSE`](LICENSE)). Vendored code under `openresty/vendor/` keeps its
own license ([`openresty/vendor/nginx-lua-prometheus/LICENSE`](openresty/vendor/nginx-lua-prometheus/LICENSE)).
