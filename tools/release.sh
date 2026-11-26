#!/bin/sh
set -eu

tag=${name:-}
if ! printf '%s\n' "$tag" | LC_ALL=C grep -Eq '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
    echo 'Usage: make release name=v0.1.0' >&2
    exit 2
fi
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo 'Commit the release changes first.' >&2
    exit 1
fi
git fetch origin main
if ! git merge-base --is-ancestor HEAD origin/main; then
    echo 'Push the release commit to origin/main first.' >&2
    exit 1
fi
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    test "$(git rev-list -n1 "$tag")" = "$(git rev-parse HEAD)" || {
        echo "Tag $tag already points to a different commit." >&2
        exit 1
    }
else
    git tag -a "$tag" -m "Undocked $tag"
fi
git push origin "refs/tags/$tag"
