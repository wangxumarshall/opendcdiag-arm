#!/bin/bash
# pull-images.sh — download the official openEuler aarch64 docker image for
# each LTS release from repo.openeuler.org (China-reachable; docker.io is not
# reachable from this host) and load it into the local podman image store
# under a uniform tag, so docker-build-test.sh can address them without a
# registry.
#
# Images come as openEuler-docker.aarch64.tar.xz under
#   https://repo.openeuler.org/openEuler-<ver>/docker_img/aarch64/
# We retag each to  openeuler-offline:<tag>  where <tag> matches the
# .os-version tag in third-party/rpms/ (e.g. openeuler-offline:24.03-LTS-SP3).
#
# Reverse order: 24.03 first (the buildable baseline), then 22.03, then 20.03.
# Resume-friendly: skips tarballs already downloaded+verified and images
# already loaded.
#
# Usage: ./pull-images.sh [--no-verify]
#   --no-verify  skip sha256 verification (default verifies)
set -euo pipefail

REPO="https://repo.openeuler.org"
IMGDIR="${IMGDIR:-$(pwd)/third-party/docker-images}"
VERIFY="${VERIFY:-1}"
[[ "${1:-}" == "--no-verify" ]] && VERIFY=0

# Reverse order: 24.03 (all SPs), 22.03 (all SPs), 20.03 (all SPs)
# Each entry: <repo-version-dir>|<tag>
VERSIONS=(
    "openEuler-24.03-LTS-SP4|24.03-LTS-SP4"
    "openEuler-24.03-LTS-SP3|24.03-LTS-SP3"
    "openEuler-24.03-LTS-SP2|24.03-LTS-SP2"
    "openEuler-24.03-LTS-SP1|24.03-LTS-SP1"
    "openEuler-24.03-LTS|24.03-LTS"
    "openEuler-22.03-LTS-SP4|22.03-LTS-SP4"
    "openEuler-22.03-LTS-SP3|22.03-LTS-SP3"
    "openEuler-22.03-LTS-SP2|22.03-LTS-SP2"
    "openEuler-22.03-LTS-SP1|22.03-LTS-SP1"
    "openEuler-22.03-LTS|22.03-LTS"
    "openEuler-20.03-LTS-SP4|20.03-LTS-SP4"
    "openEuler-20.03-LTS-SP3|20.03-LTS-SP3"
    "openEuler-20.03-LTS-SP2|20.03-LTS-SP2"
    "openEuler-20.03-LTS-SP1|20.03-LTS-SP1"
    "openEuler-20.03-LTS|20.03-LTS"
)

mkdir -p "$IMGDIR"

podman_tag_exists() {
    podman image inspect "openeuler-offline:$1" >/dev/null 2>&1
}

for entry in "${VERSIONS[@]}"; do
    repover="${entry%%|*}"
    tag="${entry##*|}"
    tarball="$IMGDIR/openEuler-${tag}.aarch64.tar.xz"
    sumfile="$IMGDIR/openEuler-${tag}.aarch64.tar.xz.sha256sum"
    url="$REPO/$repover/docker_img/aarch64/openEuler-docker.aarch64.tar.xz"
    sumurl="$REPO/$repover/docker_img/aarch64/openEuler-docker.aarch64.tar.xz.sha256sum"

    echo "======== $tag ($repover) ========"

    if podman_tag_exists "$tag"; then
        echo "  image openeuler-offline:$tag already loaded — skipping"
        continue
    fi

    # Download tarball if missing or incomplete
    if [ ! -s "$tarball" ]; then
        echo "  downloading $url"
        curl -fSL --retry 3 -o "$tarball" "$url"
    else
        echo "  tarball already present"
    fi

    # sha256 verify — compare the hash directly (the published .sha256sum
    # file names the tarball "openEuler-docker.aarch64.tar.xz", but we store
    # it version-tagged, so filename-based `sha256sum -c` won't match; a
    # direct hash comparison is filename-agnostic and robust).
    if [ "$VERIFY" -eq 1 ]; then
        curl -fsSL --retry 3 -o "$sumfile" "$sumurl" || {
            echo "  (no sha256sum file published; skipping verify)" >&2
        }
        if [ -s "$sumfile" ]; then
            expected=$(awk 'NR==1{print $1}' "$sumfile")
            actual=$(sha256sum "$tarball" | awk '{print $1}')
            if [ "$actual" = "$expected" ]; then
                echo "  sha256 OK"
            else
                echo "  sha256 MISMATCH for $tag (expected=$expected actual=$actual)" >&2
                rm -f "$tarball"
                exit 1
            fi
        fi
    fi

    echo "  podman load -> openeuler-offline:$tag"
    # podman load imports the image with whatever repo:tag is baked into the
    # tarball (e.g. docker.io/library/openeuler-24.03-lts-sp3:latest). Capture
    # that reference and retag to our uniform openeuler-offline:<tag> scheme.
    load_out=$(podman load -i "$tarball" 2>&1)
    # "Loaded image: <ref>" (podman) or "Loaded image ID: <sha>" (older).
    loaded_ref=$(echo "$load_out" | sed -nE 's/.*Loaded image: //p' | head -1)
    if [ -z "$loaded_ref" ]; then
        loaded_ref=$(echo "$load_out" | sed -nE 's/.*Loaded image( ID)?:[[:space:]]*//p' | head -1)
    fi
    if [ -z "$loaded_ref" ]; then
        # fallback: the most recently pulled/loaded image (top of images list)
        loaded_ref=$(podman images -n --format '{{.Repository}}:{{.Tag}}' | head -1)
    fi
    if [ -z "$loaded_ref" ]; then
        echo "  podman load failed for $tag" >&2
        exit 1
    fi
    podman tag "$loaded_ref" "openeuler-offline:$tag" >/dev/null
    # Drop the original internal tag to keep the store tidy (best-effort).
    podman rmi "$loaded_ref" >/dev/null 2>&1 || true
    echo "  done: openeuler-offline:$tag (from $loaded_ref)"
done

echo ""
echo "======== loaded images ========"
podman images --format '{{.Repository}}:{{.Tag}}  {{.Size}}' | grep openeuler-offline | sort
