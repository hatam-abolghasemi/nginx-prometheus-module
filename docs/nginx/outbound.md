# outbound — metrics for the calls your app makes

The same metrics for traffic **leaving** your app, recorded as `mode="1"`:
which hosts it calls, how often, how fast, and what comes back. The app opts
in by sending its egress through the sidecar's forward proxy on
`127.0.0.1:3128`; nothing else in it changes.

Needs the [sidecar](sidecar.md) (on by default there), or the server
below on your own nginx. [**https interception**](interception.md) builds on
this to see inside HTTPS calls.

---

## What you get

```
# plain HTTP: the full picture
nginx_http_requests_total{method="GET",route="/v1/rates",status="200",host="api.partner.com",mode="1",protocol="http"} 310
# HTTPS: the tunnel, host and port as the route, 200 = tunnel established
nginx_http_requests_total{method="CONNECT",route="api.partner.com:443",status="200",host="api.partner.com",mode="1",protocol="tunnel"} 52
nginx_http_requests_total{method="CONNECT",route="db.example.com:443",status="502",host="db.example.com",mode="1",protocol="tunnel"} 3
```

- **Plain HTTP** is recorded in full: method, sanitized route, the real status,
  duration, sizes, and `nginx_http_upstream_response_time_seconds` (the remote
  side's time).
- **HTTPS** is a `CONNECT` tunnel: you see *which host and port*, *how often*,
  *how long the tunnel lived* (duration) and whether it could be opened (`502`
  when the target refused or didn't answer). Method, route and status
  *inside* the tunnel are encrypted; [interception](interception.md) decodes them.
- With [caller identity](identity.md) on, `source_service` /
  `source_namespace` on outbound series are the pod itself.

An IP target is shown as `$ip` (or `kubernetes` under
`prometheus_cluster_ip_prefix`) in both `host` and the CONNECT route, so one
series per address never happens.

**HTTPS needs nginx 1.31 or later** (its tunnel module, built in). On the
stable line, outbound records plain HTTP only, and the sidecar's log says so.

---

## Step by step

### 1. The proxy

**With the sidecar:** nothing to do, it's on with `APP_PORT`. The log line:

```
/docker-entrypoint.d/40-prometheus-sidecar.sh: sidecar for 127.0.0.1:8080, outbound on 127.0.0.1:3128
```

`OUTBOUND=off` turns it off.

**On your own nginx:**

```nginx
server {
    listen       127.0.0.1:3128;
    server_name  outbound;

    resolver     10.233.0.3 ipv6=off valid=10s;    # the pod's DNS: /etc/resolv.conf
    prometheus_mode 1;

    location / {
        if ($request_method = CONNECT) {
            tunnel_pass;                           # nginx 1.31+
        }

        proxy_pass          http://$http_host$request_uri;
        proxy_http_version  1.1;
        proxy_set_header    Host $http_host;
        proxy_set_header    Proxy-Connection "";
        proxy_set_header    Proxy-Authorization "";
    }
}
```

Keep it on loopback: it's an open proxy to anything the pod can reach.

### 2. Point the app at it

Most HTTP clients and runtimes read these (set both cases, libraries
disagree):

```yaml
- name: app
  env:
    - { name: HTTP_PROXY,  value: "http://127.0.0.1:3128" }
    - { name: HTTPS_PROXY, value: "http://127.0.0.1:3128" }
    - { name: NO_PROXY,    value: "127.0.0.1,localhost,.svc,.cluster.local" }
    - { name: http_proxy,  value: "http://127.0.0.1:3128" }
    - { name: https_proxy, value: "http://127.0.0.1:3128" }
    - { name: no_proxy,    value: "127.0.0.1,localhost,.svc,.cluster.local" }
```

Leave in-cluster names in `NO_PROXY` if you only want egress to the outside;
take them out to see service-to-service calls from the caller's side too.

### 3. Check it

```bash
curl -s localhost:9145/metrics | grep 'mode="1"'
```

---

## Limits

- Only clients that honour the proxy variables go through it. Something that
  connects directly (many database drivers, raw sockets) is invisible here;
  that's [stream](stream.md).
- HTTPS is one series per destination and outcome, not per route, unless
  [interception](interception.md) is on.
- A CONNECT is logged when the tunnel closes: long-lived tunnels (WebSockets,
  HTTP/2 connections a client keeps open) show up late, with a long duration.
- No authentication on the proxy; it only listens on `127.0.0.1`.
