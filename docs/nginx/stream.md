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
  image: ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:mainline-alpine
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

## Limits

- The app must connect to the local port; something with the upstream
  address hard-coded bypasses it.
- `upstream_addr` is the address reached, one series per address: behind a
  Kubernetes Service that's its ClusterIP, stable. Turn it off with
  `prometheus_label upstream_addr off;` if your upstreams change addresses.
- TLS to the upstream (`proxy_ssl on`) is fine; the session is still counted.
- Caller identity (`source_service` on stream sessions) isn't in the C module
  yet; the Lua module has it, off by default.
- No `stream{}` without an `http{}` with the module: stream metrics are
  served by the http module.
