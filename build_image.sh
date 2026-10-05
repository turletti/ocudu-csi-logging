#!/bin/bash
# Builds an OCUDU gNB image with the per-RB SRS CSI logger: clones an OCUDU tag, applies patch_ocudu_srs_csi.sh and
# builds the image.
#   dpdk variant (default): upstream docker/Dockerfile, unmodified, target runtime-dpdk, COMPONENT=gnb (O-RAN 7.2
#                           fronthaul, e.g. Benetel). Tag <image>:<ocudu_tag>.
#   zmq variant:            docker/Dockerfile.zmq of this repo (upstream images have no ZeroMQ). Tag <image>:<ocudu_tag>-zmq.
#
# Usage: build_image.sh [-t OCUDU_TAG] [-v dpdk|zmq] [-m MARCH] [-j NUM_JOBS] [-i IMAGE] [-r OCUDU_REPO] [-s SRC_DIR]
#                       [-- extra docker build args]
#   -t  OCUDU tag or branch            (default release_26_10)
#   -v  variant                         (default dpdk)
#   -m  -march of the RAN node CPU      (default native: only right when building on a machine of the same CPU family)
#   -j  parallel jobs                   (default: all cores)
#   -i  image name                      (default ocudu-gnb-csi)
#   -r  OCUDU repository                (default https://gitlab.com/ocudu/ocudu.git)
#   -s  use this OCUDU source tree (copied, never modified) instead of cloning, e.g. an extracted git archive
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OCUDU_TAG=release_26_10
VARIANT=dpdk
MARCH=native
NUM_JOBS=""
IMAGE=ocudu-gnb-csi
OCUDU_REPO=https://gitlab.com/ocudu/ocudu.git
SRC=""
while getopts "t:v:m:j:i:r:s:h" opt; do
  case $opt in
    t) OCUDU_TAG=$OPTARG ;;
    v) VARIANT=$OPTARG ;;
    m) MARCH=$OPTARG ;;
    j) NUM_JOBS=$OPTARG ;;
    i) IMAGE=$OPTARG ;;
    r) OCUDU_REPO=$OPTARG ;;
    s) SRC=$OPTARG ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
case $VARIANT in dpdk | zmq) ;; *) echo "unknown variant $VARIANT (dpdk|zmq)" >&2; exit 2 ;; esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ -n "$SRC" ]; then
  cp -a "$SRC/." "$WORK/ocudu"
else
  git clone --quiet --depth 1 --branch "$OCUDU_TAG" "$OCUDU_REPO" "$WORK/ocudu"
fi
OCUDU_REV=$(git -C "$WORK/ocudu" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
CSI_REV=$(git -C "$HERE" describe --always --dirty 2>/dev/null || echo unknown)

"$HERE/patch_ocudu_srs_csi.sh" "$WORK/ocudu"

LABELS=(--label "ocudu.tag=$OCUDU_TAG" --label "ocudu.revision=$OCUDU_REV" --label "csi.logger.revision=$CSI_REV"
        --label "csi.format_version=3.1")
if [ $VARIANT = dpdk ]; then
  TAG="$IMAGE:$OCUDU_TAG"
  docker build -f "$WORK/ocudu/docker/Dockerfile" --target runtime-dpdk \
    --build-arg COMPONENT=gnb --build-arg MARCH="$MARCH" --build-arg NUM_JOBS="$NUM_JOBS" \
    --build-arg OCUDU_IMAGE_VERSION="$OCUDU_TAG-csi" "${LABELS[@]}" -t "$TAG" "$@" "$WORK/ocudu"
else
  TAG="$IMAGE:$OCUDU_TAG-zmq"
  docker build -f "$HERE/docker/Dockerfile.zmq" \
    --build-arg MARCH="$MARCH" --build-arg NUM_JOBS="$NUM_JOBS" \
    --build-arg OCUDU_IMAGE_VERSION="$OCUDU_TAG-csi" "${LABELS[@]}" -t "$TAG" "$@" "$WORK/ocudu"
fi
echo "built $TAG (OCUDU $OCUDU_TAG $OCUDU_REV, CSI logger $CSI_REV, -march=$MARCH)"
