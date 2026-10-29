# Building MLE Code

MLE Code is the mle editor by [adsr](https://github.com/adsr/mle), continued
by quomy. The command you run is still `mle`.

Every supported system uses the same build:

    make mle_vendor=1

That vendors Lua 5.5 and PCRE2, so the distro does not have to ship those
exact libraries. Editor sources are in `src/`. The editor is written to
`dist/mle`. Object files, the static libraries, and the unit-test harness
are written to `.cache/`. The source tree is not filled with `.o`, `.bak`,
or `.out` files.

    ./dist/mle -h
    make test mle_vendor=1
    scripts/dist.sh

`scripts/dist.sh` also copies a named file such as
`dist/mle-code_<version>_<os>_<arch>` and writes `dist/SHA256SUMS`.
The make target it runs is `release`.

`make install` copies `dist/mle` to `/usr/local/bin/mle`. Pass `prefix` to
change that. `make clean` removes `dist/`, `.cache/`, and any leftover
object files in the source tree.

A release build for several systems at once is the GitHub Actions workflow
[mle_release.yml](../.github/workflows/mle_release.yml). It uploads every
finished binary in one release, plus a checksum file. See
[release.md](release.md).

| System | Page |
| --- | --- |
| Debian, Ubuntu, Fedora, RHEL, Arch, Alpine, Void, openSUSE, Gentoo, Nix | [linux.md](linux.md) |
| FreeBSD, OpenBSD, NetBSD | [bsd.md](bsd.md) |
| macOS | [macos.md](macos.md) |
| Windows, WSL, MSYS2, Cygwin | [windows.md](windows.md) |
| Release archives | [release.md](release.md) |

Plain `make`, without `mle_vendor=1`, links `-llua5.5` and `-lpcre2-8` and
expects uthash headers. Use it only when those packages match. Lua 5.4
headers are not enough. `mle_vendor=1` is the build the rest of these pages
use.

`mle_static=1` adds `-static` on top of the vendor libraries:

    make mle_vendor=1 mle_static=1
