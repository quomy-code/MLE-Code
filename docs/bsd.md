# BSD

MLE Code's makefile is GNU make. The `make` that ships with FreeBSD,
OpenBSD, and NetBSD will not read it. Install `gmake` and use that for every
command below, including `gmake test mle_vendor=1` and `gmake clean`.

The editor needs a POSIX PTY for the integrated terminal. That is part of
these systems. Lua and PCRE2 come from `vendor/` when you pass
`mle_vendor=1`.

## FreeBSD

    pkg install gmake git gcc
    gmake mle_vendor=1
    ./dist/mle

`clang` from the base system is also enough. If `cc` is already clang or
gcc, you can omit the `gcc` package.

## OpenBSD

    pkg_add gmake git gcc
    gmake mle_vendor=1
    ./dist/mle

## NetBSD

    pkgin install gmake git gcc
    gmake mle_vendor=1
    ./dist/mle

On pkgsrc the same packages are `devel/gmake`, `devel/git`, and a compiler
from the base system or `lang/gcc`.

## Install

    gmake install mle_vendor=1
    gmake install mle_vendor=1 prefix=/usr/local

There is no official MLE Code package in the BSD ports trees yet. The
upstream `mle` package, where a port still ships it, is adsr's editor and
does not include this tree's terminal, git panel, or themes.
