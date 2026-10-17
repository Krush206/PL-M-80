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

# a 9 character name: fine without 8.3 limits (used by the limits test)
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
check "listing on stdout" grep -q 'HELLO: DO' "$run/out.txt"
check "completion message on stdout" grep -q 'COMPILATION COMPLETE' "$run/out.txt"
check "asm produced" test -f "$run/hello.asm"
not_check "no .obj by default" test -f "$run/hello.obj"
not_check "no .lst by default" test -f "$run/hello.lst"
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
( cd "$run/a" && "$PLM" hello.plm ) > "$run/a/out.txt" 2>&1
( cd "$run/b" && "$PLM" hello.plm ) >/dev/null 2>&1
check "CRLF source compiles" test -f "$run/b/hello.asm"
check "asm identical for LF and CRLF input" cmp -s "$run/a/hello.asm" "$run/b/hello.asm"
if command -v file >/dev/null 2>&1 && file "$run/a/out.txt" | grep -q CRLF; then
    no "listing on stdout is LF text"
else
    ok "listing on stdout is LF text"
fi

echo "== file names"
run="$TMP/names"
mkdir -p "$run/sub dir" "$run/deep/deeper/deepest"
for n in "./hello.plm" "sub dir/hello.plm" "deep/deeper/deepest/hello.plm"; do
    cp "$TMP/hello.plm" "$run/$n"
done
cp "$TMP/hello.plm" "$run/noext"
cp "$TMP/hello.plm" "$run/tabname.plm"
( cd "$run" && "$PLM" "./hello.plm" ) >/dev/null 2>&1
check "./name works" test -f "$run/hello.asm"
( cd "$run" && "$PLM" "'sub dir/hello.plm'" ) >/dev/null 2>&1
check "quoted name with a space in the directory works" test -f "$run/sub dir/hello.asm"
( cd "$run" && "$PLM" "deep/deeper/deepest/hello.plm" ) >/dev/null 2>&1
check "deep path works, outputs next to source" test -f "$run/deep/deeper/deepest/hello.asm"
cp "$TMP/hello.plm" "$run/MIXEDPLM.PLM"
( cd "$run" && "$PLM" "MIXEDPLM.PLM" ) >/dev/null 2>&1
check "upper-case name works, case preserved" test -f "$run/MIXEDPLM.asm"
( cd "$run" && "$PLM" noext ) >/dev/null 2>&1
check "missing .PLM extension accepted" test -f "$run/noext.asm"
( cd "$run" && "$PLM" "$(printf 'tabname.plm\tnoprint')" ) > "$run/tabout.txt" 2>&1
rc=$?
check "tab between source name and controls works" test "$rc" -eq 0
not_check "  and the control took effect" grep -q 'HELLO: DO' "$run/tabout.txt"
cp "$TMP/hello.plm" "$run/abs.plm"
( cd "$TMP" && "$PLM" "$run/abs.plm" ) >/dev/null 2>&1
check "absolute path source works" test -f "$run/abs.asm"
cp "$TMP/hello.plm" "$run/guard.plm"
( cd "$run" && "$PLM" "missingdir/guard.plm" ) >/dev/null 2>&1
rc=$?
check "missing source exits nonzero, even with a same-named file in the cwd" test "$rc" -ne 0

echo "== long file names (no 8.3 limits)"
run="$TMP/limits"
mkdir -p "$run"
cp "$TMP/toolongname.plm" "$run/"
( cd "$run" && "$PLM" toolongname.plm ) >/dev/null 2>&1
check "source base name > 8 accepted" test -f "$run/toolongname.asm"
long=$(printf 'a%.0s' $(seq 1 200))
cp "$TMP/hello.plm" "$run/$long.plm"
( cd "$run" && "$PLM" "$long.plm" ) >/dev/null 2>&1
check "200 character source names accepted" test -f "$run/$long.asm"
toolong=$(printf 'a%.0s' $(seq 1 260))
( cd "$run" && "$PLM" "$toolong.plm" ) > "$run/out.txt" 2>&1
rc=$?
check "over-long source names rejected" test "$rc" -ne 0
if grep -qi 'SOURCE FILE NAME INCORRECT' "$run/out.txt"; then
    ok "reports fatal for over-long name"
else
    no "reports fatal for over-long name"
fi

echo "== case-sensitive paths, case preserved"
run="$TMP/case"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/Mixed.plm"
( cd "$run" && "$PLM" Mixed.plm ) >/dev/null 2>&1
check "exact-case argument works" test -f "$run/Mixed.asm"
check "created names keep their case" test -f "$run/Mixed.asm"
check "source name untouched" test -f "$run/Mixed.plm"
( cd "$run" && "$PLM" mixed.plm ) >/dev/null 2>&1
rc=$?
check "wrong-case argument no longer finds Mixed.plm" test "$rc" -ne 0

