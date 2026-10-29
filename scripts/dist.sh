#!/bin/sh
# Build MLE Code for this machine and copy a named binary into dist/.
# Extra make arguments are passed through, for example:
#   scripts/dist.sh mle_static=1
#   scripts/dist.sh CC=musl-gcc mle_static=1 mle_dist_tag=linux-x86_64
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
exec make release mle_vendor=1 "$@"
