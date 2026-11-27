# https interception — see inside outbound HTTPS and gRPC

With [outbound](outbound.md), your app's HTTPS calls are recorded as
`CONNECT api.example.com:443 → 200`: the host and port, nothing inside.
Interception decodes them, so outbound reads like inbound:

| Inside the tunnel | Recorded as |
|---|---|
| HTTPS, on any port | method, route, status, `protocol="https"` |
| gRPC over TLS | `POST /package.Service/Method`, `grpc_<code>`, `protocol="grpcs"` |
| plaintext gRPC (h2c) | `POST /package.Service/Method`, `grpc_<code>`, `protocol="grpc"` |
| anything else | `CONNECT host:port`, `protocol="tunnel"`, as before |

This is TLS interception: a man in the middle of your app's own egress, inside
your own trust domain. It is off unless you turn it on. Read
[the security model](#security-model) first.

Needs nginx 1.31+ and [outbound](outbound.md).

## How it works

```
app ──CONNECT api:443──▶ :3128 ── first bytes? ──┬─ TLS ──▶ :3129  a certificate for "api",
                                                  │                 minted and signed by YOUR CA;
                                                  │                 decrypted, recorded, sent on
                                                  │                 to api:443 over verified TLS
                                                  ├─ HTTP/2 ▶ :3130 decoded, recorded, sent on
                                                  └─ other ──▶ api:443, relayed as is
```

1. `:3128` answers `200 Connection Established` and reads the first bytes the
   app sends into the tunnel. TLS, HTTP/1 and HTTP/2 clients speak first, so
   this adds no wait.
2. A TLS ClientHello goes to `:3129`, the HTTP/2 preface to `:3130`, each behind
   a PROXY protocol line that carries the CONNECT's port. Anything else goes to
   the real target.
3. `:3129` finishes the handshake with a certificate for the name the app asked
   for (its SNI, or the address when it connected to an IP), signed by your CA.
   Certificates are made once per name and kept, per worker, for 6 days.
4. Each decrypted request is recorded like any other, as `mode="1"`, and sent
   to its destination over TLS **verified against the system trust store**:
   the app's guarantee that it talks to the real `api` still holds.
5. Connections to destinations are kept alive, **per name and port**, not per
   address: a connection verified for one name is never used for another name
   on the same IP. Once warm, an intercepted request costs no extra round trip.
6. The outer CONNECT of a decoded tunnel is not recorded again.

For gRPC, `route` is the method path as is, and `status` is
`grpc_<code>` from `grpc-status` (`grpc_0` OK, `grpc_14` UNAVAILABLE); a
non-200 HTTP status is kept as is.

## With the sidecar image

1. Make a CA, once, and keep the key secret:

   ```bash
   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 3650 \
     -subj "/CN=egress-intercept" -keyout ca.key -out ca.crt
   sudo kubectl -n shop create secret generic egress-ca --from-file=ca.crt --from-file=ca.key
   ```

2. Mount it into the sidecar at `/etc/nginx/intercept/` and set
   `INTERCEPT_TLS=on`:

   ```yaml
   - name: nginx
     image: ghcr.io/hatam-abolghasemi/nginx:mainline-alpine
     env:
       - { name: APP_PORT, value: "8080" }
       - { name: INTERCEPT_TLS, value: "on" }
     volumeMounts:
       - { name: egress-ca, mountPath: /etc/nginx/intercept, readOnly: true }
   ```

3. Make the app trust `ca.crt`. How depends on its runtime: add it to the
   system store of the app's image (`update-ca-certificates`), or point the
   client at it (`SSL_CERT_FILE`, `NODE_EXTRA_CA_CERTS`, `REQUESTS_CA_BUNDLE`,
   a Java truststore).

The start log says `outbound on 127.0.0.1:3128 with https interception`.

| Env | Default | |
|---|---|---|
| `INTERCEPT_TLS` | `off` | `on` turns it on |
| `INTERCEPT_CA_CERT`, `INTERCEPT_CA_KEY` | `/etc/nginx/intercept/ca.crt`, `ca.key` | The CA |
| `INTERCEPT_TRUSTED_CA` | the system bundle | The CAs destinations are verified against |

## On your own nginx

The sidecar's config is plain nginx; see what the image generates in
`/etc/nginx/conf.d/sidecar.conf`. The pieces:

| Directive | Context | |
|---|---|---|
| `prometheus_intercept_connect;` | location, if in location | Handles CONNECT here: first bytes to `:3129` / `:3130`, or relayed. Use it instead of `tunnel_pass` |
| `prometheus_intercept_ca <crt> <key>;` | http | The CA, read when the configuration is loaded |
| `$prometheus_intercept_cert`, `$prometheus_intercept_key` | variables | The minted certificate and its key: `ssl_certificate data:$prometheus_intercept_cert;` |
| `prometheus_intercept_resolve on;` | server, location | Resolve the request's destination (`Host`, port from the PROXY line) with `resolver` |
| `prometheus_intercept_origin <n>;` | upstream | Connect to that destination, keeping up to `n` idle connections per worker, per name. Needs a placeholder `server 0.0.0.1;` |
| `prometheus_intercept_timeout <time>;` | http, server, location | Idle time of a relayed tunnel; default `300s` |
| `prometheus_intercept_connect_timeout <time>;` | http, server, location | Connecting a relayed tunnel; default `10s` |

## Security model

- The CA key can sign a certificate for any name your app trusts it for. Treat
  it like any other secret, one CA per trust domain, and trust it only in the
  apps whose egress goes through this sidecar.
- Destinations are verified, always: interception never turns a verified
  connection into an unverified one. A destination whose certificate doesn't
  match is a `502`, recorded.
- Only what goes through `:3128` is touched, and `:3129` / `:3130` listen on
  loopback only.
- Clients that pin certificates (some mobile SDKs, some cloud clients) will
  refuse the minted certificate; keep those off the proxy (`NO_PROXY`).