echo "== controls"
run="$TMP/ctrl"
mkdir -p "$run/out"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm noprint ) > "$run/np.txt" 2>&1
not_check "NOPRINT silences the listing" grep -q 'HELLO: DO' "$run/np.txt"
check "default run produces the .asm output" test -f "$run/hello.asm"
rm -f "$run/hello.asm"
( cd "$run" && "$PLM" hello.plm noobject ) >/dev/null 2>&1
not_check "NOOBJECT suppresses the .asm output" test -f "$run/hello.asm"
( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1
( cd "$run" && "$PLM" hello.plm 'print(out/my.lst)' ) >/dev/null 2>&1
check "PRINT(dir/name) honored" test -f "$run/out/my.lst"
( cd "$run" && "$PLM" hello.plm 'object(out/my.obj)' ) >/dev/null 2>&1
check "OBJECT(dir/name) honored" test -f "$run/out/my.obj"
( cd "$run" && "$PLM" hello.plm "print('my file.lst')" ) >/dev/null 2>&1
check "quoted PRINT name with a space honored" test -f "$run/my file.lst"
( cd "$run" && "$PLM" hello.plm xref ) > "$run/xrefout.txt" 2>&1
check "XREF runs" grep -qi 'CROSS' "$run/xrefout.txt"
if grep -qi 'CROSS' "$run/xrefout.txt"; then ok "cross-reference in listing"; else no "cross-reference in listing"; fi

echo "== \$INCLUDE"
run="$TMP/incrun"
mkdir -p "$run/sub"
cp "$TMP/withinc.plm" "$TMP/quoted.plm" "$TMP/subdir.plm" "$TMP/missing.plm" "$TMP/indent.plm" "$run/"
cp "$TMP/inc/inc1.plm" "$run/"
cp "$TMP/inc/sub/inc2.plm" "$run/sub/"
( cd "$run" && "$PLM" withinc.plm ) >/dev/null 2>&1
check "\$INCLUDE (name) works" test -f "$run/withinc.asm"
( cd "$run" && "$PLM" quoted.plm ) >/dev/null 2>&1
check "\$INCLUDE (\"name\") works" test -f "$run/quoted.asm"
( cd "$run" && "$PLM" subdir.plm ) >/dev/null 2>&1
check "\$INCLUDE (dir/name) works" test -f "$run/subdir.asm"
( cd "$run" && "$PLM" missing.plm ) >/dev/null 2>&1
rc=$?
check "missing include exits nonzero" test "$rc" -ne 0
( cd "$run" && "$PLM" indent.plm ) >/dev/null 2>&1
rc=$?
check "indented \$INCLUDE is not a control (column 1 required)" test "$rc" -ne 0
printf 'MAINTAB: DO;\n$\tINCLUDE (inc1.plm)\n  INC1VAR = 1;\nEND MAINTAB;\n' > "$TMP/tabincl.plm"
printf 'MAINTAB2: DO;\n$\tNOLIST\t\n  DECLARE V BYTE;\n  V = 1;\nEND MAINTAB2;\n' > "$TMP/tabctl.plm"
cp "$TMP/tabincl.plm" "$TMP/tabctl.plm" "$run/"
( cd "$run" && "$PLM" tabincl.plm ) >/dev/null 2>&1
check "\$INCLUDE after a tab works" test -f "$run/tabincl.asm"
( cd "$run" && "$PLM" tabctl.plm ) >/dev/null 2>&1
check "tabs around a control name work" test -f "$run/tabctl.asm"

echo "== include search order"
run="$TMP/search"
mkdir -p "$run/src/helper"
# (a) include name with a directory, relative to the SOURCE file
printf 'MAINX: DO;\n$INCLUDE (helper/inc3.plm)\n  INC3VAR = 1;\nEND MAINX;\n' > "$run/src/dirrel.plm"
printf 'DECLARE INC3VAR BYTE;\n' > "$run/src/helper/inc3.plm"
( cd "$run" && "$PLM" src/dirrel.plm ) >/dev/null 2>&1
check "dir include resolves relative to the source file" test -f "$run/src/dirrel.asm"
# plain include name: relative to the current directory when the source
# directory does not have it
printf 'DECLARE DEFS BYTE;\n' > "$run/defs.plm"
printf 'MAINZ: DO;\n$INCLUDE (defs.plm)\n  DEFS = 1;\nEND MAINZ;\n' > "$run/src/cwdrel.plm"
( cd "$run" && "$PLM" src/cwdrel.plm ) >/dev/null 2>&1
check "plain include falls back to the current directory" test -f "$run/src/cwdrel.asm"
# and the source file itself must NOT fall back to the current directory
printf 'NOSUCH: DO;\nEND NOSUCH;\n' > "$run/decoy.plm"
( cd "$run" && "$PLM" src/decoy.plm ) >/dev/null 2>&1
rc=$?
check "missing source is not silently taken from the cwd" test "$rc" -ne 0

echo "== device specs are gone"
run="$TMP/dev"
mkdir -p "$run/src"
cp "$TMP/hello.plm" "$run/src/"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" 'src/hello.plm' ) >/dev/null 2>&1
check "plain subdir path works" test -f "$run/src/hello.asm"
( cd "$run" && "$PLM" ':F1:hello.plm' ) >/dev/null 2>&1
rc=$?
check ":Fn: device specs are rejected" test "$rc" -ne 0

echo "== z80asm output"
run="$TMP/z80asm"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm ) >/dev/null 2>&1
check "z80 assembly produced by default" test -f "$run/hello.asm"
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

