# stream — metrics for Redis, databases and anything else over TCP

The traffic your app sends that isn't HTTP: Redis, PostgreSQL, MySQL,
RabbitMQ, Kafka, any TCP or UDP. nginx's stream module proxies it, and
`ngx_stream_prometheus_module` records every session: how many, how many
bytes each way, how long, how fast the upstream answered, and which ones
failed to connect. On the same `:9145/metrics` as everything else.

Needs nothing else; the [sidecar](sidecar.md) makes it one variable,
`STREAM`.

---

## What you get

```
nginx_stream_connections_total{destination="redis",upstream_addr="10.233.12.7:6379",status="200"} 418
nginx_stream_connections_total{destination="orders-db",upstream_addr="10.233.40.2:5432",status="502"} 3
nginx_stream_upstream_connect_time_seconds{destination="redis",upstream_addr="10.233.12.7:6379",status="200"} 0.001
nginx_stream_session_duration_seconds{destination="redis",upstream_addr="10.233.12.7:6379",status="200"} 12.4
nginx_stream_upstream_bytes_received_total{destination="redis",upstream_addr="10.233.12.7:6379",status="200"} 9310000
nginx_stream_connections_active{state="active"} 6
```

- `destination` is the name you give the server (`prometheus_destination`),
  `upstream_addr` the address it reached, `status` `200` for a session that
  ran, `502` when the upstream couldn't be reached.
- A session is recorded when it closes: a pooled connection that stays open
  for an hour is one session, counted after that hour. `connections_active`
  shows what's open right now.
- What's *inside* the session (Redis commands, SQL) isn't seen: this is L4.

Every metric and its default: [README.md](README.md#stream-metrics).

---

## Step by step

### With the sidecar image

One entry per destination, `<name>:<local port>:<host>:<port>`:

```yaml
- name: nginx
  image: ghcr.io/hatam-abolghasemi/nginx:mainline-alpine
  env:
    - { name: APP_PORT, value: "8080" }
    - { name: STREAM,   value: "redis:16379:redis-master.data.svc:6379 orders-db:15432:orders-db.data.svc:5432" }
```

Then point the app's clients at the local ports: `REDIS_ADDR=127.0.0.1:16379`,
`DB_HOST=127.0.0.1 DB_PORT=15432`. Each entry becomes a stream server on
`127.0.0.1:<local port>` proxying to `<host>:<port>`, in
`/etc/nginx/main.d/sidecar-stream.conf`.

### On your own nginx

```nginx
load_module modules/ngx_http_prometheus_module.so;
load_module modules/ngx_stream_prometheus_module.so;

stream {
    server {
        listen                  127.0.0.1:16379;
        prometheus_destination  redis;
        proxy_pass              redis-master.data.svc:6379;
    }
}
```

Both modules are needed: the stream one records, the http one serves
`/metrics`. The stream zone (1m by default) is made as soon as there is a
`stream{}` block; `prometheus_record off;` leaves a server out.

### Check it

```bash
redis-cli -h 127.0.0.1 -p 16379 ping
curl -s localhost:9145/metrics | grep '^nginx_stream_connections_total'
```

---

## Who opened the session

`source_service` and `source_namespace`, off by default as in the Lua
module, the same meaning as on the HTTP metrics:

```nginx
stream {
    prometheus_label source_service   on;
    prometheus_label source_namespace on;
    # servers as above
}
```

```
nginx_stream_connections_total{destination="redis",upstream_addr="10.233.12.7:6379",status="200",source_service="checkout",source_namespace="shop"} 418
nginx_stream_connections_active{state="active",source_service="checkout",source_namespace="shop"} 6
```

The caller is worked out by the http module, with its
[caller identity](identity.md) settings in `http{}`: the same
`prometheus_source_static` and `prometheus_source_ptr` ranges, PTR cache and
DNS client, `prometheus_source_default` and `prometheus_source_unresolved`.
In this order:

1. an address in a `prometheus_source_ptr` range: its Service by reverse DNS
   (unresolved until the answer is there);
2. an address in a `prometheus_source_static` range: that range's labels;
3. loopback or a unix socket: this pod itself, its [own identity](identity.md#own-identity)
   (`app` / `namespace`). In the sidecar, where the app connects to
   `127.0.0.1:<port>`, that is every session;
4. anything else: `prometheus_source_default`.

A new address's lookup starts when its first session opens; the labels are
read when it closes, so even that first session is usually resolved.
`connections_active` uses what's known when a session opens.

There's no User-Agent or `X-Forwarded-Client-Cert` on L4, so no probe rule and
no waypoint unwrapping: a session through the mesh waypoint is labelled
`waypoint`.

With the source labels on, `connections_active` is one series per caller
(`state="active"` plus the two labels), and nothing until a first session.
Turning them on or off starts the stream counts over on the next reload.

---

## Limits

- The app must connect to the local port; something with the upstream
  address hard-coded bypasses it.
- `upstream_addr` is the address reached, one series per address: behind a
  Kubernetes Service that's its ClusterIP, stable. Turn it off with
  `prometheus_label upstream_addr off;` if your upstreams change addresses.
- TLS to the upstream (`proxy_ssl on`) is fine; the session is still counted.
- Caller identity on stream needs the http module's zone; without it every
  caller is `unknown` (and a warning is logged at start).
- PTR callers are only resolved when caller identity is on in `http{}` too
  (`source_*` labels and `prometheus_source_ptr`): that's what starts the DNS
  client. Static ranges, loopback and the default work either way.
- No `stream{}` without an `http{}` with the module: stream metrics are
  served by the http module.
