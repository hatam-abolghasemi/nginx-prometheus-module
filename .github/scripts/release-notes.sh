#!/bin/sh
# Prints the release notes for a tag: its `## <tag>` section of CHANGELOG.md
# (up to the next `## ` heading), followed by the image note. Exits non-zero if
# the tag has no section, so a release is never published with empty notes.
#
#   sh .github/scripts/release-notes.sh v1.1.0
#
# Used by .github/workflows/release.yml; run it by hand to (re)write the notes
# of an existing release:
#   sh .github/scripts/release-notes.sh v1.1.0 | gh release edit v1.1.0 --notes-file -
set -eu

tag="${1:?usage: release-notes.sh vX.Y.Z}"
changelog="${2:-CHANGELOG.md}"

section=$(awk -v tag="$tag" '
    /^## / {
        if (found) exit
        # "## v1.1.0 — 2026-11-05" or "## v1.1.0"
        split($0, f, " ")
        if (f[2] == tag) { found = 1; next }
    }
    found { print }
' "$changelog")

if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "release-notes: no '## $tag' section in $changelog" >&2
    exit 1
fi

printf '%s\n' "$section" | sed -e '/./,$!d'
cat <<'NOTE'

---

**The module:** the `.so` files attached below are built from this tag for
the newest mainline and stable nginx (glibc and musl, amd64 and arm64). Load
one with `load_module`; it must match your nginx version exactly. Any other
version: build it from source, see
[docs/module.md](https://github.com/hatam-abolghasemi/nginx-prometheus-module/blob/main/docs/module.md#install).

**Images** are published continuously under the upstream images' own tags,
not these versions:

    ghcr.io/hatam-abolghasemi/nginx-prometheus-module/nginx:<nginx-tag>
    ghcr.io/hatam-abolghasemi/nginx-prometheus-module/openresty:<openresty-tag>-<flavor>

Tags: [docs/images.md](https://github.com/hatam-abolghasemi/nginx-prometheus-module/blob/main/docs/images.md).
Pin by digest (`@sha256:...`) in production.
NOTE
