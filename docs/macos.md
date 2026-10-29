# macOS

Install the command line tools, then build with the system compiler:

    xcode-select --install
    make mle_vendor=1
    ./dist/mle

That uses Apple clang. The integrated terminal uses the system PTY.

Homebrew is optional. It is useful when you want a newer GNU make, or the
shell tools mle can call:

    brew install make git fzf universal-ctags
    gmake mle_vendor=1

If both `make` and `gmake` exist, either one is fine as long as it is GNU
make. Apple's make is GNU make and can read this makefile.

MacPorts, if you use it instead of Homebrew:

    sudo port install gmake
    gmake mle_vendor=1

`make install` copies the binary to `/usr/local/bin/mle`. A prefix under
your home directory avoids writing outside it:

    make install mle_vendor=1 prefix="$HOME/.local"

A named archive for this Mac is:

    scripts/dist.sh

The file looks like `dist/mle-code_<version>_darwin_<arch>`.

The GitHub release workflow also builds `macos` on `macos-latest` and, under
the name `cosmo`, one Actually Portable Executable that runs on macOS as
well. See [release.md](release.md).
