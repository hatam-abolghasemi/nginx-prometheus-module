# caller identity — who called you, and who you are

Four labels that turn "a request" into "a request from `cart` in `shop` to
`checkout`":

| Label | Answers | Default |
|---|---|---|
| `source_service`, `source_namespace` | Which Service and namespace sent this request | off |
| `app`, `namespace` | Which Service and namespace this nginx is | off |
| `client_subnet` | Roughly where the client is, without one series per IP | off |

Caller names come from reverse DNS of the caller's pod IP, so they need
**Kubernetes** cluster DNS. Under plain Docker everything else works, and
callers get the [default labels](#how-a-caller-is-named).

Depends on nothing; the [sidecar](sidecar.md) makes it one variable. The
same caller labels work on [stream sessions](stream.md#who-opened-the-session),
with these settings.

---

## What you get

```
nginx_http_requests_total{method="POST",route="/orders",status="201",host="checkout",mode="0",source_service="cart",source_namespace="shop",protocol="http"} 118
nginx_http_requests_total{method="GET",route="/healthz",status="200",host="checkout",mode="0",source_service="health-probe",source_namespace="kubernetes",protocol="http"} 40
```

`sum by (source_service, source_namespace) (rate(nginx_http_requests_total[5m]))`
is who calls you and how much; add `route` and `status` to see which caller
gets the errors.

---

## Step by step

### With the sidecar image

Add `POD_CIDR` next to `APP_PORT`: your cluster's pod range, from your CNI's
config or:

```bash
sudo kubectl cluster-info dump | grep -m1 -o 'cluster-cidr=[^"]*'
```

```yaml
env:
  - { name: APP_PORT, value: "8080" }
  - { name: POD_CIDR, value: "10.233.0.0/16" }
```

That turns on `source_service` / `source_namespace` with callers in that range
resolved by reverse DNS, and (from `APP_PORT`) own identity. The DNS server is
the pod's own, from `/etc/resolv.conf`.

### On your own nginx

```nginx
http {
    prometheus_label source_service on;
    prometheus_label source_namespace on;
    prometheus_source_ptr 10.233.0.0/16;                  # the pod CIDR

    # optional: known ranges outside the cluster, named without DNS
    prometheus_source_static 192.168.104.0/24 internal zone_a;
    prometheus_source_static 192.168.140.0/22 internal zone_b;

    # optional: who this nginx is (needs to run in a pod)
    prometheus_identity on;
    prometheus_label app on;
    prometheus_label namespace on;
}
```

Every directive with its default: [module.md](README.md#directives).

### Check it

```bash
curl -s localhost:9145/metrics | grep -o 'source_service="[^"]*",source_namespace="[^"]*"' | sort | uniq -c
```

A new caller's **first** request can show `health-probe` / `kubernetes`: its
lookup starts with that request, and later requests carry the answer. With
own identity on, the error log has a line once the pod's name is found:

```
prometheus: this pod is checkout in shop
```

---

## How a caller is named

In this order, first match wins:

1. **A kubelet probe** (`User-Agent: kube-probe/…`) is
   `prometheus_source_unresolved`: `health-probe` / `kubernetes`. Probes come
   from the node's IP, which says nothing about the caller. Off with
   `prometheus_source_probe off`.
2. **A pod**, an address in a `prometheus_source_ptr` range, is named by
   reverse DNS: `<pod-ip>.<service>.<namespace>.svc.cluster.local` gives
   `service` / `namespace`. A pod behind several Services has one PTR record
   per Service, so one is picked the same way every time: Services matching
   `prometheus_source_ignore_service` (`-metrics$`, `-headless$`) are passed
   over unless nothing else is left, then the alphabetically first wins. A pod
   with no Service (`<pod-ip>.<namespace>.pod.cluster.local`) gets
   `health-probe` / its namespace. No answer (NXDOMAIN) gets
   `health-probe` / `kubernetes`.
3. **A known range**, `prometheus_source_static`, gets that range's labels;
   the longest matching range wins.
4. **Anything else** is `prometheus_source_default`: `public` / `unknown`.

Then, **the mesh waypoint**: if the caller resolved to the Service named by
`prometheus_source_waypoint` (`waypoint`, e.g. Istio ambient), the real caller
is in the waypoint's `X-Forwarded-Client-Cert`
(`URI=spiffe://…/ns/<namespace>/sa/<service-account>`) and is used instead.
The header is trusted only after reverse DNS, not the request, has said the
peer is the waypoint, so a client can't fake it.

**The outbound leg** (`prometheus_mode 1`) is called by this pod itself: its
callers are always its own `app` / `namespace`.

## How lookups stay off the request path

- Reverse DNS never delays a request. A request from an address with no
  answer starts the lookup before its content is produced and is logged with
  whatever is known by then; in-cluster DNS usually answers first.
- Answers are kept in the zone, shared by all workers, **until a new answer
  replaces them**: worker 0 re-resolves every known caller each
  `prometheus_source_refresh` (15s), so an IP handed to a different pod is
  relabelled within one interval. Keep it at or below your scrape interval.
- A timeout or SERVFAIL keeps what is stored; a lookup is retried at most once
  per `prometheus_dns_timeout` + 1s per address.
- A caller no request has come from for `prometheus_source_forget` (5 min) is
  dropped.
- The module asks DNS itself (one UDP socket per worker), because nginx's own
  resolver keeps only the first PTR record.

## Own identity

`prometheus_identity on` finds this pod's IP in `/etc/hosts` (the kubelet
writes `<pod-ip> <pod-name>` there) and resolves it the same way, retrying
every 5s, backing off to 60s, until the pod is a Ready endpoint of a Service.
Until then, `app` / `namespace` are `prometheus_identity_fallback`: `unknown`
and the namespace from the service account mount.

Once found it is used for:

- the `app` / `namespace` labels (off by default, `prometheus_label app on`);
- the `host` label: a `Host` that is this pod's own IP (kubelet probes, a
  caller using the pod IP) shows the pod's Service, the name other callers use;
- the outbound leg's caller.

## client_subnet

The first `X-Forwarded-For` address, else the peer, masked: `/24` for `10/8`,
`192/8` and `172.16/12`, `/16` for anything else. IPv6 is `unknown`. Off by
default: it grows with your client base.

## Limits

- IPv4 only: an IPv6 caller is `prometheus_source_default`.
- `cluster.local` is the default domain; set `prometheus_cluster_domain` if
  yours differs.
- The labels are what DNS says about the **peer address**. Behind a proxy or
  load balancer that isn't the mesh waypoint, that's the proxy; use the
  `realip` module (its result is what the module sees) or `client_subnet` with
  `X-Forwarded-For`.
