# Releases

One command on the machine you are sitting at:

    scripts/dist.sh

That runs `make release mle_vendor=1`. The make target is `release` so it
does not share a name with the `dist/` directory. You get:

* `dist/mle`, the binary to run and to install
* `dist/mle-code_<version>_<os>_<arch>`, the file to ship
* `dist/SHA256SUMS`, checksums of every `mle-code_*` file in `dist/`

Override the compiler or the tag when you are cross-building or packing a
static Linux binary:

    scripts/dist.sh CC=musl-gcc mle_static=1 mle_dist_tag=linux-x86_64

Object files never go into `dist/`. They stay in `.cache/obj/`. Vendored
Lua and PCRE2 objects stay in `.cache/vendor/`.

## GitHub

[.github/workflows/mle_release.yml](../.github/workflows/mle_release.yml)
runs when a GitHub release is published, and when someone starts it by hand.

It builds four binaries:

| Tag | Runner | What it is |
| --- | --- | --- |
| `linux-x86_64` | `ubuntu-latest` | static musl |
| `linux-aarch64` | `ubuntu-24.04-arm` | static musl |
| `macos` | `macos-latest` | the system compiler |
| `cosmo` | `ubuntu-latest` | [cosmocc](https://cosmo.zip) Actually Portable Executable |

Each job uploads its `dist/mle-code_*` file as an artifact. A final job
downloads every artifact that succeeded, writes one `SHA256SUMS`, and
attaches the whole set to the GitHub release. A failed runner does not
throw away the binaries that did build.

`workflow_dispatch` builds the same matrix and keeps the artifacts on the
workflow run. It does not publish a release.

The `linux-aarch64` job needs a GitHub-hosted arm runner. The `cosmo` job
downloads cosmocc during the run. Both match the release workflow adsr
shipped with mle; the collect step is what puts them in the release together.

## Check a download

    cd dist
    sha256sum -c SHA256SUMS

On macOS, `shasum -a 256 -c SHA256SUMS`.

The file you run day to day can stay `dist/mle`. The versioned names are
for the release.
