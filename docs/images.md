# Images

The module on its own, then two images, each a rebuild of an upstream image
in its main flavors, on the same platforms. **The tags are upstream's own**:
swap the image name, keep the tag. Any other flavor is
[one `docker build`](#any-other-flavor).

| Package | Built from | What's in it |
|---|---|---|
| [`ghcr.io/hatam-abolghasemi/nginx-prometheus-module`](https://github.com/users/hatam-abolghasemi/packages/container/package/nginx-prometheus-module) | [`nginx`](https://hub.docker.com/_/nginx) | the [C module](nginx/README.md)'s two `.so` files, nothing else: [the module](#the-module) |
| [`ghcr.io/hatam-abolghasemi/nginx`](https://github.com/users/hatam-abolghasemi/packages/container/package/nginx) | [`nginx`](https://hub.docker.com/_/nginx) | the [C module](nginx/README.md), metrics on `:9145/metrics` |
| [`ghcr.io/hatam-abolghasemi/openresty`](https://github.com/users/hatam-abolghasemi/packages/container/package/openresty) | [`openresty/openresty`](https://hub.docker.com/r/openresty/openresty) | the [Lua module](openresty/README.md): [inbound](openresty/inbound.md), [outbound](openresty/outbound.md), [https interception](openresty/https-interception.md), [stream](openresty/stream.md) |

The package pages list every tag and digest, newest Alpine first.

**One version of each, always the newest.** nginx: the newest nginx release,
Debian (the plain tag) and Alpine. OpenResty: the newest docker-openresty
release, Alpine and Debian (bookworm); it can be a little behind nginx, as
each follows its own upstream. Older exact versions stay pullable for pinning
but aren't rebuilt.

**They follow upstream.** Every 6 hours
[`watch-upstream.yml`](../.github/workflows/watch-upstream.yml) looks at
upstream: a new nginx, a new docker-openresty release, or a tag upstream
has rebuilt on a newer Debian or Alpine base (a new digest under the same
tag) is built, smoke-tested and pushed. Each image records the upstream
digest it was built from (`org.opencontainers.image.base.digest`), so a
security rebuild upstream reaches these images within hours.

## The module

`ghcr.io/hatam-abolghasemi/nginx-prometheus-module`: `ngx_http_prometheus_module.so`
and `ngx_stream_prometheus_module.so` at `/`, built against that exact nginx,
for your own nginx image or host. Same tags as [nginx](#nginx) below:
`<version>` is for a Debian (glibc) nginx, `<version>-alpine` for Alpine
(musl).

```dockerfile
FROM nginx:1.31.10-alpine
COPY --from=ghcr.io/hatam-abolghasemi/nginx-prometheus-module:1.31.10-alpine / /usr/lib/nginx/modules/
RUN sed -i '1i load_module modules/ngx_http_prometheus_module.so;' /etc/nginx/nginx.conf
```

More, and on a host: [install](nginx/README.md#install).

## nginx

The newest nginx, in the default (Debian) and `alpine` flavors, on every
platform upstream publishes them for:

| Tag | Meaning |
|---|---|
| `<version>`, `<version>-alpine` | that nginx version |
| `<series>[-alpine]`, e.g. `1.31-alpine` | newest version in that series |
| `latest`, `alpine`, `mainline-alpine` | the newest, Alpine |
| `mainline` | the newest, Debian |

One difference from upstream: `latest` is Alpine here (upstream's is Debian),
the same as in the OpenResty image. For Debian, use `mainline` or a version.

```bash
docker pull ghcr.io/hatam-abolghasemi/nginx:alpine
docker pull ghcr.io/hatam-abolghasemi/nginx:1.31.10       # Debian
```

## openresty

The newest [docker-openresty](https://github.com/openresty/docker-openresty)
release, in the `alpine` and `bookworm` (Debian) flavors:

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

## Deprecated

These get no new builds and will be deleted from ghcr.io. Move to the names
and tags above, or [build the flavor yourself](#any-other-flavor).

`…/nginx-prometheus-module/nginx` and `…/openresty` are not
`ghcr.io/hatam-abolghasemi/nginx-prometheus-module` itself, with nothing
after it: that one is [the module](#the-module), current.

| Deprecated | Last built | Instead |
|---|---|---|
| `ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx` | v2.3.0 | `ghcr.io/hatam-abolghasemi/nginx`, same tags |
| `ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty` | v2.3.0 | `ghcr.io/hatam-abolghasemi/openresty`, same tags |
| `ghcr.io/hatam-abolghasemi/openresty-prometheus-exporter` | v1.x | `ghcr.io/hatam-abolghasemi/openresty` |
| nginx flavor tags other than default and `alpine` (`perl`, `otel`, `alpine-slim`, `trixie`, ...) | v2.3.0 | the default or `alpine` tag, or one `docker build` |
| openresty flavor tags other than `alpine` and `bookworm` (`noble`, `rocky`, ...) | v2.3.0 | `alpine` / `bookworm`, or one `docker build` |
| nginx `stable`, `stable-alpine` (and the module package's) | v2.7.0 | `alpine` / `latest` (the newest nginx), or a pinned version |

What will go, and deleting it (the maintainer, with `delete:packages`):
`sh .github/scripts/prune-images.sh` lists it, `--delete` deletes it.
Exact version tags of the main flavors stay.

## Pinning

A tag is rebuilt when this repo's code changes (and nginx/OpenResty tags move
upstream), so pin the digest for an immutable reference:

```bash
docker buildx imagetools inspect ghcr.io/hatam-abolghasemi/nginx:1.31.10-alpine
```

The `org.opencontainers.image.revision` label names the commit an image was
built from.
