#!/bin/sh
# Lists, and with --delete removes, the deprecated images on ghcr.io:
#
#   - the old names: nginx-prometheus-module/nginx, nginx-prometheus-module/openresty
#     (until v2.3.0) and openresty-prometheus-exporter (v1.x), whole packages;
#   - old flavor tags of the current images, no longer built since v2.3.1:
#     nginx tags other than default and alpine (perl, otel, alpine-slim, ...),
#     openresty tags other than alpine and bookworm (noble, rocky, ...);
#   - the nginx stable line, no longer built since v2.8.0: the versions tagged
#     stable / stable-alpine, in the image and in the module package.
#
# A package version is deleted only if it has tags and every one of them is
# deprecated. Untagged versions are never touched: they are the per-platform
# manifests of the multi-arch tags that stay.
#
#   gh auth refresh -s read:packages,delete:packages
#   sh .github/scripts/prune-images.sh             # dry run: prints what would go
#   sh .github/scripts/prune-images.sh --delete    # deletes it
#
# OWNER (default hatam-abolghasemi) and OWNER_TYPE (users or orgs) select the
# account. Needs gh and jq.
set -eu

OWNER=${OWNER:-hatam-abolghasemi}
OWNER_TYPE=${OWNER_TYPE:-users}
DELETE=false
[ "${1:-}" = --delete ] && DELETE=true

OLD_PACKAGES="nginx-prometheus-module/nginx nginx-prometheus-module/openresty openresty-prometheus-exporter"

# tags that stay, per current image (extended regular expressions)
KEEP_nginx='^(latest|alpine|mainline|mainline-alpine|[0-9]+\.[0-9]+(\.[0-9]+)?|[0-9]+\.[0-9]+(\.[0-9]+)?-alpine)$'
KEEP_openresty='^(latest|alpine|bookworm|.*-alpine|.*-bookworm)$'

enc() { printf '%s' "$1" | sed 's#/#%2F#g'; }

api() { gh api "$@"; }

if $DELETE; then echo "deleting"; else echo "dry run (--delete to delete)"; fi

for pkg in $OLD_PACKAGES; do
    if api "/$OWNER_TYPE/$OWNER/packages/container/$(enc "$pkg")" > /dev/null 2>&1; then
        echo "package  ghcr.io/$OWNER/$pkg  (deprecated name)"
        if $DELETE; then
            api -X DELETE "/$OWNER_TYPE/$OWNER/packages/container/$(enc "$pkg")"
            echo "         deleted"
        fi
    fi
done

for image in nginx nginx-prometheus-module openresty; do
    case $image in nginx-prometheus-module) keep=$KEEP_nginx ;; *) eval "keep=\$KEEP_$image" ;; esac
    api --paginate "/$OWNER_TYPE/$OWNER/packages/container/$image/versions?per_page=100" \
        --jq '.[] | select((.metadata.container.tags | length) > 0)
                  | [(.id | tostring), (.metadata.container.tags | join(","))] | @tsv' \
    | while IFS="$(printf '\t')" read -r id tags; do
        stays=false
        why="old flavor"
        if echo ",$tags," | grep -Eq ',stable(-alpine)?,'; then
            why="stable line"
        else
            for t in $(echo "$tags" | tr ',' ' '); do
                if echo "$t" | grep -Eq "$keep"; then stays=true; break; fi
            done
        fi
        $stays && continue

        echo "version  ghcr.io/$OWNER/$image  $tags  ($why)"
        if $DELETE; then
            api -X DELETE "/$OWNER_TYPE/$OWNER/packages/container/$image/versions/$id"
            echo "         deleted"
        fi
    done
done
