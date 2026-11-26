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

One module, two implementations kept in step: the same metric names, labels
and route rules, so queries work with either.

| Path | What |
|---|---|
| `nginx/` | **The C module** for stock nginx, `ngx_http_prometheus_module`: `config` (nginx's build file), `src/`, `docker/` (its image), `tests/` |
| `openresty/` | **The Lua module** for OpenResty: `lib/` (the code; `metrics_config.lua` is data only), `vendor/nginx-lua-prometheus/` (vendored **unmodified**, don't edit it here), `docker/` (its image), `tests/` |
| `docs/` | `nginx/` and `openresty/` guides, and `images.md` for both images |
| `examples/` | `nginx-sidecar/`, `openresty-sidecar/` (Kubernetes) and the Docker `quickstart/` |
| `grafana/` | The dashboard (Grafana v2 schema), built on the Lua module's labels |
| `.github/workflows/` | `nginx-*.yml` and `openresty-*.yml`; `release.yml` for both |

A change to what is recorded (a metric, a label, a route rule) is made in
both, or says in the changelog which one has it. The guides for the C module
are [docs/nginx/](docs/nginx/README.md); for the Lua module,
[docs/openresty/modules.md](docs/openresty/modules.md) explains every module
and has recipes for [adding a metric](docs/openresty/modules.md#adding-a-metric),
[a label](docs/openresty/modules.md#adding-a-label) and
[a sanitizer check](docs/openresty/modules.md#adding-a-sanitizer-check), and
[architecture.md](docs/openresty/architecture.md) the design narrative.

## Running the tests

### The C module

From `nginx/`. The label and identity code builds and runs on its own:

```bash
cd nginx
cc -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
   -o label_test tests/label_test.c src/ngx_http_prometheus_label.c && ./label_test
cc -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
   -o identity_test tests/identity_test.c src/ngx_http_prometheus_identity.c && ./identity_test
```

Then the module in a real nginx, from the nginx source of any version
(`--with-cc-opt='-fsanitize=address,undefined'` for a checked build):

```bash
./auto/configure --prefix="$PWD/out" --with-compat --with-http_stub_status_module \
    --with-http_ssl_module --add-dynamic-module=/path/to/nginx-prometheus-module/nginx
make -j && make install
for t in module source plug outbound; do
    sh "/path/to/nginx-prometheus-module/nginx/tests/${t}_test.sh" out/sbin/nginx \
        out/modules/ngx_http_prometheus_module.so
done
```

They use ports 18060–18090, 19145–19150 and 9145, and check the output with
`promtool` when it's on `PATH`. nginx's own style applies to `nginx/src/`:
4 spaces, 80 columns, declarations at the top of a block, `ngx_` names.

The image, from the repo root:

```bash
docker build -f nginx/docker/Dockerfile --build-arg NGINX_TAG=alpine -t npm:dev nginx
```

### The Lua module

From `openresty/`. The unit tests need only LuaJIT (or Lua 5.1), no nginx:

```bash
cd openresty
luajit tests/lib_test.lua
```

To see your change end to end, build the image and run the quickstart against it:

```bash
docker build -f openresty/docker/Dockerfile --build-arg OPENRESTY_TAG=alpine -t ope:dev openresty
cd examples/quickstart
sed -i 's#ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty:latest#ope:dev#' docker-compose.yml
docker compose up -d
curl -s localhost:9145/metrics | grep -v '^#' | head -40
```

`nginx_metric_errors_total` must stay at 0 in both. Anything else means a
label schema mismatch (Lua) or a full zone (C).

## What a good change looks like

- **The hot path stays cheap.** The C module's log handler and the Lua
  module's `log_by_lua` run on every request. No per-request allocations you
  can avoid, no blocking I/O.
- **Cardinality stays bounded.** A new label must have a small, fixed set of
  values, or be bucketed at the source like `route` and `host`. If a value can
  carry an ID, it doesn't become a label as-is.
- **Defaults don't change silently.** New metrics and labels that cost series
  start off (`false` in `metrics_config.lua`, an `off` default in the C module) unless they're clearly worth it
  for everyone. If a default must change, say so in the changelog.
- **Pure logic gets a test.** Parsers, classifiers and the sanitizer are written
  to be testable without nginx; add cases to `nginx/tests/*_test.c` or
  `openresty/tests/lib_test.lua`.
- **Docs move with the code.** Update the feature guide (`docs/nginx/` or
  `docs/openresty/`), the metrics lists for new metrics or labels, and the
  dashboard if it should show them.
- **One dated entry in [CHANGELOG.md](CHANGELOG.md)**, under **Unreleased**,
  describing what changed and why, in the style of the existing entries. A
  release moves those entries under its `## vX.Y.Z` heading, which becomes the
  GitHub Release notes.

## Pull requests

1. Fork, branch from `main`.
2. Keep the PR to one concern; split unrelated changes.
3. Make sure the tests of what you touched pass (above); CI runs them too,
   plus the vendored library's own suite.
4. Fill in the PR template.

By contributing you agree that your work is released under the project's
[MIT license](LICENSE), and to follow the [code of conduct](CODE_OF_CONDUCT.md).
