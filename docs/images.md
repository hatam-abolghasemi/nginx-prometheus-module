# Images

Two images, each a rebuild of an upstream image in every flavor it publishes,
on the same platforms. **The tags are upstream's own**: swap the image name,
keep the tag.

| Image | Rebuilt from | What's added |
|---|---|---|
| [`ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx`](https://github.com/hatam-abolghasemi/nginx-prometheus-module/pkgs/container/nginx-prometheus-module%2Fnginx) | [`nginx`](https://hub.docker.com/_/nginx) | the [module](nginx/README.md), metrics on `:9145/metrics` |
| [`ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty`](https://github.com/hatam-abolghasemi/nginx-prometheus-module/pkgs/container/nginx-prometheus-module%2Fopenresty) | [`openresty/openresty`](https://hub.docker.com/r/openresty/openresty) | the OpenResty sidecar: [inbound](openresty/inbound.md), [outbound](openresty/outbound.md), [https interception](openresty/https-interception.md), [stream](openresty/stream.md) |

The package pages list every tag and digest.

## nginx

The newest mainline and the newest stable nginx, every flavor (`alpine`,
`alpine-slim`, `perl`, `otel`, `bookworm`, ...):

| Tag | Meaning |
|---|---|
| `<version>`, `<version>-<flavor>` | that nginx version, that flavor |
| `<series>[-<flavor>]`, e.g. `1.31-alpine` | newest version in that series |
| `mainline[-<flavor>]`, `<flavor>`, `latest` | newest mainline |
| `stable[-<flavor>]` | newest stable |

```bash
docker pull ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:alpine
docker pull ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:stable-alpine
```

## openresty

Every [docker-openresty](https://github.com/openresty/docker-openresty)
release, every flavor:

| Tag | Meaning |
|---|---|
| `<release>-<flavor>` | that docker-openresty release, that flavor |
| `<version>-<flavor>` | newest revision of that OpenResty version |
| `<series>-<flavor>` | newest release in that series |
| `<flavor>`, e.g. `alpine` | newest release |
| `latest` | newest release, `alpine` flavor (upstream's own `latest` is bookworm) |

```bash
docker pull ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty:latest
```

Images published before v2.0.0 stay pullable under their old name,
`ghcr.io/hatam-abolghasemi/openresty-prometheus-exporter:<tag>`; new builds
go to the name above.

## Pinning

A tag is rebuilt when this repo's code changes (and nginx/OpenResty tags move
upstream), so pin the digest for an immutable reference:

```bash
docker buildx imagetools inspect ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:1.31.10-alpine
```

The `org.opencontainers.image.revision` label names the commit an image was
built from.
