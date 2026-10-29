# Windows

The editor's terminal panel needs a POSIX PTY (`fork`, `posix_openpt`).
That is why the native build is a Cygwin or MSYS2 build, or a Linux build
inside WSL. A Visual Studio project is not provided.

The release named `cosmo` is an Actually Portable Executable. It runs on
Windows without those environments. Shell integration and the terminal
panel still prefer a POSIX host. See [release.md](release.md).

## WSL

Install [WSL](https://learn.microsoft.com/windows/wsl/) with Ubuntu, then
follow [linux.md](linux.md) for Debian and Ubuntu:

    sudo apt install build-essential git
    make mle_vendor=1
    ./dist/mle

This is the closest match to the Linux build, including the terminal panel.

## Cygwin

In the Cygwin setup program, install `gcc-core`, `make`, and `git`. Open a
Cygwin shell in this tree:

    make mle_vendor=1
    ./dist/mle

Install with `make install`. The binary is a Cygwin program and wants the
Cygwin DLL on `PATH`.

## MSYS2

From a UCRT64 or MINGW64 shell:

    pacman -S --needed git make mingw-w64-ucrt-x86_64-gcc
    make mle_vendor=1

Use the matching gcc package for the shell you opened (`mingw-w64-x86_64-gcc`
in MINGW64). If the link fails on `posix_openpt` or `fork`, use the Cygwin
or WSL instructions above. Those two provide the PTY the terminal panel uses.

## What not to use

`nmake`, MinGW without a POSIX layer, and the Windows `cmd` compiler driver
are not set up for this tree. Cross-compiling from Linux with MinGW is the
same situation: the editor core may link, and the terminal panel will not,
until a POSIX PTY is available.
