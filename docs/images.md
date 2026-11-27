# Images

Two images, each a rebuild of an upstream image in its main flavors, on the
same platforms. **The tags are upstream's own**: swap the image name, keep the
tag. Any other flavor is [one `docker build`](#any-other-flavor).

| Image | Rebuilt from | What's added |
|---|---|---|
| [`ghcr.io/hatam-abolghasemi/nginx`](https://github.com/users/hatam-abolghasemi/packages/container/package/nginx) | [`nginx`](https://hub.docker.com/_/nginx) | the [C module](nginx/README.md), metrics on `:9145/metrics` |
| [`ghcr.io/hatam-abolghasemi/openresty`](https://github.com/users/hatam-abolghasemi/packages/container/package/openresty) | [`openresty/openresty`](https://hub.docker.com/r/openresty/openresty) | the [Lua module](openresty/README.md): [inbound](openresty/inbound.md), [outbound](openresty/outbound.md), [https interception](openresty/https-interception.md), [stream](openresty/stream.md) |

The package pages list every tag and digest.

## nginx

The newest mainline and the newest stable nginx, in the default (debian) and
`alpine` flavors, on every platform upstream publishes them for:

| Tag | Meaning |
|---|---|
| `<version>`, `<version>-alpine` | that nginx version |
| `<series>[-alpine]`, e.g. `1.31-alpine` | newest version in that series |
| `mainline[-alpine]`, `alpine`, `latest` | newest mainline |
| `stable[-alpine]` | newest stable |

```bash
docker pull ghcr.io/hatam-abolghasemi/nginx:alpine
docker pull ghcr.io/hatam-abolghasemi/nginx:stable-alpine
```

## openresty

Every [docker-openresty](https://github.com/openresty/docker-openresty)
release, in the `alpine` and `bookworm` flavors:

| Tag | Meaning |
|---|---|
| `<release>-<flavor>` | that docker-openresty release |
| `<version>-<flavor>` | newest revision of that OpenResty version |
| `<series>-<flavor>` | newest release in that series |
| `alpine`, `bookworm` | newest release |
| `latest` | newest release, `alpine` (upstream's own `latest` is bookworm) |

```bash
docker pull ghcr.io/hatam-abolghasemi/openresty:latest
```

## Any other flavor

Both Dockerfiles take any upstream tag, flavor included (`alpine-slim`,
`perl`, `otel`, `trixie` for nginx; `noble`, `rocky`, ... for OpenResty), and
work for any version. From a clone of this repo:

```bash
docker build -f nginx/docker/Dockerfile --build-arg NGINX_TAG=1.31.10-perl -t my/nginx:1.31.10-perl nginx
```
```bash
docker build -f openresty/docker/Dockerfile --build-arg OPENRESTY_TAG=1.31.1.1-4-noble -t my/openresty:1.31.1.1-4-noble openresty
```

Add `--platform linux/arm64` (with buildx) for another architecture. The nginx
build compiles the module against the nginx inside that exact image, so the
result is the same as a published tag. Only the `.so` files, for your own
nginx image: add `--target module --output type=local,dest=out`.

## Older names

| Until | Name |
|---|---|
| v2.3.0 | `ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx`, `…/nginx-prometheus-module/openresty` |
| v1.x | `ghcr.io/hatam-abolghasemi/openresty-prometheus-exporter` |

They stay pullable, but get no new builds: move to the names above.

## Pinning

A tag is rebuilt when this repo's code changes (and nginx/OpenResty tags move
upstream), so pin the digest for an immutable reference:

```bash
docker buildx imagetools inspect ghcr.io/hatam-abolghasemi/nginx:1.31.10-alpine
```

The `org.opencontainers.image.revision` label names the commit an image was
built from.
