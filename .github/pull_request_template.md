## What and why

<!-- What does this change, and what problem does it solve? Link the issue: Fixes #123 -->

## Area

- [ ] the C module (`nginx/`)
- [ ] the Lua module (`openresty/`)
- [ ] inbound / outbound
- [ ] https interception
- [ ] stream (L4)
- [ ] route sanitizer / host classifier / caller identity
- [ ] image / docker
- [ ] Grafana dashboard
- [ ] docs only

## Checklist

- [ ] The tests of what changed pass: `nginx/tests/`, `openresty/tests/lib_test.lua` (new pure logic has test cases)
- [ ] No new per-request cost on the hot path, or it's justified above
- [ ] New labels have a bounded set of values; new costly metrics/labels default to off
- [ ] `nginx_metric_errors_total` stays 0 with the change running
- [ ] Docs updated (`docs/nginx/` or `docs/openresty/`, metrics lists for metrics/labels)
- [ ] A change to what is recorded is in both modules, or the changelog says which has it
- [ ] `CHANGELOG.md` entry added
