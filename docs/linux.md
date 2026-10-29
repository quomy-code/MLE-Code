# Linux

Install a C compiler, make, and git. Then, from the MLE Code tree:

    make mle_vendor=1
    ./dist/mle

`git` is only required when the `vendor/` submodules are not already checked
out. A release tarball that already contains `vendor/` can build without it.

## Debian and Ubuntu

    sudo apt install build-essential git
    make mle_vendor=1

A fully static binary also needs musl:

    sudo apt install musl-tools
    make mle_vendor=1 mle_static=1 CC=musl-gcc

## Fedora, RHEL, and CentOS

    sudo dnf install gcc make git
    make mle_vendor=1

On older releases the package tool is `yum` with the same package names.

## Arch and derivatives

    sudo pacman -S --needed base-devel git
    make mle_vendor=1

## Alpine

    apk add build-base git
    make mle_vendor=1

Alpine already uses musl, so `mle_static=1` does not need a separate cross
compiler:

    make mle_vendor=1 mle_static=1

## Void

    sudo xbps-install base-devel git
    make mle_vendor=1

## openSUSE

    sudo zypper install gcc make git
    make mle_vendor=1

## Gentoo

    emerge dev-vcs/git
    make mle_vendor=1

A compiler and GNU make come from the stage system. If `make` is missing,
install `sys-devel/gcc` and `sys-devel/make`.

## Nix

    nix-shell -p gnumake gcc git
    make mle_vendor=1

The editor is not installed into a Nix profile by that command. It stays at
`dist/mle` until you copy it yourself.
