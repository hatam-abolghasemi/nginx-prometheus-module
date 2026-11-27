# Changelog

This project's own code, grouped by release. Each release lists its dated
entries, newest first. The vendored library under `openresty/vendor/nginx-lua-prometheus/`
has its own `CHANGELOG.md`.

A release's section here is also its GitHub Release notes: pushing a `vX.Y.Z`
tag publishes the `## vX.Y.Z` section below (`.github/scripts/release-notes.sh`).
Image tags follow the upstream images (nginx, docker-openresty), not these
versions; see [docs/images.md](docs/images.md).

New entries go under **Unreleased** until the next tag.

## Unreleased

- **CI:** the sanitizer (ASan/UBSan) build runs on the newest mainline only;
  the stable line keeps its regular build. nginx 1.30's own core trips UBSan
  at startup (`memcpy(dst, NULL, 0)`, fixed in 1.31), so that job could never
  test this module; `nginx/tests/ubsan.supp` is gone with it. The sidecar
  test gets the sanitizer settings through `sudo` by name: the runner's
  `sudo` no longer keeps the environment with `-E`, which made both
  sanitizer jobs fail.

## v2.7.0 — 2026-11-27

### 2026-11-27

- **The module as a package:** `ghcr.io/hatam-abolghasemi/nginx-prometheus-module`
  holds just the two `.so` files, under the nginx image's tags
  (`<version>` for Debian/glibc, `<version>-alpine` for musl), on every
  platform upstream nginx is published for.
  Into your own image: `COPY --from=ghcr.io/hatam-abolghasemi/nginx-prometheus-module:1.31.10-alpine / /usr/lib/nginx/modules/`.
  [docs/images.md](docs/images.md#the-module).
- **Upgrading: nginx `latest` is now Alpine**, as in the OpenResty image:
  `latest` and `alpine` are the newest mainline on Alpine. For Debian, pin
  `mainline` or a version such as `1.31.10`.
- **The images follow upstream rebuilds too:** besides new nginx and
  docker-openresty releases, a tag upstream rebuilds on a newer Debian or
  Alpine base (same tag, new digest) is rebuilt within hours. Each image
  records the upstream digest it came from
  (`org.opencontainers.image.base.digest`). The first scheduled run after this
  release rebuilds every current tag once, to record it.
- **The package pages show the newest Alpine first**, with a description:
  `latest` / `alpine` are pushed last.
- **Docs in the order you'd use them:** the module into your own nginx, then
  the nginx image, then the OpenResty image.
- **CI:** the sanitizer job on nginx 1.30 (the stable line) is green again.
  nginx 1.30 core itself trips UBSan at startup (`memcpy(dst, NULL, 0)` in
  `ngx_pstrdup`, fixed in 1.31), which stopped nginx before any test ran;
  `nginx/tests/ubsan.supp` suppresses that one report. Nothing in it names
  this module's code.

## v2.6.1 — 2026-11-27

### 2026-11-27

- **Deprecated:** the old image names (`…/nginx-prometheus-module/nginx`,
  `…/nginx-prometheus-module/openresty`, `openresty-prometheus-exporter`) and
  the flavor tags no longer built since v2.3.1 (nginx: anything but default
  and `alpine`; OpenResty: anything but `alpine` and `bookworm`). They will be
  deleted from ghcr.io; move to `ghcr.io/hatam-abolghasemi/nginx` and
  `…/openresty`, main flavors, or build another flavor yourself.
  [docs/images.md](docs/images.md#deprecated).
- **The defaults stay:** `latest` and `alpine` are the newest mainline, which
  has https outbound and interception; `stable[-alpine]` is the newest stable.
- `.github/scripts/prune-images.sh`: lists what's deprecated on ghcr.io
  (`--delete` deletes it); untagged per-platform manifests are never touched.

## v2.6.0 — 2026-11-27

### 2026-11-27

- **The C image is as easy as the OpenResty one.** New sidecar variables:
  `PROMETHEUS_CONFIG` / `PROMETHEUS_STREAM_CONFIG` take module directives
  with no file to mount (any image, sidecar or not); if `nginx -t` rejects
  them they're left out and logged, and nginx starts anyway.
  `INTERCEPT_BYPASS` (hosts never decrypted, relayed as `CONNECT`, as in the
  Lua image), `INTERCEPT_CA_RELOAD` (a CA rotated in place is picked up
  with a reload, every 60s by default), `INTERCEPT_TLS_PORT` /
  `INTERCEPT_H2C_PORT`, and `IPV6`. Guide:
  [docs/nginx/sidecar.md](docs/nginx/sidecar.md).
- **https interception, configurable and IPv6:**
  `prometheus_intercept_connect tls=<addr> h2c=<addr>;` puts the terminators
  anywhere nginx can connect (`host:port`, `[v6]:port`, `unix:/path`;
  default `127.0.0.1:3129` / `:3130`). IPv6 CONNECT targets
  (`[2001:db8::1]:443`) are relayed and decrypted, with certificates for the
  address, and IPv6 answers are used when the resolver gives them (IPv4
  first). The per-worker pool and certificates are documented as by design.
- **Kubernetes example with interception:**
  [examples/nginx-sidecar-intercept/](examples/nginx-sidecar-intercept/), an
  overlay on the sidecar example; an init container gives the app a trust
  bundle with your CA, so the app doesn't change.
- **The image always has a CA bundle** (installed at build time where a flavor
  has none), so `INTERCEPT_TLS=on` works on every flavor.
- **The quickstart runs either image:**
  `SIDECAR_IMAGE=ghcr.io/hatam-abolghasemi/nginx:mainline-alpine docker compose up -d`.
- **Tests:** real gRPC through interception (grpcs and h2c, unary and
  streaming, `grpc_<code>` from trailers and from a trailers-only answer),
  terminators on a custom port and a unix socket, IPv6 where the runner has
  it. New `nginx/tests/sidecar_test.sh` runs the image's sidecar script with
  the image's layout in namespaces of its own, no Docker: stock, sidecar,
  outbound, stream, interception, bypass, custom ports, CA rotation,
  `PROMETHEUS_CONFIG`. In CI.

## v2.5.0 — 2026-11-27

### 2026-11-27

- **Caller identity on stream sessions in the C module.** With
  `prometheus_label source_service on;` and `prometheus_label source_namespace on;`
  in `stream{}` (off by default, as in the Lua module), every stream metric
  gets `source_service` / `source_namespace` after `status`, and
  `nginx_stream_connections_active` is kept per caller. The dashboard's stream
  panels that group by caller now work with the C module.
- **One resolver for both modules.** The stream module asks the http module,
  found by its zone's name, so it uses the same ranges, PTR cache and DNS
  client as HTTP callers. The two `.so` files still don't link to each other.
  A configured range comes first, then loopback (this pod's own identity, as
  the Lua module does it), then `prometheus_source_default`.
  Guide: [docs/nginx/stream.md](docs/nginx/stream.md#who-opened-the-session).
- **Tests:** `nginx/tests/stream_test.sh` covers PTR, NXDOMAIN, static,
  loopback and per-caller active sessions, plus a reload that turns the
  labels off while a session is open.

## v2.4.0 — 2026-11-27

### 2026-11-27

- **https interception in the C module** (nginx 1.31+). Outbound HTTPS and
  gRPC are recorded with their real method, route and status instead of
  `CONNECT host:port`: `prometheus_intercept_connect` answers the CONNECT,
  reads the tunnel's first bytes, and hands TLS to a terminator on `:3129`
  (a certificate minted per name, signed by your CA) and plaintext HTTP/2 to
  `:3130`; anything else is relayed as before. Decrypted requests go on to
  their destination over TLS verified against the system trust store.
  Guide: [docs/nginx/interception.md](docs/nginx/interception.md).
- **Connections to destinations are kept per name**, not per address
  (`prometheus_intercept_origin`): once warm, an intercepted request costs no
  extra round trip, and a connection verified for one name is never used for
  another on the same IP.
- **gRPC:** an outbound gRPC call's `status` is `grpc_<code>` from
  `grpc-status`, and its `route` is the method path as is, as in the Lua
  module.
- **Sidecar:** `INTERCEPT_TLS=on` with a CA mounted at `/etc/nginx/intercept/`
  (`INTERCEPT_CA_CERT`, `INTERCEPT_CA_KEY`, `INTERCEPT_TRUSTED_CA`).
- **Tests:** `nginx/tests/intercept_test.sh` in CI: decryption, reuse per
  name, no reuse across names, unverified destinations, h2, h2c, relayed
  tunnels.

## v2.3.1 — 2026-11-27

### 2026-11-27

- **Shorter image names:** `ghcr.io/hatam-abolghasemi/nginx` and
  `ghcr.io/hatam-abolghasemi/openresty`, same tags. The old names
  (`…/nginx-prometheus-module/nginx`, `…/nginx-prometheus-module/openresty`)
  stay pullable but get no new builds: change the image name when you upgrade.
- **The main flavors are built, any other is one `docker build`:** nginx
  default (debian) and `alpine`, for mainline and stable; OpenResty `alpine`
  and `bookworm`. Tags for other flavors (`alpine-slim`, `perl`, `otel`,
  `noble`, ...) are no longer published; [build any of them](docs/images.md#any-other-flavor)
  from the same Dockerfile, or dispatch the workflow with `flavors=all`.
- **The dashboard works with the C module too.** Its `namespace` and `pod`
  come from the scrape (the ServiceMonitor), not from the module; the docs
  said otherwise.

## v2.3.0 — 2026-11-27

### 2026-11-27

- **Stream (L4) in the C module.** A second module,
  `ngx_stream_prometheus_module.so`, records every TCP/UDP session a
  `stream{}` server proxies and serves it on the same `:9145/metrics`:
  sessions, bytes to and from the client and the upstream, session duration,
  upstream connect time and the sessions open now, by `destination`,
  `upstream_addr` and `status`, with histograms (session duration, connect
  time, first byte time) opt-in. Same metric names as the Lua module. Name a
  server with `prometheus_destination redis;`; the other directives are the
  http ones (`prometheus_zone`, a 1m `prometheus_stream` zone by default,
  `prometheus_metric`, `prometheus_label`, `prometheus_buckets`,
  `prometheus_gauge_expiry`, `prometheus_record`). Guide: [docs/nginx/stream.md](docs/nginx/stream.md).
- **`nginx_metric_errors_total`** now also counts stream sessions not
  recorded because the stream zone was full.
- **The nginx image** loads both modules, and includes
  `/etc/nginx/main.d/*.conf` at the top level of `nginx.conf`, where
  `stream{}` blocks go. The sidecar takes `STREAM="redis:16379:redis.shop:6379 ..."`:
  one local port per destination, proxied and recorded.
- **Release assets** are now two `.so` per nginx version, libc and
  architecture: `ngx_http_prometheus_module.so` and
  `ngx_stream_prometheus_module.so`. The http one still loads alone, in an
  nginx built without `--with-stream`.
- **README** rewritten around what the module exposes: every metric and
  label, with examples, and how routes are sanitized.
- **One module, two implementations, one directory each.** The C module moved
  from `module/` and `tests/` to `nginx/` (`config`, `src/`,
  `docker/`, `tests/`); the Lua module from `lib/`, `vendor/`, `docker/` and
  `tests/` to `openresty/`. Docs follow: [`docs/nginx/`](docs/nginx/README.md),
  [`docs/openresty/`](docs/openresty/README.md), and a [`docs/README.md`](docs/README.md)
  comparing the two; `ARCHITECTURE.md` is now `docs/openresty/architecture.md`.
  Each image builds from its own directory, and the test workflows are
  `nginx-tests.yml` and `openresty-tests.yml`. Nothing recorded changes.
- **Building the C module from source** now points `--add-dynamic-module` at
  `nginx-prometheus-module/nginx` (was `…/module`).

## v2.2.0 — 2026-11-27

### 2026-11-27

- **Plug and play: `load_module` is the whole setup.** With no
  `prometheus_zone`, a 4m zone is made; with no `prometheus;` location
  anywhere, the module opens `:9145` itself (`prometheus_listen`) and serves
  `/metrics` and `/healthz` there, whatever `nginx.conf` and its servers are.
  That listener is the module's own, not an `http{}` server: nothing in the
  config can shadow it and scrapes are never recorded. If a server already
  listens on the port, nginx still starts and the module says so in the error
  log. `prometheus_listen off` and `prometheus_zone off` opt out.
- **Changed:** a config that loads the module and has a `prometheus_zone` but
  no `prometheus;` location now gets the `:9145` listener (before, nothing was
  served). The nginx image drops its `conf.d/prometheus.conf`, which only
  existed to do this; the image is now upstream plus one `load_module` line and
  the sidecar script.
- **Outbound in the sidecar**: with `APP_PORT`, a forward proxy on
  `127.0.0.1:3128` for the app's egress (`HTTP(S)_PROXY`), recorded as
  `mode="1"`, resolving names with the pod's DNS. Plain HTTP is recorded in
  full; HTTPS as a `CONNECT` tunnel through nginx's tunnel module (nginx 1.31+;
  on the stable line, plain HTTP only and the log says so). `OUTBOUND=off`
  turns it off. Guide: [docs/module-outbound.md](docs/nginx/outbound.md).
- **A CONNECT's route is its target**, `host:port`, with the host classified
  like the `host` label (`$ip`, `kubernetes`), instead of the empty URI.
- Tests: `tests/plug_test.sh` (load_module alone, the listener, a taken port,
  reloads, opting out) and `tests/outbound_test.sh` (plain HTTP and CONNECT
  through a proxy); the image smoke test checks outbound to the internet.

## v2.1.0 — 2026-11-26

### 2026-11-26

- **Caller identity in the module**: `source_service` / `source_namespace`,
  named the way the OpenResty sidecar names them. Kubelet probes by
  `User-Agent`; pods in `prometheus_source_ptr` ranges by reverse DNS
  (`<pod-ip>.<service>.<namespace>.svc.cluster.local`, one Service picked the
  same way every time, `-metrics` / `-headless` passed over); known ranges by
  `prometheus_source_static` (longest wins); everything else by
  `prometheus_source_default`; the mesh waypoint's real caller from its
  `X-Forwarded-Client-Cert`, trusted only once reverse DNS says the peer is the
  waypoint. Lookups never wait in the request path: they start before the
  content, answers are shared by all workers in the zone, re-resolved every
  `prometheus_source_refresh` (15s) and kept until a new answer replaces them.
  The module has its own small DNS client, since nginx's resolver keeps only
  the first PTR record ([docs/module-identity.md](docs/nginx/identity.md)).
- **Own identity**: `prometheus_identity on` finds the pod's IP in
  `/etc/hosts` and its Service by reverse DNS, retrying with backoff. Used for
  the new `app` / `namespace` labels, the `host` label (the pod's own IP shows
  as its Service) and the outbound leg's caller.
- **`client_subnet` label**: the first `X-Forwarded-For` address, else the peer,
  /24 for private ranges and /16 for public.
- All five new labels are **off by default** in the module (the sidecar's
  `source_*` are on by default): a plain nginx shouldn't start asking DNS.
  `prometheus_label <name> on` turns each one on.
- **The nginx image is a sidecar with `APP_PORT`**: `:80` forwards
  transparently to `127.0.0.1:$APP_PORT`, own identity on; `POD_CIDR` adds
  caller identity, `CLUSTER_IP_PREFIX` the `kubernetes` host label. Without
  `APP_PORT` the image is unchanged. Guide: [docs/module-sidecar.md](docs/nginx/sidecar.md),
  example: [`examples/nginx-sidecar/`](examples/nginx-sidecar/).
- Tests: `tests/identity_test.c` (DNS wire format, PTR names, the pick, XFCC,
  `/etc/hosts`, subnets) and `tests/source_test.sh` (every naming path against
  a fake DNS server, `tests/fake_dns.c`, including a refresh relabelling an IP).

## v2.0.0 — 2026-11-26

### 2026-11-26

- **The project is now `nginx-prometheus-module`** (renamed from
  `openresty-prometheus-exporter`; GitHub redirects the old URLs). Its centre is
  a **native nginx module**, `ngx_http_prometheus_module`, in C under
  `module/`: stock nginx records every HTTP request in a shared memory zone
  and serves Prometheus metrics from a `prometheus;` location. No Lua, no
  OpenResty, no exporter process. Setup is `load_module`, `prometheus_zone`
  and one location ([docs/module.md](docs/nginx/README.md)).
- **Same metric names as the sidecar**, so queries and the dashboard carry
  over: `nginx_http_requests_total`, the last-value gauges for duration,
  request and response size (expired by `prometheus_gauge_expiry`, 15s), their
  opt-in histograms, `nginx_http_connections{state}`, `nginx_connection_requests`,
  `nginx_connection_time_seconds`, `nginx_connection_slots_limit` and
  `nginx_metric_errors_total`. Labels: `method`, `route` (the sidecar's route
  sanitizer, same results), `status`, `host` (the sidecar's classifier),
  `mode` (`prometheus_mode`), `protocol`. New: upstream timing of the attempt
  that answered, `nginx_http_upstream_response_time_seconds` (on) plus
  `_histogram`, `nginx_http_upstream_connect_time_seconds` and
  `nginx_http_upstream_header_time_seconds` (off).
- **Not in the module yet** (the OpenResty sidecar keeps them): outbound,
  https interception, stream, `source_service` / `source_namespace`,
  `client_subnet`, `app` / `namespace`. The connection metrics carry no
  `app` / `namespace` labels; Prometheus target labels identify the instance.
- **A new image, `ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx`**:
  every flavor of the newest mainline and stable official nginx, rebuilt with
  the module loaded and metrics on `:9145/metrics`, under nginx's own tags
  (`.github/workflows/nginx-image.yml`, `module/docker/`).
- **Prebuilt `.so` files on every release**, for the newest mainline and stable
  nginx, glibc and musl, amd64 and arm64, with a `.sha256` each. New nginx
  versions get theirs attached to the newest release by the scheduled run.
- **The OpenResty image moves to
  `ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty`**, same tags,
  same contents. Images under the old name stay pullable; new builds go only to
  the new one. Update `image:` when you next upgrade.
- **`docs/images.md` is no longer generated** and committed by the workflow
  (the bot's commits made it a contributor); it now describes both images'
  tag schemes, and the GHCR package pages list the tags and digests.
- Tests: `tests/label_test.c` (the label code on its own) and
  `tests/module_test.sh` (the module in a real nginx, output checked by
  promtool), run by `.github/workflows/module.yml` on the newest mainline and
  stable nginx, with and without AddressSanitizer/UBSan.

**Upgrading:** nothing changes inside the OpenResty image. Switch its `image:`
to the new name; the old one gets no new builds.

### 2026-11-14

- **Interception decodes every HTTP tunnel, not just port 443.** With
  `INTERCEPT_TLS=on`, each CONNECT is classified by the first bytes the app
  sends into it (`lib/connect_tunnel.lua`): TLS on **any** port goes to `:3129`,
  the HTTP/2 preface (plaintext gRPC) to a new `:3130` terminator, anything else
  to the real target unchanged. No per-port or per-host configuration. Off, or
  for a host in `INTERCEPT_BYPASS`, CONNECTs keep nginx's native `tunnel_pass`.
- **gRPC is recorded per call**, over TLS (`:3129` now speaks HTTP/2 and
  forwards `application/grpc` with `grpc_pass` over verified TLS) and plaintext
  (`:3130`, h2c): `route="/package.Service/Method"` (not sanitized),
  `status="grpc_<code>"` from `grpc-status`; a non-200 HTTP status is kept.
- **`intercept_origin.lua`:** `resolve(default_port)` (443 unless given; 80 on
  `:3130`). Names that don't resolve as given are retried with each `search`
  domain from `/etc/resolv.conf`, so short Service names (`svc.namespace`)
  resolve as they do for the app; full names still take one query.
- **New `protocol` label** on HTTP request metrics: `http`, `https`, `grpc`,
  `grpcs` or `tunnel` (a CONNECT that wasn't decoded), taken from what nginx saw
  — the scheme, gRPC by content type — with nothing configured. On by default
  (`http.label_protocol`); it adds one label to every request series, so
  existing series are replaced once on upgrade.
- **Kubelet probes are labelled `kubernetes.health-probe`** (`probe_default_namespace`
  default `default` → `kubernetes`): the kubelet belongs to the cluster, not to
  the `default` namespace. Also applies to unresolved dynamic-range addresses.
- **A `Host` that is the pod's own IP is labelled with the pod's own service**
  (discovered by `self_identity`) instead of `kubernetes`, so kubelet probes and
  direct-to-pod callers show the same host as callers using the Service. Until
  discovery succeeds, or with `discover_identity` off, it stays `kubernetes`.
- New `intercept.tunnel_connect_timeout` (10s) and `intercept.tunnel_idle_timeout`
  (300s) for the detecting tunnel.
- `:3129` request bodies are no longer capped at nginx's 1 MB default
  (`client_max_body_size 0`), as on the rest of the outbound path.

### Upgrading

- **`INTERCEPT_TLS_PORTS` is removed** (ignored if set): TLS is detected on
  every port.
- **`default.conf`'s `:3128` hook changed.** If you mount your own
  `default.conf`, replace the CONNECT block with:
  ```nginx
  set_by_lua_block $connect_mode     { return require("intercept").connect_mode() }
  set_by_lua_block $connect_upstream { return require("intercept").upstream() }
  if ($connect_mode = detect) {
      content_by_lua_block { require("connect_tunnel").run() }
  }
  if ($connect_mode = tunnel) {
      tunnel_pass $connect_upstream;
  }
  ```
- With interception on, an unreachable destination now closes the tunnel after
  `200` instead of answering `502` (the tunnel's first bytes must be read before
  the destination is chosen); the CONNECT metric still shows 502.
- With interception on, TLS on non-443 ports is now decrypted too. A client that
  tunnels non-HTTP TLS through the proxy (a database or broker over TLS) must be
  listed in `INTERCEPT_BYPASS`.
- `:3129` negotiates HTTP/2 with clients that offer it; destinations are still
  reached over HTTP/1.1 (gRPC: HTTP/2).

### 2026-11-05

- **FAQ and design decisions.** New `docs/faq.md`: short answers on how this
  compares with Istio, OpenTelemetry SDKs, eBPF agents and access logs, the
  sidecar choice, failure modes, latency, caller identity and its gaps, route
  sanitizing, why no traces, histograms, egress coverage, cost and operations.
  New `docs/design-decisions.md` holds the reasoning and trade-offs behind each
  answer, including why OpenResty rather than stock nginx with the Lua module. Linked from the README and the docs index. No code change.

## v1.1.0 — 2026-11-05

Leaner per-pod defaults, configuration through one env var, caller attribution
that holds up under load, and a dashboard for every metric.

### Upgrading from v1.0.0

Several defaults changed. Check these before rolling the new image out:

- **Every histogram is off.** Percentile queries and the dashboard's p50/p95/p99
  panels show nothing until you turn the ones you use back on, e.g.
  `METRICS_CONFIG="http.metric_latency_hist=true"`. Counters and last-value
  gauges are unchanged.
- **https interception is loaded by the image's entrypoint**, and only when the
  container starts with `INTERCEPT_TLS=on`. If your Deployment sets `command:`
  on the nginx container, remove it (or start it with
  `/usr/local/bin/openresty-exporter-entrypoint`), or interception stays off.
  `intercept.conf` moved from `conf.d/` to `optional/`.
- **No access log** and **`worker_connections 1024`** (was 4096) in the image's
  `nginx.conf`. A mounted `nginx.conf` keeps its own values.
- **Kubelet probes** (`User-Agent: kube-probe/…`) are labelled
  `source_service="health-probe"`, `source_namespace="default"`, whatever their
  source IP.
- **Settings renamed or retuned** in `metrics_config.lua`: `ptr_cache_ttl` is
  gone (PTR answers are now kept until replaced; see `ptr_refresh_interval`,
  `ptr_forget_after`, `ptr_sync_timeout`), `ptr_dns_timeout` 200 → 2000 ms (it
  no longer runs on the request path), `expiry_window` 10 → 15 s. If you mount
  your own `metrics_config.lua`, re-copy it from this release and re-apply your
  values, or new settings will be missing.

### Highlights

- `METRICS_CONFIG` env var overrides simple config values without mounting a file.
- `nginx_connection_slots_limit` metric; slot usage vs limit on the dashboard.
- PTR lookups moved off the request path, refreshed in the background.
- Intercepted HTTPS re-originates over pooled, per-host keep-alive connections.
- Grafana dashboard covering every metric, incl. HTTPS interception and exporter
  health tabs.
- `latest` is the alpine flavor; `docs/images.md` is generated from the registry;
  failed image runs are retried.

### 2026-11-05

Leaner defaults: things a sidecar pays for on every pod, whether it uses them or
not, are now off until asked for.

- **Every histogram is off by default.** `http.metric_latency_hist`,
  `stream.metric_session_duration`, `stream.metric_upstream_connect_time_hist`
  and `stream.metric_upstream_first_byte_time` join the size and connection
  histograms, which were already off. A histogram is (buckets + 3) series per
  label combination, the largest share of the `prometheus_metrics` shared dict
  and of the scrape. Counters and last-value gauges are unchanged. Turn one back
  on with e.g. `METRICS_CONFIG="http.metric_latency_hist=true"`. The
  dashboard's percentile panels (Summary p95, Latency percentiles, L4
  percentiles) show *No data* until you do.
- **https interception is only loaded when it's on.** `intercept.conf` moved
  from `conf.d/` to `optional/` (`/etc/nginx/optional/` in the image). A new
  entrypoint (`docker/entrypoint.sh`) links it into `conf.d/` when the
  container starts with `INTERCEPT_TLS=on`, then execs the command. With it off
  there's no `:3129` listener, OpenSSL context, system CA bundle in memory or
  `ssl_mint_cache`. The decision is made once per container start: changing
  `INTERCEPT_TLS` needs a restart, not a reload. A Deployment that sets
  `command:` skips the entrypoint; `lib/intercept.lua` then keeps interception
  off and logs one error, instead of sending CONNECTs to a port nobody listens
  on. Use `args:`, or start `command:` with the entrypoint. The image restates
  `CMD ["openresty", "-g", "daemon off;"]` (setting ENTRYPOINT clears the base
  CMD), and the build fails if `openresty` isn't on PATH.
- **Access log off by default.** The metrics already cover every request; per
  request log lines cost a write each plus log-pipeline storage. To get them
  back for one server, add
  `access_log /dev/stdout combined buffer=64k flush=1s;` inside its `server {}`.
- **`worker_connections` 4096 → 1024** in the image's `nginx.conf`. Slots are
  preallocated (~0.45 KB each), and a sidecar in front of one app rarely needs
  more. `nginx_connection_slots_limit` now reads 1024; raise it in a mounted
  `nginx.conf` for high-traffic services.
- **Fix: kubelet probes were labelled as their node's subnet.** A probe
  connects from the node's own IP, so with the node range in `static_ranges`,
  `/health` traffic showed up as e.g. `source_service="internal"`,
  `source_namespace="zone_d"`. Requests with `User-Agent: kube-probe/…` (every
  kubelet probe sends it) are now labelled `probe_default_service` /
  `probe_default_namespace` (`health-probe` / `default`) whatever their source
  IP, with no lookup. No path is assumed. New `source_attribution.is_probe()`;
  settings `http.probe_by_user_agent` (default on) and
  `http.probe_user_agent_prefix` (`kube-probe/`).
- Tests: defaults keep every histogram off; `intercept` routes to `:3129` only
  when on *and* loaded. The image smoke test now asserts no histogram, no
  access-log line and no `:3129` by default, `:3129` present with
  `INTERCEPT_TLS=on`, and that `METRICS_CONFIG` turns the latency histogram on.

### 2026-11-03

- **New metric: `nginx_connection_slots_limit{app, namespace}`.** The
  connection-slot ceiling, `worker_connections` × worker processes, so slots in
  use can be read as a share of the limit. The dashboard's Connections tab
  already queried it, but the exporter never emitted it, so "Slot limit per pod"
  read 0 and the utilization panels stayed empty. `worker_connections` comes
  from the new `http.worker_connections` setting when > 0 (also via
  `METRICS_CONFIG`), otherwise from the directive in `http.nginx_conf_path`
  (default `/etc/nginx/nginx.conf`, the image's config), read once per worker at
  startup. If neither works, the metric isn't exported and worker 0 logs one
  warning; nothing is guessed. New `lib/connection_slots.lua`; on by default
  (`http.metric_connection_slots_limit`), one series per pod. In the image it
  reads 4096. The image smoke test asserts it, and the override through
  `METRICS_CONFIG`.
- **Quickstart on plain Docker.** New `examples/quickstart/`: `docker compose up`
  brings up a demo app with the exporter as its sidecar, generated inbound and
  outbound traffic, Prometheus, and Grafana with the dashboard provisioned. The
  README gains a dashboard screenshot, a "Try it in a minute" section and status badges. No code or
  image change.
- **Dashboard covers every metric.** New tabs: *HTTPS interception* (visible vs
  opaque outbound, a ranked table of tunnelled `host:port`s) and *Exporter
  health* (`nginx_metric_errors_total`, series per pod, route/host/caller
  cardinality). New panels: inbound by caller and outbound by destination host
  (Request breakdown); request/response size percentiles (Payload sizes) and
  connection histograms (Connections), both off-by-default metrics; stream
  throughput on both legs, time to first byte and open sessions per destination
  (L4 streams). Method, mode and destination cells are colour-coded.
- **Fix: L4 connect-time p50/p95/p99** for status classes 1xx/3xx/4xx/5xx queried
  `nginx_stream_session_duration_seconds_bucket`, which doesn't exist; they now
  use `nginx_stream_upstream_connect_time_seconds_histogram_bucket` like the 2xx
  series.

### 2026-11-02

- **`METRICS_CONFIG`: override simple settings from an env var.** A Deployment
  can now flip a switch without mounting a full copy of `metrics_config.lua`:
  `METRICS_CONFIG="http.metric_latency_hist=false;http.expiry_window=30"`.
  Entries are `path=value`, separated by `;` or newlines, applied on top of
  whichever `metrics_config.lua` is loaded (baked or mounted). Only existing
  boolean/number/string settings can be set, plus
  `apps.stream.<destination>.metrics_enabled` for any destination; the value
  must parse as the setting's type. An unknown path (e.g. a typo), a wrong type
  or a table setting is logged at `error` and skipped; it never stops nginx.
  New `lib/config_env.lua`; `http_metrics`, `stream_metrics`, `tls_intercept`
  and `intercept_origin` now read config through it. The image declares
  `env METRICS_CONFIG;` in `main.d/metrics-config-env.conf`. No default changed.

### 2026-10-30

- **Fewer shared-dict writes per request.** The vendored library writes every
  gauge `set()` straight to the shared dict under its lock (counters and
  histograms are batched per worker). `record()` wrote the four
  `nginx_http_connections` states on every request; they're a server-wide
  snapshot, so `collect()` now sets all four once per scrape (it previously
  skipped `active`). 4 of the ~11 locked writes per request are gone.
- **Expired gauges are deleted, not zeroed.** `gauge_expiry`'s callbacks in
  `http_metrics` and `stream_metrics` now `del()` a quiet series instead of
  `set(0)`, so it drops out of `/metrics` and the dict instead of lingering as
  `0` forever. Panels that relied on seeing `0` for idle series now get no data.
  The sweeper runs on worker 0 only (the dict is shared; `get_keys()` locks it).
- **`route_sanitizer` no longer allocates per path segment.** The merged options
  table is memoised per caller opts table instead of rebuilt for every segment
  of every request.
- **Access log is buffered** (`buffer=64k flush=1s`): one write per batch
  instead of a `write()` syscall per request; lines can appear up to 1s late.
- **`APP_PORT` read once per worker.** The inbound balancer moved to
  `lib/app_peer.lua`, which reads it at load instead of per request.
- **One resolver/identity builder for HTTP and stream.** `stream_metrics.init()`
  repeated `http_metrics.init()`'s resolver and own-identity setup line for
  line. Both now call `lib/caller_identity.lua`'s `build()`. No behavior
  change: same config keys, same defaults, each subsystem keeps its own dict.

### 2026-10-28

- **`probe_default_namespace` is now used.** The resolver paired
  `probe_default_service` with `default_namespace` (the namespace for public
  IPs), so unresolved pod-range callers showed as `unknown.health-probe`
  instead of `default.health-probe`. New resolver option
  `default_service_namespace` carries `probe_default_namespace`; public IPs
  keep `default_namespace`.
- **Intercepted HTTPS no longer reconnects to the destination per request.**
  `:3129` re-originated with `proxy_pass https://$host…`, which keeps no
  connection pool, so every decrypted request paid a new TCP + TLS handshake to
  the real destination: about two round trips added to each call (+45 ms p50
  against a destination 20 ms away). New `lib/intercept_origin.lua` resolves the
  destination and pools connections per hostname; the `upstream
  intercept_origin` block in `intercept.conf` sets `keepalive 0` so nginx's
  implicit IP-keyed pool can't hand one host's verified connection to another.
  New `intercept.*` keys: `dns_server`, `dns_timeout`, `dns_cache_ttl`,
  `pool_idle_timeout`, `pool_max_requests`.
- **Intercepted non-443 ports are re-originated on their own port.** `$host`
  drops the port, so a port added to `INTERCEPT_TLS_PORTS` was re-sent to 443.
  The port now comes from the `Host` header.
- **Gauge expiry window raised from 10s to 15s** (`http.expiry_window`,
  `stream.expiry_window`, and the `gauge_expiry` default). With a 15s scrape
  interval and the 5s sweep, about 1 in 6 isolated writes to a quiet series
  was zeroed before any scrape saw it. At 15s every write is scraped at least
  once, and about 1 in 6 is scraped twice. Keep `expiry_window` equal to the
  scrape interval.

### 2026-10-27

- **Caller identity no longer expires.** The PTR cache dropped every answer
  after 15s and labelled the next request with the fallback, so any caller
  slower than one request per 15s per peer IP (e.g. traffic through two
  ingress replicas once a minute) always showed as `unknown.health-probe`.
  Answers are now kept until a new answer replaces them; one worker
  re-resolves every known IP each `ptr_refresh_interval` (15s, keep it at or
  below the scrape interval), so a reassigned IP is relabelled within one
  scrape. NXDOMAIN resets an IP to the fallback; timeouts and SERVFAIL keep
  the previous answer. Only IPs that sent a request in the last `ptr_forget_after` (5 min) are refreshed; idle ones are dropped.
- **New IPs resolve inline** (`ptr_sync_timeout`, 100ms) in the access/preread
  phase instead of the first request always getting the fallback.
- **Stable name when an IP has several PTR records** (one per Service selecting
  the pod): the alphabetically first name not matching `ptr_ignore_services`
  (default `-metrics`, `-headless`) is used, instead of whichever answer DNS
  listed first.
- `ptr_cache_ttl` is replaced by `ptr_refresh_interval` and still read as a
  fallback.

### 2026-10-23

- **`latest` is now Alpine** (was bookworm, as upstream). The flavor is one
  variable in the image workflow, `LATEST_FLAVOR`.
- **[`docs/images.md`](docs/images.md)** lists the newest release, every
  flavor's tags and platforms, and pull commands by tag and by digest. The
  image workflow's new `catalog` job regenerates it from GHCR and commits it
  when it changes (it never committed before, so a new release such as
  1.31.1.1-5 left the docs behind). Hand-written docs and examples no longer
  name a release.
- **Failed image runs are retried**: `retry.yml` re-runs the failed jobs up to
  2 times.

### 2026-10-21 (after v1.0.0)

- **Grafana dashboard** in [`grafana/`](grafana/): a v2-schema dashboard
  (Grafana 12+) with one tab per area — Summary, Request breakdown, Latency
  percentiles, Last observed latency, Payload sizes, Connections, L4 streams.
  Dashboard-wide filters are only data source and namespace; host, route and
  status live on the tabs that use them (L4 streams lists stream status codes).
  Linked from the README and the docs index.

## v1.0.0 — 2026-10-21

First release: inbound and outbound L7 metrics, caller identity, https
interception, L4 stream metrics, one image per docker-openresty release.

### 2026-10-21

- **Every docker-openresty flavor, same tags.** The image workflow no longer
  builds only Alpine: for each release it reads the flavors and platforms
  upstream published on Docker Hub (alpine, alpine-slim, alpine-apk,
  alpine-fat, bookworm, bookworm-fat, noble, jammy, rocky, alma, fedora, …)
  and builds each one on the same platforms (amd64, arm64, s390x where
  upstream has it), one matrix job per flavor with its own smoke test. Tags
  are upstream's: `<release>-<flavor>`, `<version>-<flavor>`,
  `<series>-<flavor>`, `<flavor>`, and `latest` (= bookworm, as upstream).
  The old flavorless tags (`:1.31.1.1-4`, `:1.31.1.1`) and the `-<sha>` tags
  are no longer published; pin by digest instead.
- `docker/Dockerfile` is flavor-neutral: `OPENRESTY_TAG` now includes the
  flavor; `docker/setup.sh` finds the flavor's config path from `nginx -V`
  (covers `-debug`/`-valgrind` prefixes) and links the distro CA bundle to
  `/etc/nginx/intercept/trusted-ca.crt`; the fallback certificate is made in a
  build stage, so no flavor needs openssl. `nginx.conf` includes `mime.types`
  relative to the config dir. Leftover `/etc/nginx/intercept/{ports,bypass}`
  dirs removed.
- **Features are named, not numbered**: inbound, outbound, https interception
  (depends on outbound), stream. Guides renamed to `docs/inbound.md`,
  `docs/outbound.md`, `docs/https-interception.md`, `docs/stream.md`.
  `metrics_config.apps.forward_proxy` is now `apps.outbound` — rename it in a
  mounted config (an old key is ignored, i.e. outbound metrics stay on).
- Fixed `lib/http_metrics.lua` filing label values under the wrong names when
  `label_app`/`label_namespace` were on (schema and values were built in
  different orders). Both now come from one ordered list.
- Fixed the stream exposition server answering scrapes with bare text instead
  of an HTTP response: `stream_metrics.collect()` now speaks HTTP;
  `metric_data()` still returns the raw text. Added the
  `stream.label_source_*` flags to `metrics_config.lua` (off).
- `examples/openresty-sidecar/` rebuilt around the image: a kustomization that
  mounts only `stream.conf` (+ optional `metrics_config.lua`), the
  ServiceMonitor scraping `:9145` instead of `:80`. The example `nginx.conf` /
  `default.conf` copies (which had drifted, and used `--` comments nginx can't
  parse) are gone — `docker/` is the reference config.
- The image smoke test now also covers https interception (throwaway CA,
  `INTERCEPT_TLS=on`, a decrypted `GET` recorded once, no `CONNECT` line); it
  previously only proved the CA-less default starts.
- Added `tests/lib_test.lua` (pure modules + HTTP label pairing) and a `lib` job
  in `Run Tests`; refreshed the vendored-suite jobs' action versions.
- The changelog moved here from `ARCHITECTURE.md`.

### 2026-10-20

- **Optional HTTPS interception** for the forward proxy — record the real
  method/route/status of outbound HTTPS instead of only `CONNECT host:port`.
  The machinery is baked into the image: `lib/intercept.lua` (the per-CONNECT
  decision), `conf.d/intercept.conf` (the `ssl_mint_cache` dict and the
  `127.0.0.1:3129` TLS terminator, which mints per-SNI leaf certs via
  `lib/tls_intercept.lua` and re-originates over verified TLS), and
  `main.d/intercept-env.conf` (declares the env switches for workers). An app's
  `default.conf` opts in with one line in its `:3128` block —
  `tunnel_pass $connect_upstream;` fed by `require("intercept").upstream()` —
  so no maps, dict or `:3129` server live in `default.conf`. Off by default:
  with `INTERCEPT_TLS` unset the proxy passes CONNECT through unchanged and
  `:3129` stays idle. Enable per deployment with `INTERCEPT_TLS=on` plus a
  mounted CA (nginx gets `ca.crt`+`ca.key`; the app trusts `ca.crt`); ports
  default to 443 (`INTERCEPT_TLS_PORTS`), mTLS/pinned hosts go in
  `INTERCEPT_BYPASS`. `record()` counts each call once via `intercept.should()`,
  so routing and metrics share one decision. See "Seeing outgoing HTTPS
  details".

### 2026-10-19

- The image is now `FROM openresty/openresty:<tag>-alpine` (official, current
  Alpine) instead of compiling docker-openresty's `alpine/Dockerfile`:
  `docker-bake.hcl` is gone, builds take seconds, and a release is built once
  its official image is on Docker Hub. The forward proxy stays on nginx's
  native CONNECT support, with no third-party module.
- `lib/http_metrics.lua`: requests nginx rejects before the access phase (400,
  405, 413, `deny`) are recorded with `source_service="unknown"` instead of
  failing with "incorrect label count".

### 2026-10-17

- Added `.github/workflows/openresty-image.yml`, `docker-bake.hcl` and
  `docker/`: an image built from each new docker-openresty release with this
  repo baked in (see "Prebuilt image" above).
- `lib/metrics_config.lua`: restored the `own_namespace` lookup (lost in
  `4da5d7e`, which left `http.namespace` as `nil` and made every
  connection metric fail with "incorrect label count"), and
  `ptr_dns_server` now defaults to the first nameserver in
  `/etc/resolv.conf` instead of a hardcoded IP.

### 2026-09-20

- Fixed `nginx_stream_connections_active` always reporting 0. It was read
  from `$connections_active` and friends, which come from
  `ngx_http_stub_status_module` and don't exist in the stream module (the
  `or 0` fallback hid that). The gauge is now maintained by hand: `+1` from
  the new `stream_metrics.connection_open()` (call it from
  `preread_by_lua_block` in every stream `server{}`), `-1` from `record()`.
  Only `state="active"` is exported now; `reading`/`writing`/`waiting` have
  no stream equivalent and were removed. A `server{}` without the preread
  hook is simply not counted.
- Added `lib/self_identity.lua` and `Resolver:resolve_ptr()`: the pod's own
  service/namespace is now discovered via PTR instead of read from
  `metrics_config.lua`'s `http.app`/`http.namespace` (kept as fallback; turn off
  with `http.discover_identity = false`). Fixes every pod sharing a config
  reporting itself as one hard-coded service. `http_metrics.lua` now reads
  identity through `own()` for the outbound `source_service`, the `app` and
  `namespace` labels, and the connection gauges.

### 2026-09-18

- Added `lib/app_toggle.lua`, wired into `default.conf` (forward-proxy leg)
  and `stream.conf` (per-destination).
- Added `examples/openresty-sidecar/stream.conf`: full TCP/UDP proxy
  metrics, reusing `lib/gauge_expiry.lua`.
- Added `examples/openresty-sidecar/nginx.conf` and
  `examples/openresty-sidecar/k8s/` (mock-data ConfigMaps, a sample sidecar
  Deployment, a Service/ServiceMonitor pair) so the example deploys
  end-to-end instead of just being read.
- Restructured `default.conf`/`stream.conf` down to wiring + `server{}`
  blocks only (397 → 91 and 231 → 60 lines). Added `lib/metrics_config.lua`
  (all environment-specific values) and `lib/http_metrics.lua`/
  `lib/stream_metrics.lua` (the orchestration that used to be inline,
  now proper modules built on the existing pure `lib/` modules).
