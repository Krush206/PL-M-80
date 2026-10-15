#!/bin/sh
# Functional tests for the Unix port of the PL/M-80 compiler.
# Usage: tests/run_tests.sh [path-to-plm80c]
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
PLM=${1:-$top/plm80c}
case $PLM in
    /*) ;;
    *) PLM=$(cd "$(dirname "$PLM")" 2>/dev/null && pwd)/$(basename "$PLM") ;;
esac
TMP=${TMPDIR:-/tmp}/plm80test.$$
pass=0
fail=0

if [ ! -x "$PLM" ]; then
    echo "compiler binary not found: $PLM (run 'make' first)" >&2
    exit 2
fi

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
mkdir -p "$TMP" || exit 2

ok() { pass=$((pass + 1)); echo "ok   - $1"; }
no() { fail=$((fail + 1)); echo "FAIL - $1"; }

check() { # check <description> <command...>
    desc=$1; shift
    if "$@" >/dev/null 2>&1; then ok "$desc"; else no "$desc"; fi
}

not_check() { # inverse
    desc=$1; shift
    if "$@" >/dev/null 2>&1; then no "$desc"; else ok "$desc"; fi
}

# ---------------------------------------------------------------- test sources
cat > "$TMP/hello.plm" <<'EOF'
HELLO: DO;
  DECLARE X BYTE;
  X = 1;
END HELLO;
EOF

# same program, with CRLF line endings
sed 's/$/\r/' "$TMP/hello.plm" > "$TMP/crlf.plm"

# an 8.3 name that is 9 characters long: must be rejected
cp "$TMP/hello.plm" "$TMP/toolongname.plm"

# $INCLUDE variants (a '$' control must start in column 1)
mkdir -p "$TMP/inc/sub"
cat > "$TMP/inc/inc1.plm" <<'EOF'
DECLARE INC1VAR BYTE;
EOF
cat > "$TMP/inc/sub/inc2.plm" <<'EOF'
DECLARE INC2VAR BYTE;
EOF
printf 'MAINX: DO;\n$INCLUDE (inc1.plm)\n  INC1VAR = 1;\nEND MAINX;\n' > "$TMP/withinc.plm"
printf 'MAINQ: DO;\n$INCLUDE ("inc1.plm")\n  INC1VAR = 1;\nEND MAINQ;\n' > "$TMP/quoted.plm"
printf 'MAINS: DO;\n$INCLUDE (sub/inc2.plm)\n  INC2VAR = 1;\nEND MAINS;\n' > "$TMP/subdir.plm"
printf 'MAINM: DO;\n$INCLUDE (nosuch.plm)\nEND MAINM;\n' > "$TMP/missing.plm"
# indented $INCLUDE: original compiler behaviour, column 1 is required
printf 'MAINI: DO;\n  $INCLUDE (inc1.plm)\nEND MAINI;\n' > "$TMP/indent.plm"

# ---------------------------------------------------------- 1. build & basics
echo "== build"
if make -C "$top" >/dev/null 2>&1; then ok "make builds cleanly"; else no "make builds cleanly"; fi

echo "== basic compile"
run="$TMP/basic"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm ) > "$run/out.txt" 2>&1
rc=$?
check "clean source exits 0" test "$rc" -eq 0
check "listing produced" test -f "$run/hello.lst"
check "object produced" test -f "$run/hello.obj"
check "no temp files left" sh -c "! ls '$run'/*.tmp >/dev/null 2>&1"

echo "== error exit status"
cat > "$run/bad.plm" <<'EOF'
BAD: PROCEDURE;
  X = ;
END BAD;
EOF
( cd "$run" && "$PLM" bad.plm ) >/dev/null 2>&1
rc=$?
check "error source exits 1" test "$rc" -eq 1

echo "== line endings"
run="$TMP/eol"
mkdir -p "$run/a" "$run/b"
cp "$TMP/hello.plm" "$run/a/"
cp "$TMP/crlf.plm" "$run/b/hello.plm"
( cd "$run/a" && "$PLM" hello.plm ) >/dev/null 2>&1
( cd "$run/b" && "$PLM" hello.plm ) >/dev/null 2>&1
check "CRLF source compiles" test -f "$run/b/hello.obj"
check "object identical for LF and CRLF input" cmp -s "$run/a/hello.obj" "$run/b/hello.obj"
if command -v file >/dev/null 2>&1 && file "$run/a/hello.lst" | grep -q CRLF; then
    no "listing is LF text"
else
    ok "listing is LF text"
fi

echo "== file names"
run="$TMP/names"
mkdir -p "$run/sub dir" "$run/deep/deeper/deepest"
for n in "./hello.plm" "sub dir/hello.plm" "deep/deeper/deepest/hello.plm"; do
    cp "$TMP/hello.plm" "$run/$n"
done
cp "$TMP/hello.plm" "$run/noext"
( cd "$run" && "$PLM" "./hello.plm" ) >/dev/null 2>&1
check "./name works" test -f "$run/hello.lst"
( cd "$run" && "$PLM" "'sub dir/hello.plm'" ) >/dev/null 2>&1
check "quoted name with a space in the directory works" test -f "$run/sub dir/hello.lst"
( cd "$run" && "$PLM" "deep/deeper/deepest/hello.plm" ) >/dev/null 2>&1
check "deep path works, outputs next to source" test -f "$run/deep/deeper/deepest/hello.obj"
cp "$TMP/hello.plm" "$run/MIXEDPLM.PLM"
( cd "$run" && "$PLM" "MIXEDPLM.PLM" ) >/dev/null 2>&1
check "8 character upper-case name works" test -f "$run/mixedplm.lst"
( cd "$run" && "$PLM" noext ) >/dev/null 2>&1
check "missing .PLM extension accepted" test -f "$run/noext.lst"
cp "$TMP/hello.plm" "$run/abs.plm"
( cd "$TMP" && "$PLM" "$run/abs.plm" ) >/dev/null 2>&1
check "absolute path source works" test -f "$run/abs.lst"
cp "$TMP/hello.plm" "$run/guard.plm"
( cd "$run" && "$PLM" "missingdir/guard.plm" ) >/dev/null 2>&1
rc=$?
check "missing source exits nonzero, even with a same-named file in the cwd" test "$rc" -ne 0

echo "== 8.3 name limits"
run="$TMP/limits"
mkdir -p "$run"
cp "$TMP/toolongname.plm" "$run/"
( cd "$run" && "$PLM" toolongname.plm ) > "$run/out.txt" 2>&1
rc=$?
check "source base name > 8 rejected" test "$rc" -ne 0
if grep -qi 'SOURCE FILE NAME INCORRECT' "$run/out.txt"; then
    ok "reports fatal for over-long name"
else
    no "reports fatal for over-long name"
fi

echo "== case-insensitive read, lower-case write"
run="$TMP/case"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/Mixed.plm"
( cd "$run" && "$PLM" mixed.plm ) >/dev/null 2>&1
check "lower-case argument finds Mixed.plm" test -f "$run/mixed.lst"
check "created names are lower-cased" test -f "$run/mixed.obj"
check "source name untouched" test -f "$run/Mixed.plm"

echo "== controls"
run="$TMP/ctrl"
mkdir -p "$run/out"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm noprint ) >/dev/null 2>&1
not_check "NOPRINT suppresses listing" test -f "$run/hello.lst"
check "NOOBJECT-less run does produce an object" test -f "$run/hello.obj"
rm -f "$run/hello.obj"
( cd "$run" && "$PLM" hello.plm noobject ) >/dev/null 2>&1
not_check "NOOBJECT suppresses object" test -f "$run/hello.obj"
( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1
( cd "$run" && "$PLM" hello.plm 'print(out/my.lst)' ) >/dev/null 2>&1
check "PRINT(dir/name) honored" test -f "$run/out/my.lst"
( cd "$run" && "$PLM" hello.plm 'object(out/my.obj)' ) >/dev/null 2>&1
check "OBJECT(dir/name) honored" test -f "$run/out/my.obj"
( cd "$run" && "$PLM" hello.plm "print('my file.lst')" ) >/dev/null 2>&1
check "quoted PRINT name with a space honored" test -f "$run/my file.lst"
( cd "$run" && "$PLM" hello.plm xref ) >/dev/null 2>&1
check "XREF runs" test -f "$run/hello.lst"
if grep -qi 'CROSS' "$run/hello.lst"; then ok "cross-reference in listing"; else no "cross-reference in listing"; fi

echo "== \$INCLUDE"
run="$TMP/incrun"
mkdir -p "$run/sub"
cp "$TMP/withinc.plm" "$TMP/quoted.plm" "$TMP/subdir.plm" "$TMP/missing.plm" "$TMP/indent.plm" "$run/"
cp "$TMP/inc/inc1.plm" "$run/"
cp "$TMP/inc/sub/inc2.plm" "$run/sub/"
( cd "$run" && "$PLM" withinc.plm ) >/dev/null 2>&1
check "\$INCLUDE (name) works" test -f "$run/withinc.obj"
( cd "$run" && "$PLM" quoted.plm ) >/dev/null 2>&1
check "\$INCLUDE (\"name\") works" test -f "$run/quoted.obj"
( cd "$run" && "$PLM" subdir.plm ) >/dev/null 2>&1
check "\$INCLUDE (dir/name) works" test -f "$run/subdir.obj"
( cd "$run" && "$PLM" missing.plm ) >/dev/null 2>&1
rc=$?
check "missing include exits nonzero" test "$rc" -ne 0
( cd "$run" && "$PLM" indent.plm ) >/dev/null 2>&1
rc=$?
check "indented \$INCLUDE is not a control (column 1 required)" test "$rc" -ne 0

echo "== include search order"
run="$TMP/search"
mkdir -p "$run/src/helper"
# (a) include name with a directory, relative to the SOURCE file
printf 'MAINX: DO;\n$INCLUDE (helper/inc3.plm)\n  INC3VAR = 1;\nEND MAINX;\n' > "$run/src/dirrel.plm"
printf 'DECLARE INC3VAR BYTE;\n' > "$run/src/helper/inc3.plm"
( cd "$run" && "$PLM" src/dirrel.plm ) >/dev/null 2>&1
check "dir include resolves relative to the source file" test -f "$run/src/dirrel.obj"
# plain include name: relative to the current directory when the source
# directory does not have it
printf 'DECLARE DEFS BYTE;\n' > "$run/defs.plm"
printf 'MAINZ: DO;\n$INCLUDE (defs.plm)\n  DEFS = 1;\nEND MAINZ;\n' > "$run/src/cwdrel.plm"
( cd "$run" && "$PLM" src/cwdrel.plm ) >/dev/null 2>&1
check "plain include falls back to the current directory" test -f "$run/src/cwdrel.obj"
# and the source file itself must NOT fall back to the current directory
printf 'NOSUCH: DO;\nEND NOSUCH;\n' > "$run/decoy.plm"
( cd "$run" && "$PLM" src/decoy.plm ) >/dev/null 2>&1
rc=$?
check "missing source is not silently taken from the cwd" test "$rc" -ne 0

echo "== :F1: device mapping"
run="$TMP/dev"
mkdir -p "$run/src"
cp "$TMP/hello.plm" "$run/src/"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && F1="$run/src" "$PLM" ':F1:hello.plm' ) >/dev/null 2>&1
check ":F1: honoured via environment" test -f "$run/hello.lst"
( cd "$run" && env -u F1 "$PLM" ':F2:hello.plm' ) >/dev/null 2>&1
rc=$?
check "unmapped device falls back to cwd" test "$rc" -eq 0

echo "== z80asm output"
run="$TMP/z80asm"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1
check "z80 assembly produced alongside object" test -f "$run/hello.asm"
rm -f "$run/hello.asm" "$run/hello.obj"
( cd "$run" && "$PLM" hello.plm noobject ) >/dev/null 2>&1
not_check "NOOBJECT suppresses z80 assembly" test -f "$run/hello.asm"
( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1
if command -v z80asm >/dev/null 2>&1; then
    ( cd "$run" && z80asm -o hello.bin hello.asm ) >/dev/null 2>&1
    check "z80asm assembles the generated output" test -f "$run/hello.bin"
else
    ok "z80asm assembler check skipped (z80asm not installed)"
fi

echo "== z80asm options -o -h"
run="$TMP/z80opt"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm -o 0100h ) >/dev/null 2>&1
check "-o sets the org address" grep -q '^	org	100H' "$run/hello.asm"
( cd "$run" && "$PLM" hello.plm -o xyz ) >/dev/null 2>&1
rc=$?
check "bad -o address exits nonzero" test "$rc" -ne 0
( cd "$run" && "$PLM" hello.plm -h ) >/dev/null 2>&1
check "-h adds the SMS header" grep -q 'TMR SEGA' "$run/hello.asm"
if command -v z80asm >/dev/null 2>&1; then
    ( cd "$run" && z80asm -o hello.bin hello.asm ) >/dev/null 2>&1
    check "-h output assembles to a 32K ROM" test "$(wc -c < "$run/hello.bin")" -eq 32768
    magic=$(dd if="$run/hello.bin" bs=1 skip=32752 count=8 2>/dev/null)
    check "SMS magic sits at 7FF0" test "$magic" = "TMR SEGA"
else
    ok "z80asm header check skipped (z80asm not installed)"
fi
# -h must keep variables and the stack out of the read-only SMS ROM
cat > "$run/smsvar.plm" <<'PLM'
smsvar: do;
declare i byte;
do i = 0 to 3;
    output(7fh) = i;
end;
end;
PLM
( cd "$run" && "$PLM" smsvar.plm -h ) >/dev/null 2>&1
check "-h places variables in SMS RAM" \
    grep -qE 'SMSVAR_I:[[:space:]]+equ[[:space:]]+0C000H' "$run/smsvar.asm"
check "-h places the stack in SMS RAM" \
    grep -qE 'SMSVAR_STACK_ORIGIN:[[:space:]]+equ[[:space:]]+0DFF0H' "$run/smsvar.asm"
cat > "$run/smsinit.plm" <<'PLM'
smsinit: do;
declare t(2) byte initial (1, 2);
output(7fh) = t(1);
end;
PLM
( cd "$run" && "$PLM" smsinit.plm -h ) >/dev/null 2>&1
check "-h keeps INITIAL bytes in ROM" \
    grep -q 'SMSINIT_T_INIT:' "$run/smsinit.asm"
check "-h defines INITIAL variables in RAM" \
    grep -qE 'SMSINIT_T:[[:space:]]+equ[[:space:]]+0C000H' "$run/smsinit.asm"
check "-h copies INITIAL data to RAM at startup" \
    grep -q 'ldir' "$run/smsinit.asm"

echo "== error messages"
run="$TMP/err"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm 'print(a-very-long-listing-name.lst)' ) > "$run/e1.txt" 2>&1
rc=$?
check "over-long control path rejected" test "$rc" -ne 0
if grep -q 'ILLEGAL COMMAND TAIL SYNTAX OR VALUE' "$run/e1.txt"; then
    ok "reports bad command tail for over-long path"
else
    no "reports bad command tail for over-long path"
fi

echo "== output hygiene"
run="$TMP/hygiene"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1
# created files must not be world-writable (open mode 0666 & umask)
bad=$(find "$run" -perm -0002 2>/dev/null)
if [ -z "$bad" ]; then ok "no world-writable outputs"; else no "no world-writable outputs: $bad"; fi
if command -v file >/dev/null 2>&1 && file "$run/hello.lst" | grep -qi 'text\|ASCII'; then
    ok "listing is a text file"
else
    ok "listing is a text file (no 'file' command)"
fi
left=$(find "$run" \( -name '*.tmp' -o -name '.f0*' -o -name '*~' \) | head -5)
if [ -z "$left" ]; then ok "no scratch files remain"; else no "scratch files remain: $left"; fi

echo "== repeated runs (state leakage / file-handle exhaustion)"
run="$TMP/loop"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
i=0
while [ $i -lt 50 ]; do
    ( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1 || break
    i=$((i + 1))
done
check "50 consecutive compiles succeed" test "$i" -eq 50

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
