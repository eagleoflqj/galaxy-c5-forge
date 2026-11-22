#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 4 ]]; then
    echo "Usage: $0 QCOM_GIT_DIR SAMSUNG_GIT_DIR STABLE_GIT_DIR NEW_SOURCE_DIRECTORY" >&2
    exit 2
fi
repo=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
declare -A git_dirs revisions
git_dirs[qcom]=$(realpath -e -- "$1")
git_dirs[samsung]=$(realpath -e -- "$2")
git_dirs[stable]=$(realpath -e -- "$3")
revisions[qcom]=ce6e4e43e6d70f266e114e77a736dcd8e949f111
revisions[samsung]=71b335bed497bbe8b9754c9c986801a66212ce4f
revisions[stable]=87a72e81764d2fc5411706c551462d61cdb97660
out=$(realpath -m -- "$4")
for origin in qcom samsung stable; do
    git --git-dir="${git_dirs[$origin]}" cat-file -e "${revisions[$origin]}^{commit}"
done
mkdir -- "$out"
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
git_base=(git --git-dir="${git_dirs[qcom]}" --work-tree="$out")
GIT_INDEX_FILE="$scratch/index" "${git_base[@]}" read-tree "${revisions[qcom]}"
GIT_INDEX_FILE="$scratch/index" "${git_base[@]}" -c core.autocrlf=false checkout-index -a --prefix="$out/"
while IFS= read -r path; do
    rm -- "$out/$path"
done < "$repo/kernel/excluded-paths.txt"

while IFS=$'\t' read -r path mode oid origin source_path; do
    destination="$out/$path"
    mkdir -p -- "$(dirname -- "$destination")"
    if [[ "$origin" == overlay ]]; then
        cp -- "$repo/kernel/overlay/$source_path" "$scratch/blob"
    else
        git --git-dir="${git_dirs[$origin]}" cat-file blob "${revisions[$origin]}:$source_path" > "$scratch/blob"
    fi
    [[ $(git hash-object --no-filters "$scratch/blob") == "$oid" ]] || { echo "Source mismatch: $path" >&2; exit 1; }
    if [[ -L "$destination" ]]; then unlink -- "$destination"; fi
    case "$mode" in
        100644|100755) install -m "${mode#100}" "$scratch/blob" "$destination" ;;
        120000) ln -s -- "$(cat "$scratch/blob")" "$destination" ;;
        *) echo "Unsupported source mode: $mode" >&2; exit 1 ;;
    esac
done < "$repo/kernel/content.tsv"
