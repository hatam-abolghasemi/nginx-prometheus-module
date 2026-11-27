#!/bin/sh
# Prints the release notes for a tag: its `## <tag>` section of CHANGELOG.md
# (up to the next `## ` heading), then the module and image notes. Exits
# non-zero if the tag has no section, so a release is never published with
# empty notes.
#
#   sh .github/scripts/release-notes.sh v2.2.0 [CHANGELOG.md] [repo URL]
#
# The changelog is wrapped at 80 columns for reading in an editor; GitHub
# shows every newline in a release as a line break, so each paragraph and
# list item is joined back into one line. The "### <date>" headings that
# group entries in the file are left out, and links into the repo point at
# the tag's files.
#
# Used by .github/workflows/release.yml; by hand:
#   sh .github/scripts/release-notes.sh v2.2.0 | gh release edit v2.2.0 --notes-file -
set -eu

tag="${1:?usage: release-notes.sh vX.Y.Z [CHANGELOG.md] [repo URL]}"
changelog="${2:-CHANGELOG.md}"
repo="${3:-https://github.com/hatam-abolghasemi/nginx-prometheus-module}"

section=$(awk -v tag="$tag" '
    /^## / {
        if (found) exit
        # "## v2.2.0 — 2026-11-27" or "## v2.2.0"
        split($0, f, " ")
        if (f[2] == tag) { found = 1; next }
    }
    found { print }
' "$changelog")

if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "release-notes: no '## $tag' section in $changelog" >&2
    exit 1
fi

printf '%s\n' "$section" | awk -v repo="$repo" -v tag="$tag" '
    function flush() { if (cur != "") { print cur; cur = "" } }

    # code blocks pass through as they are
    /^[[:space:]]*```/ { flush(); print; code = !code; next }
    code { print; next }

    # the date headings only group entries in the file
    /^### [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9][[:space:]]*$/ { next }

    /^[[:space:]]*$/ { flush(); if (!blank) print ""; blank = 1; next }

    {
        blank = 0
        line = $0
        # relative links into the repo point at this tag
        gsub(/\]\(docs\//, "](" repo "/blob/" tag "/docs/", line)
        gsub(/\]\(examples\//, "](" repo "/tree/" tag "/examples/", line)
        gsub(/\]\(grafana\//, "](" repo "/tree/" tag "/grafana/", line)
    }

    # a heading, a list item or a table row starts a line of its own
    line ~ /^#/ || line ~ /^[[:space:]]*([-*]|[0-9]+\.) / || line ~ /^[[:space:]]*\|/ {
        flush(); cur = line; sub(/[[:space:]]+$/, "", cur)
        if (line ~ /^#/ || line ~ /^[[:space:]]*\|/) flush()
        next
    }

    # anything else continues the line before it
    {
        sub(/^[[:space:]]+/, "", line); sub(/[[:space:]]+$/, "", line)
        cur = (cur == "") ? line : cur " " line
    }

    END { flush() }
' | awk 'NF || seen { print; seen = 1 }' | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}'

# before v2 there was no module, and the image had its old name
case $tag in
    v0.*|v1.*)
        cat <<NOTE

## Image

\`\`\`
ghcr.io/hatam-abolghasemi/openresty-prometheus-exporter:<openresty-tag>-<flavor>
\`\`\`

Its tags are docker-openresty's own; pin a digest in production.
NOTE
        exit 0 ;;
esac

# the images' names until v2.3.1
images_prefix=ghcr.io/hatam-abolghasemi
case $tag in v2.0.*|v2.1.*|v2.2.*|v2.3.0) images_prefix=ghcr.io/hatam-abolghasemi/nginx-prometheus-module ;; esac

# where the build-from-source guide was before the nginx/ and openresty/ split
guide=docs/nginx/README.md
stream_note=" For stream (L4), load \`ngx_stream_prometheus_module.so\` too; its metrics are on the same \`/metrics\`."
case $tag in v2.0.*|v2.1.*|v2.2.*) guide=docs/module.md; stream_note="" ;; esac

# the module package, from v2.7.0: the first way in
package_note=""
package_line=""
case $tag in
    v2.[0-6].*) ;;
    *)
        package_note="The module package has both files for every nginx tag; in your own image: \`COPY --from=$images_prefix/nginx-prometheus-module:<nginx-tag> / /usr/lib/nginx/modules/\`. "
        package_line="$images_prefix/nginx-prometheus-module:<nginx-tag>
" ;;
esac

cat <<NOTE

## Module

${package_note}The \`.so\` files attached to this release are built from this tag for the newest mainline and stable nginx, glibc and musl, amd64 and arm64. A module loads only into the nginx version it was built for: check \`nginx -V\`, copy the file into \`modules/\`, and add \`load_module modules/ngx_http_prometheus_module.so;\` at the top of \`nginx.conf\`. That line is the whole setup: metrics are on \`:9145/metrics\`.$stream_note Any other nginx version: [build it from source]($repo/blob/$tag/$guide#from-source).

## Images

\`\`\`
${package_line}$images_prefix/nginx:<nginx-tag>
$images_prefix/openresty:<openresty-tag>-<flavor>
\`\`\`

The tags are the upstream images' own and are rebuilt as this project changes; pin a digest in production ([images]($repo/blob/$tag/docs/images.md)).
NOTE
