# MLE Code

MLE Code is a small, flexible, terminal-based text editor written in C.
The command is `mle`.

The original editor is [mle](https://github.com/adsr/mle) by **adsr**.
The C core, the keymap stack, Lua, syntax highlighting, and the 1.8 line
are that project. **quomy** maintains this tree: the terminal panel, the
file list, the git sidebar, themes, the symbol sets, and the `dist/` build.
Other people who patched upstream mle are named in [CHANGELOG.md](CHANGELOG.md).

The license is unchanged. See [LICENSE](LICENSE) (Apache 2.0).

Runs on Linux, Windows (Cygwin, MSYS2, or WSL), FreeBSD, macOS, and more.
Per-system commands live in [docs/](docs/README.md).

### Aims

* Keep codebase small and hackable
* Minimize build-time and run-time dependencies
* Favor simplicity over correctness
* Make extensible and configurable
* Use shell commands to enhance functionality (e.g., grep, tree)

### Features

* Small codebase
* Full UTF-8 support
* Syntax highlighting
* Stackable key maps (modes)
* Extensible via [Lua](https://www.lua.org)
* Scriptable rc file
* Key macros
* Window splitting
* Integrated terminal (tabs, splits, scrollback, drag to resize)
* File list and a git panel with commit, push, pull, and a click-through diff
* Command palette, project tasks, Problems, Outline, completion, and snippets
* Session restore, crash recovery, file watching, folding, and replace previews
* Six color themes, plus themes you name yourself
* Regex search and replace
* Large file support
* Incremental search
* Linear undo and redo
* Multiple cursors
* Auto indent
* Headless mode
* Navigation via [ctags](https://github.com/universal-ctags/ctags)
* Movement via [less](https://www.gnu.org/software/less/)
* Fuzzy file search via [fzf](https://github.com/junegunn/fzf)
* File browsing via [tree](https://oldmanprogrammer.net/source.php?dir=projects/tree)
* File grep via [grep](https://www.gnu.org/software/grep/)
* String manip via [perl](https://www.perl.org/)

### Building

Editor sources are in `src/`. The binary is `dist/mle`. Object files go to `.cache/`, not the source tree.

    $ git clone --recursive https://github.com/quomy-code/MLE-Code.git
    $ cd mle
    $ make mle_vendor=1
    $ ./dist/mle

`mle_vendor=1` builds the bundled Lua 5.5 and PCRE2. That is the build these
docs use. A static binary is `make mle_vendor=1 mle_static=1`.

    $ make test mle_vendor=1
    $ make install              # /usr/local/bin/mle
    $ make install prefix=/usr  # /usr/bin/mle
    $ scripts/dist.sh           # dist/mle-code_<version>_<os>_<arch>

`make clean` removes `dist/`, `.cache/`, and stray `.o`, `.bak`, and `.out`
files. Plain `make` links system `-llua5.5` and `-lpcre2-8` instead. Use it
only when those headers are installed. Lua 5.4 is not a substitute.

Packages, compilers, and the release workflow are in [docs/](docs/README.md):
Linux, BSD, macOS, and Windows.

### Installing from a repo

Upstream mle may be available via your system's package manager. That package
is adsr's release, not necessarily this tree.

    # apt install mle   # Ubuntu and Debian-based distros
    # dnf install mle   # CentOS, RHEL, Fedora-based distros
    # pkg install mle   # FreeBSD
    # yay -S mle        # Arch (via AUR)
    # snap install mle  # all major Linux distros
    # nix-env -i mle    # NixOS (via nixpkgs)
    # apk add mle       # Alpine
    # xbps-install mle  # Void
    # brew install mle  # macOS (Homebrew)
    # port install mle  # macOS (MacPorts)
    # setup-x86.exe -q -P mle # Cygwin

### Basic usage

    $ mle               # Open blank buffer
    $ mle one.c         # Edit one.c
    $ mle one.c:100     # Edit one.c at line 100
    $ mle one.c two.c   # Edit one.c and two.c
    $ mle -h            # Show command line help

The default key bindings are intuitive. Input text as normal, use directional
keys to move around, use `Ctrl-S` to save, `Ctrl-O` to open, `Ctrl-X` to exit.

Press `F1` for a short guide and `F2` for full help.

### Advanced usage: mlerc

mle is customized via command line options. Run `mle -h` to view all cli
options.

To set default options, make an rc file named `~/.mlerc` (or `/etc/mlerc`). The
contents of the rc file are any number of cli options separated by newlines.
Lines that begin with a semi-colon are interpreted as comments.

If `~/.mlerc` is executable, mle executes it and interprets the resulting stdout
as described above. For example, consider the following snippet from an
executable `~/.mlerc` bash(1) script:

    # Define 'test' kmap
    echo '-Ktest,,1'

    # M-q: replace grep with git grep if `.git` exists
    if [ -d ".git" ]; then
      echo '-kcmd_grep,M-q,git grep --color=never -P -i -I -n %s 2>/dev/null'
    fi

    # Set default kmap
    echo '-n test'

This overrides the built-in grep command with `git grep` if `.git` exists in
the current working directory.

### Shell command integration

The following programs will enable or enhance certain features of mle if they
exist in `PATH`.

* [bash](https://www.gnu.org/software/bash/) (tab completion)
* [fzf](https://github.com/junegunn/fzf) (fuzzy file search)
* [grep](https://www.gnu.org/software/grep/) (file grep)
* [less](https://www.gnu.org/software/less/) (less integration)
* [perl](https://www.perl.org/) (perl 1-liners)
* [readtags](https://github.com/universal-ctags/ctags) (ctags integration)
* [tree](https://oldmanprogrammer.net/source.php?dir=projects/tree) (file browsing)

Arbitrary shell commands can also be run via `cmd_shell` (`M-e` by default). If
any text is selected, it is sent to stdin of the command. Any resulting stdout
is inserted into the text buffer.

### Integrated terminal

`f7` or Alt-grave (the key above Tab) opens a terminal panel on the bottom of
the window, like the panel in VS Code. Press the same keys to hide it. On a
terminal that already reports Ctrl+Shift+grave as its own chord (kitty CSI-u,
or xterm modifyOtherKeys), that chord toggles the panel too. Classic terminals
cannot tell it from Ctrl+Space, so `f7` remains the reliable key. While the
panel is focused, keys go to the shell, including `Ctrl-C` and the arrow keys.
Drag the bar between the editor and the shell to resize the panel. It keeps
following the pointer after you leave the panel. `+` on the tab row opens
another shell, and `x` on a tab closes that one. `Alt-t` is still the command
prefix for everything else (`Alt-t ?` lists every command). `Alt-T` opens
another terminal.

`Alt-t v` and `Alt-t s` split the panel. `Alt-t b`, `Alt-t t`, `Alt-t l`,
`Alt-t r`, and `Alt-t m` dock it on the bottom, top, left, right, or over the
whole edit area. `Shift-PgUp` scrolls the 2000-line history. `Ctrl-PgUp` switches
tabs. Shift-arrows select text in the panel. `Alt-t y`, or releasing a mouse
drag, copies the selection into the editor cut buffer and the system clipboard
(OSC 52, plus `wl-copy` or `xclip` when one of those is installed). `Shift-Insert`
and middle-click paste the clipboard. `Ctrl-u` pastes the editor cut buffer.
`Alt-t w` returns focus to the editor and leaves the panel open.

`f8` opens one settings menu: mouse, tabs, syntax, wrap, undo, line numbers,
the file list, git, the terminal, its dock, the theme, symbols, copy, paste,
and rebinding any command to a key. Choosing a row applies it and leaves the
menu open. Escape, or the close row, leaves. Everyday keys are arrows, save,
open, quit, undo, find, cut, and paste. Every other binding still works; `f2`
lists them. Mouse selection is on by default; `-e0` turns it off. A drag
selects each letter under the pointer, including the one you started on.
Double-click selects a word and triple-click selects a line. Releasing a drag
copies and leaves the selection on screen. Typing replaces it. The old `M-e`
shell command is unchanged. Every editor view has a horizontal scrollbar along
the bottom and a vertical one on the right; click the arrows or track, or drag
either thumb. Split views keep their own scrollbars.

### Project tools and code intelligence

`F11` opens the command palette. Type part of a command or feature name, then
choose a row. Project search and replace, tasks, Problems, Outline, completion,
snippets, folding, language-server actions, file format controls, session
recovery, and the extra Git tools live there instead of crowding the editor.

MLE Code restores the last set of named files when it starts empty. Cursor
positions are included. Unsaved buffers are copied atomically to
`~/.mle/state/recovery` every ten seconds. Named files changed by another
process are reported without overwriting local edits. The status bar shows
UTF-8/BOM and LF/CRLF; `File format` changes those values deliberately.

Project configuration is an optional `.mle-project` file in the repository
root. It is intentionally line-based:

    task.build=make
    task.test=make test
    snippet.guard=if (${cursor}) {\n\t\n}
    lsp.command=/usr/local/bin/mle-lsp-adapter

The task runner stores its latest output for Problems; selecting a
`file:line:column` diagnostic jumps to it. Outline uses `ctags` when installed.
Completion and snippets need no external program. The optional language-server
adapter receives `ACTION FILE LINE COLUMN WORD` and prints
`file:line:column` locations, keeping protocol clients outside the small C
core.

Folding is non-destructive: the buffer and undo history retain every line.
The gutter shows `v` for a foldable block and `>` for a collapsed one. Git
marks use `+`, `~`, and `-`. Palette actions show changed lines, a side-by-side
diff, stage only the hunk under the cursor, and resolve conflicts as ours,
theirs, or both.

### Files, git, and the status bar

`f9` shows a file list on the left. Click a file to select it. Double-click,
right-click, or Enter opens it. Click a folder to expand it. Double-click or
right-click steps into it. Backspace steps up. `/` asks for a folder path, and
`~` means your home directory. Drag the blue edge to resize the list.

In the file list, `n` creates a file, `N` creates a directory, `r` renames the
selected entry, and `x` removes a file or empty directory after you type
`yes`. Open buffers are never discarded by a filesystem action.

`f10` switches the same panel to git. The line under the branch is the
GitHub account. If `gh` or `~/.git-credentials` already has one, mle uses
it. `g`, `[`, `]`, Left, and Right switch accounts. `n` starts a login:
username, then password. GitHub wants a personal access token in the
password field. Enter saves that login. `N` forgets a login mle stored.
The next line is the commit message. Enter or Commit commits it. Push, Pull,
and Fetch sit on the row under that. Push still asks you to type `yes`. Pull
runs and shows the result. Click a changed file to open a diff: each block
starts with `line N` so you can see where it changed, then the added and
removed lines. Status labels distinguish modified, staged, untracked, renamed,
deleted, and conflicted files. `f`, or clicking the branch row, filters those
labels; `/` searches changed paths and an empty search clears it. Double-click
or Enter opens the file. `a` and `u` stage or unstage one
file; `A` and `U` apply that to all files. `d` shows one diff, `D` the full
stat, `l` the log, and `L` its graph. `b` lists branches, `B` switches branches,
and `C` creates and switches to a branch. `t` lists tags, `T` creates one, `R`
shows remotes, and `w` lists worktrees. On a changed file, `h` shows its history
and `v` opens blame. `i` lists stashes, `s` creates one, `S` pops the latest
stash after confirmation, and `F` fetches and prunes. `x`
discards one file only after you type `yes`. Escape leaves the field, then
the list. SSH remotes work without adding a GitHub login.

`f8` `symbols` cycles five sets compiled into mle: `plain` (ASCII), `marks`
(arrows and triangles), and three bitmap fonts, `mini`, `slab`, and `line`.
The quiet `mini` set is intended for dense panels. The bitmap fonts are drawn
as braille, so their shapes do not depend on the terminal's font.

The status bar names the file, the line and column, the language, and the git
branch. Click the file name for the buffer list, the line for go-to-line, and
the branch for the git panel. `f1` opens a short guide. `f2` is still the full
binding list.

Six themes ship with mle: `mle`, `paper`, `night`, `forest`, `ember`, and
`ocean`. `f8` cycles them. `newtheme` copies the current theme under a name
you choose. `painttheme` sets one role, for example `comment bold cyan` or
`selection white/blue`. Your themes are written to `~/.mle/themes` and loaded
the next time mle starts. Built-in themes stay as they are; painting one
copies it first.

Syntax highlighting covers the usual source, web, data, and config languages,
including PowerShell, Terraform/HCL, GraphQL, Nix, OCaml, dotenv, and Git
control files. JSON-with-comments and modern C++/TypeScript keywords are also
recognized. A small fallback colors files that do not match a specific pack.
Rules are PCRE, so opening a file stays cheap.

### Advanced usage: Headless mode

mle provides support for non-interactive editing which may be useful for using
the editor as a regular command line tool. In headless mode, mle reads stdin
into a buffer, applies a startup macro if specified, and then writes the buffer
contents to stdout. For example:

    $ echo -n hello | mle -M 'test C-e space w o r l d enter' -p test
    hello world

If stdin is a pipe, mle goes into headless mode automatically. Headless mode can
be explicitly enabled or disabled with the `-H` option.

If stdin is a pipe and headless mode is disabled via `-H0`, mle reads stdin into
a new buffer and then runs as normal in interactive mode.

### Advanced usage: Scripting

mle is extensible via the [Lua](https://www.lua.org) programming language.
Scripts are loaded via the `-x` cli option. Commands registered by scripts can
be mapped to keys as normal via `-k`. See `src/uscript.lua` for a simple example.

There is also a `wren` branch with [Wren](http://wren.io) scripting support.
That work is on pause. It belongs to upstream mle.

### Forks

* [eon](https://github.com/tomas/eon) - mouse support and Notepad-like
  selections
* [turbo-mle](https://github.com/magiblot/turbo-mle) - Turbo Vision port

### Acknowledgments

mle makes extensive use of the following libraries.

* [uthash](https://troydhanson.github.io/uthash) for hash maps and linked lists
* [termbox2](https://github.com/termbox/termbox2) for TUI
* [PCRE2](http://www.pcre.org/) for syntax highlighting and search
