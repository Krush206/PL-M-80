# PL/M-80 compiler — Unix/Linux port

This is the Intel PL/M-80 (v3.x) compiler sources adapted to build and run
natively on Unix/Linux.  The original sources targeted ISIS-ii/MS-DOS style
file devices (`:F0:` … `:F9:`) with CRLF line endings; this port keeps the
compiler's behaviour but presents the host with ordinary Unix paths, files
and text conventions.

## Building

```sh
make            # builds ./plm80c
make check      # builds and runs the functional test suite
make clean
```

Requires a C compiler (`gcc` by default — override with `make CC=clang`).
There are no external dependencies.

## Usage

```sh
./plm80c source.plm [controls] [-o addr] [-h]
```

Examples:

```sh
./plm80c hello.plm                      # hello.plm, writes hello.lst + hello.obj
./plm80c hello.plm noprint noobject     # compile only
./plm80c src/app.plm 'print(lst/app.lst)'
./plm80c "'sub dir/app.plm'"            # quoted: may contain spaces
./plm80c ':F1:hello.plm'                # use the :F1: device mapping
./plm80c game.plm -o 0 -h               # Sega Master System ROM (see below)
```

The `-o addr` option sets the initial `org` address of the generated
`.asm` file (`0100h`, `0x100` and `256` all mean the same); without it no
`org` is emitted.  The `-h` option appends a Sega Master System cartridge
header (see below).  Both are removed from the command tail before the
usual controls are parsed, so all existing controls keep working.

Exit status: `0` on success, `1` if the source had errors, `2` for command
line / environment failures.

## Z80 assembly output (z80asm)

Whenever the `OBJECT` control is in effect the compiler writes, next to
`hello.obj`, a `hello.asm` file with the module in the dialect of the
`z80asm` assembler by Bas Wijnen (package `z80asm`):

```sh
./plm80c hello.plm          # writes hello.lst + hello.obj + hello.asm
z80asm -o hello.bin hello.asm
```

`NOOBJECT` suppresses the `.asm` file together with the `.obj` file, and
`OBJECT(dir/name.obj)` places `dir/name.asm` next to it.  The 8080 code the
compiler generates is translated 1:1 to Z80 mnemonics; data definitions,
the stack reservation and cross references are emitted symbolically, so no
locator is needed.  There is deliberately no `org` directive in the file
(the default origin is 0); prepend `org 100h` for CP/M-style binaries.

Multi-module programs combine the per-module files with includes, e.g. a
`top.asm` containing:

```
include 'main.asm'
include 'lib.asm'
```

```sh
z80asm -o prog.bin top.asm
```

Generated labels starting with `.` are local to their include file;
`PUBLIC` and `EXTERNAL` symbols stay global, which is what links the
modules together.  Each module reserves its own stack (`<mod>_STACK`,
sized to the module's maximum stack depth with a 16 byte floor — enlarge
the `ds` if the program nests deeper at run time); execution starts at
the `ASM_START` label of the module that holds the main program.

Limitations:

* `INTERRUPT` vectors have no flat-binary equivalent and are emitted as
  comments only (the handler bodies themselves are translated normally).
* `BASED` variables get no label; referencing one by name fails at
  assembly time (ordinary based access through the base works).
* Modules that are not self-contained (anything `PUBLIC`/`EXTERNAL`, or
  using 16-bit multiply/divide/modulo and similar runtime helpers) call
  out to `Pnnn` helper labels and external symbols.  Provide Z80
  implementations under the same global names (via another include) the
  same way the Intel flow provides `PLM80.LIB`.
* Do not give a `PUBLIC` procedure and a `PUBLIC` variable the same name;
  the flat namespace cannot tell them apart.

## Sega Master System output

`./plm80c game.plm -h` writes a `.asm` whose assembled binary is a 32KB
SMS ROM image: the program followed by the 16-byte cartridge header at
`7FF0H` (`"TMR SEGA"`, reserved `FFFFH`, zero checksum and product code,
version 0, `4CH` = SMS Export with checksum range `0000-7FEF`):

```sh
./plm80c game.plm -h
z80asm -o game.sms game.asm   # exactly 32768 bytes
```

The header always sits at absolute `7FF0H`, so use `-h` with the default
origin 0.  If the program does not fit below the header the compiler
refuses to write the `.asm` file (exit status 2); the `.obj`/`.lst` files
are still produced.

Most emulators boot the ROM as-is; strict ones (and real hardware) also
want a valid checksum.  Patch it after assembling (big-endian 16-bit sum
of bytes `0000-7FEF`):

