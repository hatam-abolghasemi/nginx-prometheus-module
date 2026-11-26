# Contributing

Thanks for taking the time. Bug reports, route shapes the sanitizer misses,
dashboard fixes, docs and code are all welcome.

## Before you start

- **Questions and ideas** go to [Discussions](https://github.com/hatam-abolghasemi/nginx-prometheus-module/discussions).
- **Bugs and concrete feature requests** go to [Issues](https://github.com/hatam-abolghasemi/nginx-prometheus-module/issues/new/choose); the templates ask for what's needed to reproduce.
- **Security problems** never go in a public issue; see [SECURITY.md](SECURITY.md).
- For anything bigger than a small fix, open an issue or discussion first so we
  agree on the approach before you write the code.

## Where things live

| Path | What |
|---|---|
| `lib/` | The exporter: OpenResty Lua modules. `metrics_config.lua` is data only; the rest is code |
| `vendor/nginx-lua-prometheus/` | Prometheus client library, vendored **unmodified**; don't edit it here |
| `docker/` | Image build: nginx config, entrypoint setup |
| `grafana/` | The dashboard (Grafana v2 schema) |
| `examples/` | Kubernetes sidecar example and the Docker quickstart |
| `tests/` | Unit tests for the pure `lib/` modules |
| `docs/` | Feature guides, metrics reference, maintainer's guide |

[docs/modules.md](docs/modules.md) explains every module, why it's shaped that
way, and step-by-step recipes for
[adding a metric](docs/modules.md#adding-a-metric),
[a label](docs/modules.md#adding-a-label) and
[a sanitizer check](docs/modules.md#adding-a-sanitizer-check).
[ARCHITECTURE.md](ARCHITECTURE.md) has the design narrative.

## Running the tests

### The module

The label code (route sanitizer, Host classifier, escaping) builds and runs on
its own:

```bash
cc -Wall -Wextra -Werror -fsanitize=address,undefined -Imodule \
   -o label_test tests/label_test.c module/ngx_http_prometheus_label.c && ./label_test
```

Then the module in a real nginx, from the nginx source of any version
(`--with-cc-opt='-fsanitize=address,undefined'` for a checked build):

```bash
./auto/configure --prefix="$PWD/out" --with-compat --with-http_stub_status_module \
    --with-http_ssl_module --add-dynamic-module=/path/to/nginx-prometheus-module/module
make -j && make install
sh /path/to/nginx-prometheus-module/tests/module_test.sh out/sbin/nginx \
    out/modules/ngx_http_prometheus_module.so
```

It uses ports 18080, 18081 and 19145, and checks the output with `promtool`
when it's on `PATH`. nginx's own style applies to `module/`: 4 spaces, 80
columns, declarations at the top of a block, `ngx_` names.

### The OpenResty sidecar

The unit tests need only LuaJIT (or Lua 5.1), no nginx:

```bash
luajit tests/lib_test.lua
```

To see your change end to end, build the image and run the quickstart against it:

```bash
docker build -f docker/Dockerfile --build-arg OPENRESTY_TAG=alpine -t ope:dev .
cd examples/quickstart
sed -i 's#ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty:latest#ope:dev#' docker-compose.yml
docker compose up -d
curl -s localhost:9145/metrics | grep -v '^#' | head -40
```

`nginx_metric_errors_total` must stay at 0. Anything else means a label schema
mismatch.

## What a good change looks like

- **The hot path stays cheap.** Everything in `log_by_lua` runs on every request.
  No per-request allocations you can avoid, no blocking I/O.
- **Cardinality stays bounded.** A new label must have a small, fixed set of
  values, or be bucketed at the source like `route` and `host`. If a value can
  carry an ID, it doesn't become a label as-is.
- **Defaults don't change silently.** New metrics and labels that cost series
  start off (`false` in `metrics_config.lua`) unless they're clearly worth it
  for everyone. If a default must change, say so in the changelog.
- **Pure logic gets a test.** Parsers, classifiers and the sanitizer are written
  to be testable without nginx; add cases to `tests/lib_test.lua`.
- **Docs move with the code.** Update the feature guide, `docs/metrics-reference.md`
  for new metrics or labels, and the dashboard if it should show them.
- **One dated entry in [CHANGELOG.md](CHANGELOG.md)**, under **Unreleased**,
  describing what changed and why, in the style of the existing entries. A
  release moves those entries under its `## vX.Y.Z` heading, which becomes the
  GitHub Release notes.

## Pull requests

1. Fork, branch from `main`.
2. Keep the PR to one concern; split unrelated changes.
3. Make sure `luajit tests/lib_test.lua` passes; CI runs it too, plus the
   vendored library's own suite.
4. Fill in the PR template.

By contributing you agree that your work is released under the project's
[MIT license](LICENSE), and to follow the [code of conduct](CODE_OF_CONDUCT.md).