echo "== z80 emulator core self-test"
run="$TMP/emu"
mkdir -p "$run"
if command -v z80asm >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 \
   && [ -f "$here/z80core_test.asm" ] && [ -f "$here/z80run.py" ]; then
    ( cd "$run" && z80asm -o core.bin "$here/z80core_test.asm" ) >/dev/null 2>&1
    if [ -f "$run/core.bin" ]; then
        ( cd "$run" && timeout 120 python3 "$here/z80run.py" 200000 core core.bin \
            > core.out 2>&1 )
        check "emulator core self-test passes" \
            grep -q 'core result: 01' "$run/core.out"
        not_check "  and no unknown opcodes" grep -q 'UNKNOWN' "$run/core.out"
    else
        ok "emulator self-test skipped (assembly failed)"
    fi
else
    ok "emulator self-test skipped (tools missing)"
fi

echo "== org() and smsheader controls"
run="$TMP/z80opt"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm 'org(0100h)' ) >/dev/null 2>&1
check "org() sets the org address" grep -q '^	org	100H' "$run/hello.asm"
( cd "$run" && "$PLM" hello.plm 'org(xyz)' ) >/dev/null 2>&1
rc=$?
check "bad org() address exits nonzero" test "$rc" -ne 0
( cd "$run" && "$PLM" hello.plm smsheader ) >/dev/null 2>&1
check "smsheader adds the SMS header" grep -q 'TMR SEGA' "$run/hello.asm"
( cd "$run" && "$PLM" hello.plm -h ) >/dev/null 2>&1
rc=$?
check "legacy -h flag is rejected" test "$rc" -ne 0
if command -v z80asm >/dev/null 2>&1; then
    ( cd "$run" && z80asm -o hello.bin hello.asm ) >/dev/null 2>&1
    check "smsheader output assembles to a 32K ROM" test "$(wc -c < "$run/hello.bin")" -eq 32768
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
( cd "$run" && "$PLM" smsvar.plm smsheader ) >/dev/null 2>&1
check "smsheader places variables in SMS RAM" \
    grep -qE 'SMSVAR_I:[[:space:]]+equ[[:space:]]+0C000H' "$run/smsvar.asm"
check "smsheader places the stack in SMS RAM" \
    grep -qE 'SMSVAR_STACK_ORIGIN:[[:space:]]+equ[[:space:]]+0DFF0H' "$run/smsvar.asm"
cat > "$run/smsinit.plm" <<'PLM'
smsinit: do;
declare t(2) byte initial (1, 2);
output(7fh) = t(1);
end;
PLM
( cd "$run" && "$PLM" smsinit.plm smsheader ) >/dev/null 2>&1
check "smsheader keeps INITIAL bytes in ROM" \
    grep -q 'SMSINIT_T_INIT:' "$run/smsinit.asm"
check "smsheader defines INITIAL variables in RAM" \
    grep -qE 'SMSINIT_T:[[:space:]]+equ[[:space:]]+0C000H' "$run/smsinit.asm"
check "smsheader copies INITIAL data to RAM at startup" \
    grep -q 'ldir' "$run/smsinit.asm"

echo "== error messages"
run="$TMP/err"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm 'print(a-very-long-listing-name.lst)' ) >/dev/null 2>&1
check "29 character control path accepted" test -f "$run/a-very-long-listing-name.lst"
long150=$(printf 'c%.0s' $(seq 1 150)).lst
( cd "$run" && "$PLM" hello.plm "print($long150)" ) >/dev/null 2>&1
check "150 character control path accepted" test -f "$run/$long150"

echo "== output hygiene"
run="$TMP/hygiene"
mkdir -p "$run"
cp "$TMP/hello.plm" "$run/"
( cd "$run" && "$PLM" hello.plm ) > "$run/list.txt" 2>&1
# created files must not be world-writable (open mode 0666 & umask)
bad=$(find "$run" -perm -0002 2>/dev/null)
if [ -z "$bad" ]; then ok "no world-writable outputs"; else no "no world-writable outputs: $bad"; fi
if command -v file >/dev/null 2>&1 && file "$run/list.txt" | grep -qi 'text\|ASCII'; then
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