```sh
python3 -c "d = bytearray(open('game.sms','rb').read())
s = sum(d[0:0x7FF0]) & 0xFFFF
d[0x7FFA] = s >> 8; d[0x7FFB] = s & 0xFF
open('game.sms','wb').write(d)"
```

Two things remain application business.  First, the generated stack
(`<mod>_STACK`, right behind the program) lives in ROM address space on
the SMS, where pushes go nowhere: point `<mod>_STACK_ORIGIN` at RAM
instead (e.g. replace the two stack lines with
`GAME_STACK_ORIGIN: equ 0DFF0H`).  Second, the program starts at address
0 with no BIOS services — bring your own VDP bring-up and put gameplay
behind the `ASM_START` entry point.  Product code, version and region
bytes in the header are compile-time constants in the `.asm` tail; edit
them there if the defaults do not suit the release.

## File naming rules

* **Names may be quoted** with `'...'` or `"..."`, which allows spaces in
  them.  This applies to the source file name and to file names in
  controls: `PRINT('my file.lst')`.

* **Devices** `:F0:` … `:F9:` map to directories through environment
  variables of the same name:

  ```sh
  F1=$HOME/src/plm ./plm80c ':F1:prog.plm'
  ```

  An unmapped device means the current working directory, so plain Unix
  names work without any environment setup.

* **Path separators** may be `/` or `:`; `.`/`..` components work as usual.
  Leading `/` bypasses device mapping.

* **Reads are case-insensitive, names are written lower case.**  The compiler
  looks up each component against the real directory entries (so
  `./HELLO.PLM` finds `hello.plm` on a case-sensitive filesystem); files it
  *creates* are named in lower case.  Extensions are optional when reading:
  `foo` finds `foo.plm`, `foo.PLM`.

* **Outputs land next to the source.**  `./plm80c sub/app.plm` writes
  `sub/app.lst` and `sub/app.obj` (unless `PRINT(...)`/`OBJECT(...)` says
  otherwise, or `NOPRINT`/`NOOBJECT` suppress them).  A directory inside a
  control file name (`PRINT(out/x.lst)`) is relative to the current
  directory.

* **ISIS 8.3 name limits still apply**: a source or include file name has at
  most 8 characters before the dot and 3 after it (`toolongname.plm` is
  rejected with "SOURCE FILE NAME INCORRECT"), and a file name given in a
  control is at most 16 characters long including any directory (longer ones
  are rejected as "ILLEGAL COMMAND TAIL SYNTAX OR VALUE").  Directories
  themselves may have any name and length; source file paths may be up to
  253 characters.

## `$INCLUDE`

```plm
$INCLUDE ('inc1.plm')        /* search: source directory, then cwd      */
$INCLUDE ("inc1.plm")        /* double quotes work the same way         */
$INCLUDE ('sub/inc2.plm')    /* directories allowed                     */
```

Search order:

* a name **without** a directory is looked up next to the including source
  file first, then in the current directory;
* a name **with** a directory is looked up relative to the current
  directory first, then relative to the source file's directory;
* the source file named on the command line itself never falls back to the
  current directory.

As in the original compiler, a `$` control line must start in **column 1**
(`LEFTMARGIN` changes this); an indented `$INCLUDE` is treated as PL/M
source text and produces syntax errors.

## Line endings

Input may be either LF or CRLF.  Output text (console messages and `.lst`
listings) uses LF; object files, `.ixi` cross-reference files and internal
`.tmp` scratch files stay byte-for-byte compatible with the MS-DOS/ISIS
format (CRLF record markers) so that the linker and other tools in this
suite still read them.

## Port notes

Changes made for Unix are confined to the OS-facing layer:

* `io.c` — path parsing/resolution, device mapping, directory-aware open,
  case-insensitive lookup, line-ending translation, EINTR retry,
  `SetDevDir()` for source-relative outputs, and the second lookup
  location (`altName`) used when reading so `$INCLUDE` works from either
  directory.  Space-padded ISIS names are trimmed before parsing, but
  spaces inside names are kept.
* `plma.c` — command-line source file names with directories, optionally
  quoted to allow spaces.
* `plm0b.c` — control file names may be quoted (`'...'` or `"..."`) and
  contain spaces; option strings are NUL-terminated before use.
* `plmfile.c` — `InitF` copies the file name to the end of the string
  instead of stopping at the first blank (names may contain spaces), and
  `file_t.fNam` is NUL-terminated (fixes a buffer overrun in error
  reporting).
* `plm.h` — `MAXLL` raised to 253, `spath_t` gained a `dir` member.
* `data.c`/`plmb.c` — `#pragma off/on(unreferenced)` guarded for
  non-Watcom compilers.

`tests/run_tests.sh` (run via `make check`) covers the above behaviour.
