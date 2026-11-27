#!/bin/sh
# Lists, and with --delete removes, everything on ghcr.io but the current
# version of each image (one version of each, the newest: docs/images.md):
#
#   - the old names: nginx-prometheus-module/nginx, nginx-prometheus-module/openresty
#     (until v2.3.0) and openresty-prometheus-exporter (v1.x), whole packages;
#   - in nginx, nginx-prometheus-module and openresty, every tagged version
#     that isn't the current one: older versions, the nginx stable line, old
#     flavors (perl, otel, noble, rocky, ...).
#
# The current version is what the moving tags point to (nginx and the module:
# latest, alpine, mainline, mainline-alpine; openresty: latest, alpine,
# bookworm), in either main flavor: tags are compared without their -alpine /
# -bookworm, so 1.29.2.1-1-alpine is current when 1.29.2.1-1-bookworm is
# (promote moves latest and alpine onto a version of their own). Untagged versions
# are never touched: they are the per-platform manifests of the tags that
# stay. A package with none of the moving tags is left alone.
#
#   gh auth refresh -s read:packages,delete:packages
#   sh .github/scripts/prune-images.sh             # dry run: prints what would go
#   sh .github/scripts/prune-images.sh --delete    # deletes it
#
# OWNER (default hatam-abolghasemi) and OWNER_TYPE (users or orgs) select the
# account. Needs gh.
set -eu

OWNER=${OWNER:-hatam-abolghasemi}
OWNER_TYPE=${OWNER_TYPE:-users}
DELETE=false
[ "${1:-}" = --delete ] && DELETE=true

OLD_PACKAGES="nginx-prometheus-module/nginx nginx-prometheus-module/openresty openresty-prometheus-exporter"

# the tags that always point at the current version
MOVING_nginx="latest alpine mainline mainline-alpine"
MOVING_openresty="latest alpine bookworm"

enc() { printf '%s' "$1" | sed 's#/#%2F#g'; }

# a tag without its main-flavor suffix: 1.31.10-alpine -> 1.31.10
stem() {
    case $1 in
        *-alpine)   echo "${1%-alpine}" ;;
        *-bookworm) echo "${1%-bookworm}" ;;
        *)          echo "$1" ;;
    esac
}

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

list=$(mktemp)
trap 'rm -f "$list"' EXIT

for image in nginx nginx-prometheus-module openresty; do
    case $image in
        openresty) moving=$MOVING_openresty ;;
        *)         moving=$MOVING_nginx ;;
    esac

    # "<id> <tag>,<tag>,..." for every tagged version
    if ! api --paginate "/$OWNER_TYPE/$OWNER/packages/container/$image/versions?per_page=100" \
            --jq '.[] | select((.metadata.container.tags | length) > 0)
                      | "\(.id) \(.metadata.container.tags | join(","))"' > "$list" 2> /dev/null
    then
        echo "package  ghcr.io/$OWNER/$image  not found; skipped"
        continue
    fi

    # the current tags, without their flavor: those of every version holding
    # a moving tag
    current=","
    while read -r id tags; do
        for m in $moving; do
            case ",$tags," in
                *",$m,"*)
                    for t in $(echo "$tags" | tr ',' ' '); do
                        current="$current$(stem "$t"),"
                    done
                    break ;;
            esac
        done
    done < "$list"

    if [ "$current" = "," ]; then
        echo "package  ghcr.io/$OWNER/$image  has no $moving tag; left alone"
        continue
    fi

    while read -r id tags; do
        stays=false
        for t in $(echo "$tags" | tr ',' ' '); do
            case $current in *",$(stem "$t"),"*) stays=true; break ;; esac
        done
        $stays && continue

        echo "version  ghcr.io/$OWNER/$image  $tags"
        if $DELETE; then
            api -X DELETE "/$OWNER_TYPE/$OWNER/packages/container/$image/versions/$id"
            echo "         deleted"
        fi
    done < "$list"
done
