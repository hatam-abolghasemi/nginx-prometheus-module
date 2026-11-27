# Documentation

nginx-prometheus-module is one module with two implementations. They record
the same metrics with the same names, labels and route rules, so queries work
with either, and so does the [dashboard](../grafana/).

| | [The C module](nginx/README.md) | [The Lua module](openresty/README.md) |
|---|---|---|
| Runs on | stock nginx (any version, `--with-compat`) | OpenResty, or nginx with `lua-nginx-module` |
| Setup | `load_module`; metrics on `:9145` | the image, or the Lua files in your OpenResty |
| Image | [`…/nginx`](images.md#nginx), nginx mainline and stable, debian and alpine | [`…/openresty`](images.md#openresty), every docker-openresty release, alpine and bookworm |
| Inbound, sidecar | ✓ | ✓ |
| Caller and own identity | ✓ | ✓ |
| Outbound | ✓ (HTTPS on nginx 1.31+) | ✓ |
| Stream (L4) | ✓ (caller identity not yet) | ✓ |
| https interception | coming in v2.4 | ✓ |
| Status | where development is focused | maintained in parallel |

## The C module

- [Overview, install, configure, directives, metrics](nginx/README.md)
- [sidecar](nginx/sidecar.md) · [caller identity](nginx/identity.md) · [outbound](nginx/outbound.md) · [stream](nginx/stream.md)

## The Lua module

- [Index and feature tree](openresty/README.md)
- [inbound](openresty/inbound.md) · [outbound](openresty/outbound.md) · [https interception](openresty/https-interception.md) · [stream](openresty/stream.md) · [your own OpenResty](openresty/standalone-nginx.md)
- [Metrics reference](openresty/metrics-reference.md) · [Lua modules](openresty/modules.md) · [architecture](openresty/architecture.md)
- [FAQ](openresty/faq.md) · [design decisions](openresty/design-decisions.md)

## Both

- [Images](images.md): both images, their tags, pinning
- [Grafana dashboard](../grafana/)
- [Changelog](../CHANGELOG.md) · [Contributing](../CONTRIBUTING.md)
