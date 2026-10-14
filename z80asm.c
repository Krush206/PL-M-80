/****************************************************************************
 *  plm80 v 5.0                                                             *
 *  Copyright (C) 2023 Andrey Hlus                                          *
 *                                                                          *
 *  Created based on:                                                       *
 *  plm80: C port of Intel's ISIS-II PLM80 v4.0                             *
 *  Copyright (C) 2020 Mark Ogden <mark.pm.ogden@btinternet.com>            *
 *                                                                          *
 *  This program is free software; you can redistribute it and/or           *
 *  modify it under the terms of the GNU General Public License             *
 *  as published by the Free Software Foundation; either version 2          *
 *  of the License, or (at your option) any later version.                  *
 *                                                                          *
 *  This program is distributed in the hope that it will be useful,         *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU General Public License for more details.                            *
 *                                                                          *
 *  You should have received a copy of the GNU General Public License       *
 *  along with this program; if not, write to the Free Software             *
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,              *
 *  MA  02110-1301, USA.                                                    *
 ****************************************************************************/

/*
 * z80asm output: flat Z80 assembly for the z80asm assembler by Bas Wijnen.
 *
 * When the OBJECT control is in effect the compiler writes <stem>.asm next
 * to <stem>.obj.  The file holds the module in z80asm dialect so that
 *
 *     z80asm -o prog.bin prog.asm
 *
 * produces a ready to run flat binary.  The 8080 code generator output is
 * translated 1:1 to Z80 mnemonics; data definitions, stack reservation and
 * cross references are emitted symbolically, so no locator is needed.
 *
 * Layout of the file:
 *     [jp ASM_START]      entry jump (only for modules with a main program)
 *     code                instructions in emission order, labels included
 *     data                initialized variables (db/dw), ds reservations,
 *                         AT/ABSOLUTE aliases (equ)
 *     stack               <mod>_STACK reservation + <mod>_STACK_ORIGIN
 *
 * Multi-module programs: combine the modules with include files, e.g. a
 * top.asm containing  include 'a.asm' / include 'b.asm'.  Generated labels
 * starting with '.' are local to their include file; PUBLIC and EXTERNAL
 * symbols stay global, which is what links the modules together.
 * There is deliberately no `org` directive in the generated file so that
 * includes compose; the default origin is 0.  Prepend `org 100h` (or set it
 * in the top file) for CP/M-style binaries.
 *
 * Limitations (see README.unix.md): INTERRUPT vectors are emitted as
 * comments only, BASED variables get no label.
 */

#include "plm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static byte asmBuf[1280];

/* ---------------- state ---------------- */
static bool z80active = false;
static bool z80haveMain = false;
static bool z80entryPlaced = false;
static word z80mainAddr = 0;
static word z80codeBase = 0;      /* newAddr of first code byte (3 with entry jump) */
static word z80codePos = 0;       /* next code address, relative to z80codeBase */
static word z80codeCount = 0;     /* instructions emitted */
static char z80mod[40];           /* sanitized module name for private labels */
static char z80modRaw[36];        /* module name as written, for the header */
static word z80stackNeed = 16;
static word z80dataBytes = 0; /* data-section bytes: code follows them */

/* ---------------- output buffer (whole file, written at End) ---------------- */
static char *z80out = NULL;
static size_t z80outLen = 0, z80outCap = 0;
static bool z80failed = false;

static void z80_putn(const char *s, size_t n)
{
    char *p;
    size_t need;
    if (z80failed || n == 0)
        return;
    need = z80outLen + n + 1;
    if (need > z80outCap) {
        size_t nc = z80outCap ? z80outCap * 2 : 4096;
        while (nc < need)
            nc *= 2;
        p = realloc(z80out, nc);
        if (!p) {
            z80failed = true;
            return;
        }
        z80out = p;
        z80outCap = nc;
    }
    memcpy(z80out + z80outLen, s, n);
    z80outLen += n;
}

static void z80_put(const char *s)
{
    z80_putn(s, strlen(s));
}

static void z80_putf(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    z80_put(tmp);
}

/* ---------------- small helpers ---------------- */
static char *z80_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* copy the symbol of curInfoP to a malloc'd NUL string; NULL if anonymous */
static char *z80_symname(void)
{
    offset_t s = GetSymbol();
    byte n;
    char *p;
    if (s == 0)
        return NULL;
    n = SymbolP(s)->name[0];
    p = malloc((size_t)n + 1);
    if (!p)
        return NULL;
    memcpy(p, &SymbolP(s)->name[1], n);
    p[n] = 0;
    return p;
}

static void z80_hex(char *dst, word v)
{
    /* Intel-style hex with leading digit, e.g. 0FFH */
    byte tmp[10];
    byte n = Num2Asc(v, 0, -16, tmp);
    memcpy(dst, tmp, n);
    dst[n] = 0;
}

/* plain sanitizer: alnum + _ . kept, '$' -> '_', else '_' */
static void z80_san_plain(const char *src, char *dst, size_t dstsz)
{
    size_t i = 0, o = 0;
    char c;
    while ((c = src[i++]) != 0) {
        char m = c == '$' ? '_' : c;
        if ((m >= 'A' && m <= 'Z') || (m >= 'a' && m <= 'z') ||
            (m >= '0' && m <= '9') || m == '_' || m == '.') {
            if (o == 0 && m >= '0' && m <= '9') {
                if (o + 1 < dstsz)
                    dst[o++] = '_';
            }
            if (o + 1 < dstsz)
                dst[o++] = m;
        } else if (o + 1 < dstsz)
            dst[o++] = '_';
    }
    dst[o] = 0;
}

/* full sanitizer for emitted names (input must already be trimmed) */
static void z80_sanname(const char *src, char *dst, size_t dstsz)
{
    if (src[0] == '@') {
        if (strncmp(src, "@STACK$ORIGIN", 13) == 0) {
            snprintf(dst, dstsz, "%s_STACK_ORIGIN", z80mod[0] ? z80mod : "STACK");
            return;
        }
        if (src[1] == 'P') {
            /* helper entry: numeric id identifies it */
            size_t i = 2, o = 0;
            char c;
            if (o + 1 < dstsz)
                dst[o++] = 'P';
            while ((c = src[i++]) != 0) {
                if (c < '0' || c > '9')
                    break;
                if (o + 1 < dstsz)
                    dst[o++] = c;
            }
            dst[o] = 0;
            return;
        }
        if (src[1] >= '0' && src[1] <= '9') {
            size_t i = 1, o = 0;
            char c;
            if (o + 2 < dstsz) {
                dst[o++] = '.';
                dst[o++] = 'L';
            }
            while ((c = src[i++]) != 0) {
                if (c < '0' || c > '9')
                    break;
                if (o + 1 < dstsz)
                    dst[o++] = c;
            }
            dst[o] = 0;
            return;
        }
        {
            char tmp[64];
            size_t i = 1, o = 0;
            char c;
            if (o + 1 < sizeof tmp)
                tmp[o++] = '_';
            while ((c = src[i++]) != 0 && o + 1 < sizeof tmp) {
                char m = c == '$' ? '_' : c;
                if ((m >= 'A' && m <= 'Z') || (m >= 'a' && m <= 'z') ||
                    (m >= '0' && m <= '9') || m == '_' || m == '.')
                    tmp[o++] = m;
                else
                    tmp[o++] = '_';
            }
            tmp[o] = 0;
            snprintf(dst, dstsz, "%s", tmp);
            return;
        }
    }
    z80_san_plain(src, dst, dstsz);
}

/* trim trailing blanks, tabs and CR (NOT colons: label definitions need them) */
static void z80_trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = 0;
}

/* trim trailing blanks, tabs, colons and CR (for names, not code lines) */
static void z80_trim_name(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == ':' || s[n - 1] == '\r'))
        s[--n] = 0;
}

/* ---------------- unified name table ---------------- */
typedef struct zent_s {
    char *raw;      /* PL/M name as written (malloc'd) */
    char *san;      /* sanitized base (malloc'd) */
    char *label;    /* assigned asm label (malloc'd), NULL until assigned */
    word addr;      /* compiler address */
    byte space;     /* 0 = code, 1 = cseg data, 2 = dseg data, 3 = absolute/equ */
    word size;      /* DS reservations */
    word base;      /* EQU: base info offset (valid when hasBase) */
    bool hasBase;
    bool pub, ext;
    bool isCode;
    bool addrKnown;
    bool done;      /* already emitted to the output */
} zent_t;

static zent_t *zents = NULL;
static size_t zentsN = 0, zentsC = 0;

static zent_t *z80_ent_new(const char *raw)
{
    zent_t *e;
    char san[96];
    char tmp[96];
    if (zentsN >= zentsC) {
        size_t nc = zentsC ? zentsC * 2 : 64;
        zent_t *p = realloc(zents, nc * sizeof *p);
        if (!p)
            return NULL;
        zents = p;
        zentsC = nc;
    }
    e = &zents[zentsN++];
    memset(e, 0, sizeof *e);
    e->raw = z80_strdup(raw ? raw : "");
    snprintf(tmp, sizeof tmp, "%s", raw ? raw : "");
    z80_trim_name(tmp);
    z80_sanname(tmp, san, sizeof san);
    e->san = z80_strdup(san);
    return e;
}

/* shared label assignment rule:
 *   PUBLIC/EXTERNAL -> plain sanitized name
 *   otherwise       -> <mod>_<name>
 * on collision with an assigned label -> suffix _2, _3, ...
 */
static void z80_assign(zent_t *e)
{
    char base[128];
    int suffix = 1;
    if (e->label || !e->san)
        return;
    if (e->pub || e->ext)
        snprintf(base, sizeof base, "%s", e->san);
    else
        snprintf(base, sizeof base, "%s_%s", z80mod[0] ? z80mod : "M", e->san);
    for (;;) {
        char cand[160];
        bool taken = false;
        size_t k;
        if (suffix == 1)
            snprintf(cand, sizeof cand, "%s", base);
        else
            snprintf(cand, sizeof cand, "%s_%d", base, suffix);
        for (k = 0; k < zentsN; k++) {
            if (&zents[k] != e && zents[k].label &&
                strcmp(zents[k].label, cand) == 0) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            e->label = z80_strdup(cand);
            return;
        }
        suffix++;
    }
}

static size_t z80_find(const char *san, zent_t **out, size_t outsz)
{
    size_t i, n = 0;
    for (i = 0; i < zentsN && n < outsz; i++) {
        if (zents[i].san && strcmp(zents[i].san, san) == 0)
            out[n++] = &zents[i];
    }
    return n;
}

/* find the code entry with a sanitized name (code labels never shadow) */
static zent_t *z80_find_code(const char *san)
{
    size_t i;
    for (i = 0; i < zentsN; i++) {
        if (zents[i].isCode && zents[i].san && strcmp(zents[i].san, san) == 0)
            return &zents[i];
    }
    return NULL;
}

/* ---------------- code address map (compiler addr -> new addr) ---------------- */
typedef struct {
    word caddr;
    word naddr;
    word len;
} z80map_t;

static z80map_t *z80map = NULL;
static size_t z80mapN = 0, z80mapC = 0;

static void z80_map_add(word caddr, word len, word naddr)
{
    z80map_t *p;
    if (len == 0)
        return;
    if (z80mapN >= z80mapC) {
        size_t nc = z80mapC ? z80mapC * 2 : 128;
        p = realloc(z80map, nc * sizeof *p);
        if (!p)
            return;
        z80map = p;
        z80mapC = nc;
    }
    p = &z80map[z80mapN++];
    p->caddr = caddr;
    p->naddr = naddr;
    p->len = len;
}

static long z80_map_lookup(word caddr)
{
    size_t i;
    for (i = 0; i < z80mapN; i++) {
        if (caddr >= z80map[i].caddr && caddr < z80map[i].caddr + z80map[i].len)
            return (long)z80map[i].naddr + (caddr - z80map[i].caddr);
    }
    return -1;
}

/* ---------------- deferred `$` markers ---------------- */
typedef struct {
    word caddr;
    size_t pos;
} zmark_t;

static zmark_t *zmarks = NULL;
static size_t zmarksN = 0, zmarksC = 0;

#define ZMARK_LEN 10   /* "@@@HHHH@@@" */

static void z80_mark_add(word caddr, size_t pos)
{
    zmark_t *p;
    if (zmarksN >= zmarksC) {
        size_t nc = zmarksC ? zmarksC * 2 : 16;
        p = realloc(zmarks, nc * sizeof *p);
        if (!p)
            return;
        zmarks = p;
        zmarksC = nc;
    }
    zmarks[zmarksN].caddr = caddr;
    zmarks[zmarksN].pos = pos;
    zmarksN++;
}

/* label (if any) defined exactly at a compiler code address */
static const char *z80_label_at(word caddr)
{
    size_t i;
    for (i = 0; i < zentsN; i++) {
        if (zents[i].isCode && zents[i].addrKnown &&
            zents[i].addr == caddr && zents[i].label)
            return zents[i].label;
    }
    return NULL;
}

/* Format a `$` target: label when known, number when mapped,
 * otherwise a marker for patching at End.  Sets *mark when deferred. */
static void z80_abs_text(word caddr, char *out, size_t outsz, bool *mark)
{
    const char *lab = z80_label_at(caddr);
    long na = z80_map_lookup(caddr);
    char h[16];
    *mark = false;
    if (lab) {
        snprintf(out, outsz, "%s", lab);
        return;
    }
    if (na >= 0) {
        z80_hex(h, (word)na);
        snprintf(out, outsz, "%s", h);
        return;
    }
    snprintf(out, outsz, "@@@%04X@@@", (unsigned)caddr);
    *mark = true;
}

/* ---------------- data capture (Start3) ---------------- */
typedef struct {
    byte kind;      /* 0 = raw byte, 1 = symbolic word */
    byte b0;
    char *sym;      /* kind 1: raw target name */
    word disp;      /* kind 1: +disp */
    word num;       /* kind 1 fallback value when sym is anonymous */
    word taddr;     /* kind 1: target compiler address */
    byte tspace;    /* kind 1: target space (0=code,1=cseg,2=dseg) */
    bool ext;       /* kind 1: external target */
} zitem_t;

typedef struct {
    word info;      /* normalized info offset */
    byte seg;
    word addr;
    word size;
    char *name;     /* raw name or NULL */
    bool pub, ext;
    zitem_t *items;
    size_t nItems, cItems;
    struct zent_s *ent;   /* table entry, linked at flush */
} zvar_t;

static zvar_t *zvars = NULL;
static size_t zvarsN = 0, zvarsC = 0;
static zvar_t zvarCur;
static bool zvarHave = false;
static word *zvisited = NULL;
static size_t zvisitedN = 0, zvisitedC = 0;

/* pending symbolic word: set by Z80_DataSymRef, consumed by Z80_DataValue */
static bool zsymPending = false;
static char *zsymName = NULL;
static word zsymDisp = 0;
static word zsymNum = 0;
static bool zsymExt = false;
static byte zsymSpace = 2;

static word z80_elem_size(void)
{
    byte t = GetType();
    if (t == BYTE_T)
        return 1;
    if (t == ADDRESS_T)
        return 2;
    if (t == STRUCT_T)
        return GetParentOffset();
    return 0;
}

static word z80_var_size(void)
{
    word s = z80_elem_size();
    if (TestInfoFlag(F_ARRAY))
        s *= GetDimension();
    return s;
}

static void z80_item_add(zitem_t it)
{
    zitem_t *p;
    if (!zvarHave)
        return;
    if (zvarCur.nItems >= zvarCur.cItems) {
        size_t nc = zvarCur.cItems ? zvarCur.cItems * 2 : 16;
        p = realloc(zvarCur.items, nc * sizeof *p);
        if (!p)
            return;
        zvarCur.items = p;
        zvarCur.cItems = nc;
    }
    zvarCur.items[zvarCur.nItems++] = it;
}

static void z80_mark_visited(word info)
{
    word *p;
    size_t i;
    for (i = 0; i < zvisitedN; i++) {
        if (zvisited[i] == info)
            return;
    }
    if (zvisitedN >= zvisitedC) {
        size_t nc = zvisitedC ? zvisitedC * 2 : 64;
        p = realloc(zvisited, nc * sizeof *p);
        if (!p)
            return;
        zvisited = p;
        zvisitedC = nc;
    }
    zvisited[zvisitedN++] = info;
}

static bool z80_is_visited(word info)
{
    size_t i;
    for (i = 0; i < zvisitedN; i++) {
        if (zvisited[i] == info)
            return true;
    }
    return false;
}

/* AT base prescan: varInfo (normalized) -> baseInfo (normalized, 0 = absolute) */
typedef struct {
    word varInfo, baseInfo;
} zat_t;

static zat_t *zats = NULL;
static size_t zatsN = 0, zatsC = 0;

void Z80_AtPrescan(void)
{
    byte t;
    if (!z80active)
        return;
    Rewind(&atFile);
    for (;;) {
        Fread(&atFile, &t, 1);
        if (t == 0) {           /* ATI_AHDR */
            byte b[12];
            word v, base;
            size_t i;
            zat_t *p;
            Fread(&atFile, b, 12);
            v = (word)(b[0] | (b[1] << 8));
            base = (word)(b[4] | (b[5] << 8));
            if (v == 0)
                continue;
            for (i = 0; i < zatsN; i++) {
                if (zats[i].varInfo == v) {
                    zats[i].baseInfo = base;
                    break;
                }
            }
            if (i >= zatsN) {
                if (zatsN >= zatsC) {
                    size_t nc = zatsC ? zatsC * 2 : 16;
                    p = realloc(zats, nc * sizeof *p);
                    if (!p)
                        break;
                    zats = p;
                    zatsC = nc;
                }
                zats[zatsN].varInfo = v;
                zats[zatsN].baseInfo = base;
                zatsN++;
            }
        } else if (t == 1) {    /* ATI_DHDR + body */
            byte b[8];
            Fread(&atFile, b, 4);
            for (;;) {
                Fread(&atFile, &t, 1);
                if (t == 2) {
                    Fread(&atFile, b, 2);
                } else if (t == 3) {
                    word len;
                    Fread(&atFile, (pointer)&len, 2);
                    while (len > 0) {
                        word chunk = len > sizeof b ? sizeof b : len;
                        Fread(&atFile, b, chunk);
                        len -= chunk;
                    }
                } else if (t == 4) {
                    Fread(&atFile, b, 8);
                } else {
                    break;      /* ATI_END or ATI_EOF */
                }
            }
            if (t == 6)
                break;
        } else if (t == 6) {
            break;              /* ATI_EOF: consumed, buffer now self-consistent */
        } else if (t == 5) {
            continue;           /* file-level ATI_END: keep going to EOF */
        } else {
            break;              /* unexpected: leave for Sub_49F9 (never happens) */
        }
    }
    Rewind(&atFile);
}

static bool z80_at_base(word varInfo, word *base)
{
    size_t i;
    for (i = 0; i < zatsN; i++) {
        if (zats[i].varInfo == varInfo) {
            *base = zats[i].baseInfo;
            return true;
        }
    }
    return false;
}

void Z80_DataVarBegin(word seg, word addr, bool emit, word infoNorm)
{
    offset_t saveInfo, saveSym;
    if (!z80active)
        return;
    if (zvarHave)
        Z80_DataVarEnd();
    if (!emit)
        return;
    saveInfo = curInfoP;
    saveSym = curSymbolP;
    memset(&zvarCur, 0, sizeof zvarCur);
    zvarCur.info = infoNorm;
    zvarCur.seg = (byte)seg;
    zvarCur.addr = addr;
    curInfoP = (offset_t)infoNorm + botInfo;
    zvarCur.name = z80_symname();
    zvarCur.pub = TestInfoFlag(F_PUBLIC) ? true : false;
    zvarCur.ext = TestInfoFlag(F_EXTERNAL) ? true : false;
    zvarCur.size = z80_var_size();
    z80_mark_visited(infoNorm);
    zvarHave = true;
    curInfoP = saveInfo;
    curSymbolP = saveSym;
}

void Z80_DataValue(word val)
{
    zitem_t it;
    if (!z80active || !zvarHave)
        return;
    if (zsymPending) {
        zsymPending = false;
        it.kind = 1;
        it.b0 = 0;
        it.sym = zsymName;
        zsymName = NULL;
        it.disp = zsymDisp;
        it.num = zsymNum;
        it.tspace = zsymSpace;
        it.ext = zsymExt;
        /* recover target address: value minus disp equals target link */
        if (!zsymExt)
            it.taddr = (word)(zsymNum - zsymDisp);
        else
            it.taddr = 0;
        z80_item_add(it);
        return;
    }
    if (GetType() == BYTE_T) {
        it.kind = 0;
        it.b0 = (byte)(val & 0xff);
        it.sym = NULL;
        it.disp = 0;
        it.num = 0;
        it.ext = false;
        z80_item_add(it);
    } else {
        it.kind = 0;
        it.b0 = (byte)(val & 0xff);
        it.sym = NULL;
        it.disp = 0;
        it.num = 0;
        it.ext = false;
        z80_item_add(it);
        it.b0 = (byte)((val >> 8) & 0xff);
        z80_item_add(it);
    }
}

/* pending target space, parallel to zsym* (set by DataSymRef, read by DataValue) */

void Z80_DataSymRef(bool isExt, word val)
{
    char *nm;
    if (!z80active || !zvarHave)
        return;
    nm = z80_symname();     /* curInfoP is the resolved target here */
    if (zsymName)
        free(zsymName);
    zsymName = nm;
    zsymNum = val;
    if (isExt) {
        zsymDisp = val;     /* external links are zero based */
        zsymExt = true;
        zsymSpace = 0;
    } else {
        word link = GetLinkVal();
        byte t = GetType();
        zsymDisp = (word)(val - link);
        zsymExt = false;
        if (t == PROC_T || t == LABEL_T)
            zsymSpace = 0;
        else if (TestInfoFlag(F_DATA))
            zsymSpace = 1;
        else
            zsymSpace = 2;
    }
    zsymPending = true;
}

void Z80_DataVarEnd(void)
{
    zvar_t *v;
    if (!z80active || !zvarHave)
        return;
    if (zvarsN >= zvarsC) {
        size_t nc = zvarsC ? zvarsC * 2 : 16;
        zvar_t *p = realloc(zvars, nc * sizeof *p);
        if (!p)
            return;
        zvars = p;
        zvarsC = nc;
    }
    v = &zvars[zvarsN++];
    *v = zvarCur;
    memset(&zvarCur, 0, sizeof zvarCur);
    zvarHave = false;
    zsymPending = false;
}

/* uninitialized storage + AT/ABSOLUTE aliases that never reach atFile */
void Z80_DataWalk(void)
{
    offset_t saveInfo, saveSym;
    if (!z80active)
        return;
    saveInfo = curInfoP;
    saveSym = curSymbolP;
    curInfoP = botInfo + 2;
    while (curInfoP != 0) {
        byte t = GetType();
        if ((t == BYTE_T || t == ADDRESS_T || t == STRUCT_T) && GetSymbol() != 0 &&
            !TestInfoFlag(F_MEMBER) && !TestInfoFlag(F_BASED) &&
            !TestInfoFlag(F_EXTERNAL)) {
            word norm = (word)(curInfoP - botInfo);
            if (!z80_is_visited(norm)) {
                char *nm = z80_symname();
                if (TestInfoFlag(F_AT) || TestInfoFlag(F_ABSOLUTE) ||
                    TestInfoFlag(F_MEMORY)) {
                    zent_t *e = z80_ent_new(nm);
                    if (e) {
                        word base = 0;
                        bool found = z80_at_base(norm, &base);
                        e->isCode = false;
                        e->space = 3;
                        e->pub = TestInfoFlag(F_PUBLIC) ? true : false;
                        e->ext = false;
                        e->addr = GetLinkVal();
                        e->addrKnown = true;
                        e->size = 0;
                        e->base = base;
                        e->hasBase = found;
                    }
                } else if (TestInfoFlag(F_PARAMETER)) {
                    word p = curInfoP;
                    bool pext;
                    curInfoP = (offset_t)procInfo[High(GetScope())] + botInfo;
                    pext = TestInfoFlag(F_EXTERNAL) ? true : false;
                    curInfoP = p;
                    if (!pext) {
                        zent_t *e = z80_ent_new(nm);
                        if (e) {
                            e->isCode = false;
                            e->space = 2;
                            e->pub = false;
                            e->ext = false;
                            e->addr = GetLinkVal();
                            e->addrKnown = true;
                            e->size = z80_var_size();
                        }
                    }
                } else {
                    zent_t *e = z80_ent_new(nm);
                    if (e) {
                        e->isCode = false;
                        e->space = (TestInfoFlag(F_DATA) || TestInfoFlag(F_INITIAL)) ? 1 : 2;
                        e->pub = TestInfoFlag(F_PUBLIC) ? true : false;
                        e->ext = false;
                        e->addr = GetLinkVal();
                        e->addrKnown = true;
                        e->size = z80_var_size();
                    }
                }
                if (nm)
                    free(nm);
            }
        }
        AdvNxtInfo();
    }
    curInfoP = saveInfo;
    curSymbolP = saveSym;
}

/* ---------------- 8080 -> Z80 translation ---------------- */
/* map a single 8080 register to Z80; NULL when not a register */
static const char *z80_reg(const char *s)
{
    if (strcmp(s, "A") == 0) return "a";
    if (strcmp(s, "B") == 0) return "b";
    if (strcmp(s, "C") == 0) return "c";
    if (strcmp(s, "D") == 0) return "d";
    if (strcmp(s, "E") == 0) return "e";
    if (strcmp(s, "H") == 0) return "h";
    if (strcmp(s, "L") == 0) return "l";
    if (strcmp(s, "M") == 0) return "(hl)";
    if (strcmp(s, "SP") == 0) return "sp";
    if (strcmp(s, "PSW") == 0) return "af";
    return NULL;
}

/* map a register pair to Z80 */
static const char *z80_regpair(const char *s)
{
    if (strcmp(s, "B") == 0) return "bc";
    if (strcmp(s, "D") == 0) return "de";
    if (strcmp(s, "H") == 0) return "hl";
    if (strcmp(s, "SP") == 0) return "sp";
    if (strcmp(s, "PSW") == 0) return "af";
    return NULL;
}

/* Resolve a 16-bit symbolic operand to its asm label.
 * target = compiler address from opBytes (for shadowed names),
 * dstRec = fixup kind (5 = external: keep the plain name). */
static void z80_ref(const char *raw, word target, byte dstRec, char *out, size_t outsz)
{
    char tmp[96], san[96];
    zent_t *found[16];
    size_t i, nf;
    snprintf(tmp, sizeof tmp, "%s", raw);
    z80_trim(tmp);
    if (tmp[0] == 0) {
        snprintf(out, outsz, "0");
        return;
    }
    if (tmp[0] >= '0' && tmp[0] <= '9') {
        /* numeric literal (Intel hex like 0FFH, decimal): as-is */
        snprintf(out, outsz, "%s", tmp);
        return;
    }
    if (tmp[0] == '@') {
        /* standalone helper bodies/calls are module scoped, external
           helper references stay global (provided by the library) */
        if (tmp[1] == 'P') {
            size_t k = 2, o = 0;
            char num[16], c;
            while ((c = tmp[k++]) != 0) {
                if (c < '0' || c > '9')
                    break;
                if (o + 1 < sizeof num)
                    num[o++] = c;
            }
            num[o] = 0;
            if (dstRec == 5) {
                snprintf(out, outsz, "P%s", num);
                return;
            } else {
                /* table first (collision-safe), else construct */
                char psan[32];
                zent_t *found[8];
                size_t nf, i;
                snprintf(psan, sizeof psan, "P%s", num);
                nf = z80_find(psan, found, 8);
                for (i = 0; i < nf; i++) {
                    if (found[i]->isCode && found[i]->label) {
                        snprintf(out, outsz, "%s", found[i]->label);
                        return;
                    }
                }
                snprintf(out, outsz, "%s_P%s", z80mod[0] ? z80mod : "M", num);
                return;
            }
        }
        z80_sanname(tmp, out, outsz);
        return;
    }
    /* split name+disp on the first + or - after the first char */
    {
        size_t k = 1;
        char c;
        size_t split = 0;
        while ((c = tmp[k]) != 0) {
            if (c == '+' || c == '-') {
                split = k;
                break;
            }
            k++;
        }
        if (split > 0) {
            char disp[32];
            snprintf(disp, sizeof disp, "%s", tmp + split);
            tmp[split] = 0;
            z80_sanname(tmp, san, sizeof san);
            nf = z80_find(san, found, 16);
            if (nf == 1 && found[0]->label) {
                snprintf(out, outsz, "%s%s", found[0]->label, disp);
                return;
            }
            if (nf > 1) {
                for (i = 0; i < nf; i++) {
                    if (found[i]->addrKnown && found[i]->addr == target &&
                        found[i]->label && dstRec != 5) {
                        snprintf(out, outsz, "%s%s", found[i]->label, disp);
                        return;
                    }
                }
            }
            if (nf == 0 || dstRec == 5) {
                snprintf(out, outsz, "%s%s", san, disp);
                return;
            }
            for (i = 0; i < nf; i++) {
                if (!found[i]->isCode && found[i]->label) {
                    snprintf(out, outsz, "%s%s", found[i]->label, disp);
                    return;
                }
            }
            if (found[0]->label)
                snprintf(out, outsz, "%s%s", found[0]->label, disp);
            else
                snprintf(out, outsz, "%s%s", san, disp);
            return;
        }
    }
    z80_sanname(tmp, san, sizeof san);
    nf = z80_find(san, found, 16);
    if (nf == 0 || dstRec == 5) {
        snprintf(out, outsz, "%s", san);
        return;
    }
    if (nf == 1) {
        if (found[0]->label)
            snprintf(out, outsz, "%s", found[0]->label);
        else
            snprintf(out, outsz, "%s", san);
        return;
    }
    /* shadowed name: match by address, preferring the matching space */
    for (i = 0; i < nf; i++) {
        if (found[i]->addrKnown && found[i]->addr == target &&
            found[i]->label && dstRec != 5) {
            if ((dstRec == 1) == (found[i]->isCode ? true : false)) {
                snprintf(out, outsz, "%s", found[i]->label);
                return;
            }
        }
    }
    for (i = 0; i < nf; i++) {
        if (found[i]->addrKnown && found[i]->addr == target && found[i]->label) {
            snprintf(out, outsz, "%s", found[i]->label);
            return;
        }
    }
    for (i = 0; i < nf; i++) {
        if (found[i]->label) {
            snprintf(out, outsz, "%s", found[i]->label);
            return;
        }
    }
    snprintf(out, outsz, "%s", san);
}

/* split a line into tab/comma separated tokens (in place) */
static int z80_split(char *line, char *tok[], int maxtok)
{
    int n = 0;
    char *p = line;
    while (*p && n < maxtok) {
        while (*p == '\t' || *p == ',' || *p == ' ')
            p++;
        if (*p == 0)
            break;
        tok[n++] = p;
        while (*p && *p != '\t' && *p != ',')
            p++;
        if (*p)
            *p++ = 0;
    }
    return n;
}

/* 16-bit operand with possible `$`: resolve now or request a marker.
 * Returns true when out holds final text; false when out holds a marker
 * and *markTarget received the compiler target address. */
static bool z80_op16(const char *raw, word target, byte dstRec,
    char *out, size_t outsz, word *markTarget)
{
    char tmp[96];
    bool mark = false;
    (void)dstRec;
    snprintf(tmp, sizeof tmp, "%s", raw);
    z80_trim(tmp);
    *markTarget = 0;
    if (tmp[0] == '$') {
        z80_abs_text(target, out, outsz, &mark);
        if (mark) {
            *markTarget = target;
            return false;
        }
        return true;
    }
    z80_ref(tmp, target, dstRec, out, outsz);
    return true;
}

/* translate one code-generator line to z80asm.
 * line: NUL-terminated 8080 text (already trimmed); opBytes/opByteCnt/
 * baseAddr/dstRec: context.  markTarget receives a compiler address when
 * the output contains a deferred `$` marker (0 otherwise).
 * returns true when translated (out holds assembly, no newline),
 * false when the caller should emit raw bytes instead. */
static bool z80_translate(char *line, byte *opBytes, byte opByteCnt,
    word baseAddr, byte dstRec, char *out, size_t outsz, word *markTarget)
{
    char *tok[8];
    int nt;
    const char *r1, *r2;
    char a1[128];
    word target = 0;
    (void)baseAddr;
    *markTarget = 0;
    if (opByteCnt == 3)
        target = (word)(opBytes[1] | (opBytes[2] << 8));
    else if (opByteCnt == 2)
        target = (word)(opBytes[0] | (opBytes[1] << 8));

    /* label definitions: "@Pnnn:NAME:" (helper entries; the NAME part
       is documentary, only the @Pnnn part is ever referenced) or plain
       "NAME:".  Entries come from the table so forward references stay
       consistent. */
    {
        size_t L = strlen(line);
        if (L > 0 && line[L - 1] == ':') {
            char san[96];
            char tmp[96];
            char *extra = NULL;
            zent_t *ce;
            size_t k;
            snprintf(tmp, sizeof tmp, "%s", line);
            /* only the first colon-separated part is a referenced label */
            for (k = 0; tmp[k]; k++) {
                if (tmp[k] == ':') {
                    tmp[k] = 0;
                    if (tmp[k + 1])
                        extra = tmp + k + 1;
                    break;
                }
            }
            z80_trim(tmp);
            z80_sanname(tmp, san, sizeof san);
            if (san[0] == '.') {
                if (extra)
                    snprintf(out, outsz, "%s:\t; %s", san, extra);
                else
                    snprintf(out, outsz, "%s:", san);
                return true;
            }
            /* helper bodies are module scoped: find the pre-registered entry */
            if (tmp[0] == '@' && tmp[1] == 'P') {
                zent_t *found[8];
                size_t nf = z80_find(san, found, 8);
                for (k = 0; k < nf; k++) {
                    if (found[k]->isCode) {
                        ce = found[k];
                        ce->addr = baseAddr;
                        ce->addrKnown = true;
                        if (ce->label) {
                            if (extra)
                                snprintf(out, outsz, "%s:\t; %s", ce->label, extra);
                            else
                                snprintf(out, outsz, "%s:", ce->label);
                            return true;
                        }
                    }
                }
                /* not pre-registered (should not happen): create */
                {
                    zent_t *e = z80_ent_new(tmp);
                    if (e) {
                        e->isCode = true;
                        e->pub = false;
                        e->ext = false;
                        e->addr = baseAddr;
                        e->addrKnown = true;
                        z80_assign(e);
                        if (e->label) {
                            if (extra)
                                snprintf(out, outsz, "%s:\t; %s", e->label, extra);
                            else
                                snprintf(out, outsz, "%s:", e->label);
                            return true;
                        }
                    }
                }
                if (extra)
                    snprintf(out, outsz, "%s:\t; %s", san, extra);
                else
                    snprintf(out, outsz, "%s:", san);
                return true;
            }
            ce = z80_find_code(san);
            if (ce && ce->label) {
                ce->addr = baseAddr;
                ce->addrKnown = true;
                if (extra)
                    snprintf(out, outsz, "%s:\t; %s", ce->label, extra);
                else
                    snprintf(out, outsz, "%s:", ce->label);
                return true;
            }
            {
                zent_t *e = z80_ent_new(tmp);
                if (e) {
                    e->isCode = true;
                    e->pub = false;
                    e->ext = false;
                    e->addr = baseAddr;
                    e->addrKnown = true;
                    z80_assign(e);
                    if (e->label) {
                        if (extra)
                            snprintf(out, outsz, "%s:\t; %s", e->label, extra);
                        else
                            snprintf(out, outsz, "%s:", e->label);
                        return true;
                    }
                }
            }
            if (extra)
                snprintf(out, outsz, "%s:\t; %s", san, extra);
            else
                snprintf(out, outsz, "%s:", san);
            return true;
        }
    }

    /* comments / procedure markers */
    if (line[0] == ';') {
        if (strncmp(line, "; PROC", 6) == 0) {
            char *nm = line + 6;
            char san[96];
            zent_t *ce;
            while (*nm == '\t' || *nm == ' ')
                nm++;
            z80_sanname(nm, san, sizeof san);
            ce = z80_find_code(san);
            if (ce && ce->label) {
                ce->addr = baseAddr;
                ce->addrKnown = true;
                snprintf(out, outsz, "%s:\t; PROC %s", ce->label, san);
                return true;
            }
            {
                zent_t *e = z80_ent_new(nm);
                if (e) {
                    e->isCode = true;
                    e->pub = false;
                    e->ext = false;
                    e->addr = baseAddr;
                    e->addrKnown = true;
                    z80_assign(e);
                    if (e->label) {
                        snprintf(out, outsz, "%s:\t; PROC %s", e->label, san);
                        return true;
                    }
                }
            }
            snprintf(out, outsz, "%s:\t; PROC %s", san, san);
            return true;
        }
        snprintf(out, outsz, "%s", line);
        return true;
    }

    nt = z80_split(line, tok, 8);
    if (nt == 0)
        return false;

    /* separate trailing comment tokens */
    {
        int i;
        for (i = 1; i < nt; i++) {
            if (tok[i][0] == ';') {
                nt = i;
                break;
            }
        }
    }

#define OP(i) (i < nt ? tok[i] : "")

    if (strcmp(tok[0], "ADC") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tadc\ta,%s", r1);
        return true;
    }
    if (strcmp(tok[0], "ADI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tadd\ta,%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "ANI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tand\t%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "CALL") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tcall\t%s", a1);
        return true;
    }
    if (strcmp(tok[0], "CMA") == 0) {
        snprintf(out, outsz, "\tcpl");
        return true;
    }
    if (strcmp(tok[0], "DAA") == 0) {
        snprintf(out, outsz, "\tdaa");
        return true;
    }
    if (strcmp(tok[0], "DAD") == 0 && nt == 2 && (r1 = z80_regpair(OP(1))) != NULL) {
        snprintf(out, outsz, "\tadd\thl,%s", r1);
        return true;
    }
    if (strcmp(tok[0], "DCR") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tdec\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "DCX") == 0 && nt == 2 && (r1 = z80_regpair(OP(1))) != NULL) {
        snprintf(out, outsz, "\tdec\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "DI") == 0) {
        snprintf(out, outsz, "\tdi");
        return true;
    }
    if (strcmp(tok[0], "DW") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tdw\t%s", a1);
        return true;
    }
    if (strcmp(tok[0], "EI") == 0) {
        snprintf(out, outsz, "\tei");
        return true;
    }
    if (strcmp(tok[0], "HLT") == 0) {
        snprintf(out, outsz, "\thalt");
        return true;
    }
    if (strcmp(tok[0], "IN") == 0 && nt == 2) {
        snprintf(out, outsz, "\tin\ta,(%s)", OP(1));
        return true;
    }
    if (strcmp(tok[0], "INR") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tinc\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "INX") == 0 && nt == 2 && (r1 = z80_regpair(OP(1))) != NULL) {
        snprintf(out, outsz, "\tinc\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "J") == 0 && nt == 3) {
        const char *cc = NULL;
        word mk = 0;
        if (strcmp(OP(1), "NC") == 0) cc = "nc";
        else if (strcmp(OP(1), "C") == 0) cc = "c";
        else if (strcmp(OP(1), "Z") == 0) cc = "z";
        else if (strcmp(OP(1), "NZ") == 0) cc = "nz";
        if (!cc)
            return false;
        if (!z80_op16(OP(2), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\t%s,%s", cc, a1);
        return true;
    }
    if (strcmp(tok[0], "JC") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\tc,%s", a1);
        return true;
    }
    if (strcmp(tok[0], "JM") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\tm,%s", a1);
        return true;
    }
    if (strcmp(tok[0], "JMP") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\t%s", a1);
        return true;
    }
    if (strcmp(tok[0], "JNC") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\tnc,%s", a1);
        return true;
    }
    if (strcmp(tok[0], "JNZ") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\tnz,%s", a1);
        return true;
    }
    if (strcmp(tok[0], "JPE") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\tpe,%s", a1);
        return true;
    }
    if (strcmp(tok[0], "JZ") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tjp\tz,%s", a1);
        return true;
    }
    if (strcmp(tok[0], "LDA") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tld\ta,(%s)", a1);
        return true;
    }
    if (strcmp(tok[0], "LDAX") == 0 && nt == 2) {
        if (strcmp(OP(1), "B") == 0) {
            snprintf(out, outsz, "\tld\ta,(bc)");
            return true;
        }
        if (strcmp(OP(1), "D") == 0) {
            snprintf(out, outsz, "\tld\ta,(de)");
            return true;
        }
        return false;
    }
    if (strcmp(tok[0], "LHLD") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tld\thl,(%s)", a1);
        return true;
    }
    if (strcmp(tok[0], "LXI") == 0 && nt == 3) {
        word mk = 0;
        if (strcmp(OP(1), "SP") == 0 && strncmp(OP(2), "@STACK", 6) == 0) {
            char san[96];
            z80_sanname(OP(2), san, sizeof san);
            snprintf(out, outsz, "\tld\tsp,%s", san);
            return true;
        }
        r1 = z80_regpair(OP(1));
        if (!r1)
            return false;
        if (!z80_op16(OP(2), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tld\t%s,%s", r1, a1);
        return true;
    }
    if (strcmp(tok[0], "MOV") == 0 && nt == 3 &&
        (r1 = z80_reg(OP(1))) != NULL && (r2 = z80_reg(OP(2))) != NULL) {
        snprintf(out, outsz, "\tld\t%s,%s", r1, r2);
        return true;
    }
    if (strcmp(tok[0], "MVI") == 0 && nt == 3 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tld\t%s,%s", r1, OP(2));
        return true;
    }
    if (strcmp(tok[0], "ORA") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tor\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "OUT") == 0 && nt == 2) {
        snprintf(out, outsz, "\tout\t(%s),a", OP(1));
        return true;
    }
    if (strcmp(tok[0], "PCHL") == 0) {
        snprintf(out, outsz, "\tjp\t(hl)");
        return true;
    }
    if (strcmp(tok[0], "POP") == 0 && nt == 2 && (r1 = z80_regpair(OP(1))) != NULL) {
        snprintf(out, outsz, "\tpop\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "PUSH") == 0 && nt == 2 && (r1 = z80_regpair(OP(1))) != NULL) {
        snprintf(out, outsz, "\tpush\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "RAL") == 0) {
        snprintf(out, outsz, "\trla");
        return true;
    }
    if (strcmp(tok[0], "RAR") == 0) {
        snprintf(out, outsz, "\trra");
        return true;
    }
    if (strcmp(tok[0], "RET") == 0) {
        snprintf(out, outsz, "\tret");
        return true;
    }
    if (strcmp(tok[0], "SBB") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tsbc\ta,%s", r1);
        return true;
    }
    if (strcmp(tok[0], "SHLD") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tld\t(%s),hl", a1);
        return true;
    }
    if (strcmp(tok[0], "SPHL") == 0) {
        snprintf(out, outsz, "\tld\tsp,hl");
        return true;
    }
    if (strcmp(tok[0], "STA") == 0 && nt == 2) {
        word mk = 0;
        if (!z80_op16(OP(1), target, dstRec, a1, sizeof a1, &mk))
            *markTarget = mk;
        snprintf(out, outsz, "\tld\t(%s),a", a1);
        return true;
    }
    if (strcmp(tok[0], "STAX") == 0 && nt == 2) {
        if (strcmp(OP(1), "D") == 0) {
            snprintf(out, outsz, "\tld\t(de),a");
            return true;
        }
        if (strcmp(OP(1), "B") == 0) {
            snprintf(out, outsz, "\tld\t(bc),a");
            return true;
        }
        return false;
    }
    if (strcmp(tok[0], "SUB") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tsub\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "SUI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tsub\t%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "XCHG") == 0) {
        snprintf(out, outsz, "\tex\tde,hl");
        return true;
    }
    if (strcmp(tok[0], "XTHL") == 0) {
        snprintf(out, outsz, "\tex\t(sp),hl");
        return true;
    }
    if (strcmp(tok[0], "CPI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tcp\t%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "ACI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tadc\ta,%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "SBI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tsbc\ta,%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "XRI") == 0 && nt == 2) {
        snprintf(out, outsz, "\txor\t%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "ORI") == 0 && nt == 2) {
        snprintf(out, outsz, "\tor\t%s", OP(1));
        return true;
    }
    if (strcmp(tok[0], "ANA") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tand\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "CMP") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tcp\t%s", r1);
        return true;
    }
    if (strcmp(tok[0], "ADD") == 0 && nt == 2 && (r1 = z80_reg(OP(1))) != NULL) {
        snprintf(out, outsz, "\tadd\ta,%s", r1);
        return true;
    }
    if (strcmp(tok[0], "ADC") == 0 && nt == 1) {
        snprintf(out, outsz, "\tadc\ta,a");
        return true;
    }
    if (strcmp(tok[0], "NEG") == 0) {
        snprintf(out, outsz, "\tneg");
        return true;
    }
    return false;
}

/* ---------------- code hooks (Start4) ---------------- */
static void z80_reg_label(const char *label, word caddr)
{
    zent_t *e = z80_ent_new(label);
    if (!e)
        return;
    e->isCode = true;
    e->pub = false;
    e->ext = false;
    e->addr = caddr;
    e->addrKnown = true;
    free(e->raw);
    e->raw = z80_strdup(label);
    free(e->san);
    e->san = z80_strdup(label);
    free(e->label);
    e->label = z80_strdup(label);
}

void Z80_Label(void)
{
    char raw[40], san[96];
    size_t n;
    if (!z80active)
        return;
    n = locLabStr[0];
    if (n >= sizeof raw)
        n = sizeof raw - 1;
    memcpy(raw, &locLabStr[1], n);
    raw[n] = 0;
    z80_trim(raw);
    z80_sanname(raw, san, sizeof san);
    if (san[0] == '.') {
        z80_putf("%s:\n", san);
        z80_reg_label(san, baseAddr);
        return;
    }
    {
        zent_t *found[8];
        size_t nf = z80_find(san, found, 8);
        size_t i;
        zent_t *e = NULL;
        for (i = 0; i < nf; i++) {
            if (found[i]->isCode) {
                e = found[i];
                break;
            }
        }
        if (!e) {
            e = z80_ent_new(raw);
            if (e) {
                e->isCode = true;
                e->pub = false;
                e->ext = false;
                z80_assign(e);
            }
        }
        if (e && e->label) {
            e->addr = baseAddr;
            e->addrKnown = true;
            z80_putf("%s:\n", e->label);
            return;
        }
        z80_putf("%s:\n", san);
    }
}

/* db fallback for untranslatable lines: exact bytes + original as comment */
static void z80_fallback(const char *text, byte *opBytes, byte opByteCnt)
{
    byte i;
    char h[8];
    z80_put("\tdb\t");
    for (i = 0; i < opByteCnt; i++) {
        z80_hex(h, opBytes[i]);
        z80_put(h);
        if (i + 1 < opByteCnt)
            z80_put(",");
    }
    z80_putf("\t; WARNING: untranslated 8080 '%s'\n", text);
}

void Z80_Code(void)
{
    char text[96], out[256];
    size_t m;
    word myAddr, markTarget = 0;
    bool ok;
    if (!z80active)
        return;
    m = line[0];
    if (m >= sizeof text)
        m = sizeof text - 1;
    memcpy(text, &line[1], m);
    text[m] = 0;
    z80_trim(text);
    myAddr = (word)(z80codeBase + z80codePos);
    if (baseAddr == z80mainAddr && z80haveMain && !z80entryPlaced) {
        z80_put("ASM_START:\n");
        z80entryPlaced = true;
    }
    ok = z80_translate(text, opBytes, opByteCnt, baseAddr, dstRec,
        out, sizeof out, &markTarget);
    if (ok) {
        if (markTarget != 0 || (markTarget == 0 && strstr(out, "@@@") != NULL)) {
            /* deferred `$`: record marker position in the output */
            char *mk = strstr(out, "@@@");
            size_t base = z80outLen;
            z80_put(out);
            if (mk) {
                word tgt = markTarget;
                if (tgt == 0) {
                    /* parse target from marker text "@@@HHHH@@@" */
                    unsigned v = 0;
                    sscanf(mk + 3, "%4X", &v);
                    tgt = (word)v;
                }
                z80_mark_add(tgt, base + (size_t)(mk - out));
            }
            z80_put("\n");
        } else {
            z80_put(out);
            z80_put("\n");
        }
    } else {
        z80_fallback(text, opBytes, opByteCnt);
    }
    z80_map_add(baseAddr, opByteCnt, myAddr);
    z80codePos += opByteCnt;
    if (opByteCnt > 0)
        z80codeCount++;
}

void Z80_Stmt(void)
{
    if (!z80active)
        return;
    z80_putf("\t; STATEMENT # %u\n", (unsigned)stmtNo);
}

void Z80_IntVec(word addr, word target)
{
    char ha[16], ht[16];
    if (!z80active)
        return;
    z80_hex(ha, addr);
    z80_hex(ht, target);
    z80_putf("\t; WARNING: interrupt vector at %s -> %s not emitted"
        " (no flat-binary equivalent)\n", ha, ht);
}

/* resolve a data symref item to text */
static void z80_dsym_text(zitem_t *it, char *out, size_t outsz)
{
    char san[96], h[16];
    zent_t *found[16];
    size_t nf, i;
    if ((it->sym == NULL || it->sym[0] == 0) && !it->ext) {
        z80_hex(h, (word)(it->num & 0xffff));
        snprintf(out, outsz, "%s", h);
        return;
    }
    z80_sanname(it->sym ? it->sym : "", san, sizeof san);
    if (it->ext) {
        if (it->disp != 0) {
            z80_hex(h, it->disp);
            snprintf(out, outsz, "%s+%s", san, h);
        } else
            snprintf(out, outsz, "%s", san);
        return;
    }
    nf = z80_find(san, found, 16);
    for (i = 0; i < nf; i++) {
        if (found[i]->addrKnown && found[i]->addr == it->taddr &&
            found[i]->space == it->tspace && found[i]->label) {
            if (it->disp != 0) {
                z80_hex(h, it->disp);
                snprintf(out, outsz, "%s+%s", found[i]->label, h);
            } else
                snprintf(out, outsz, "%s", found[i]->label);
            return;
        }
    }
    for (i = 0; i < nf; i++) {
        if (!found[i]->isCode && found[i]->addrKnown &&
            found[i]->addr == it->taddr && found[i]->label) {
            if (it->disp != 0) {
                z80_hex(h, it->disp);
                snprintf(out, outsz, "%s+%s", found[i]->label, h);
            } else
                snprintf(out, outsz, "%s", found[i]->label);
            return;
        }
    }
    for (i = 0; i < nf; i++) {
        if (found[i]->addrKnown && found[i]->addr == it->taddr && found[i]->label) {
            if (it->disp != 0) {
                z80_hex(h, it->disp);
                snprintf(out, outsz, "%s+%s", found[i]->label, h);
            } else
                snprintf(out, outsz, "%s", found[i]->label);
            return;
        }
    }
    if (nf == 1 && found[0]->label) {
        if (it->disp != 0) {
            z80_hex(h, it->disp);
            snprintf(out, outsz, "%s+%s", found[0]->label, h);
        } else
            snprintf(out, outsz, "%s", found[0]->label);
        return;
    }
    for (i = 0; i < nf; i++) {
        if (found[i]->label) {
            if (it->disp != 0) {
                z80_hex(h, it->disp);
                snprintf(out, outsz, "%s+%s", found[i]->label, h);
            } else
                snprintf(out, outsz, "%s", found[i]->label);
            return;
        }
    }
    if (it->disp != 0) {
        z80_hex(h, it->disp);
        snprintf(out, outsz, "%s+%s", san, h);
    } else
        snprintf(out, outsz, "%s", san);
}

/* ---------------- data flush (Start4, before code) ---------------- */
static void z80_emit_data_var(zvar_t *v, zent_t *e)
{
    size_t i;
    char h[16];
    char ref[128];
    z80_putf("%s:\n", e->label);
    i = 0;
    while (i < v->nItems) {
        size_t run;
        if (v->items[i].kind == 1) {
            z80_dsym_text(&v->items[i], ref, sizeof ref);
            z80_putf("\tdw\t%s\n", ref);
            z80dataBytes += 2;
            i++;
            continue;
        }
        run = 0;
        z80_put("\tdb\t");
        while (i < v->nItems && v->items[i].kind == 0 && run < 16) {
            z80_hex(h, v->items[i].b0);
            z80_put(h);
            z80dataBytes++;
            i++;
            run++;
            if (i < v->nItems && v->items[i].kind == 0 && run < 16)
                z80_put(",");
        }
        z80_put("\n");
    }
    if (v->nItems == 0) {
        /* no records: reserve space */
        word sz = v->size ? v->size : 1;
        z80_putf("\tds\t%u\n", (unsigned)sz);
        z80dataBytes += sz;
    }
}

void Z80_DataFlush(void)
{
    offset_t saveInfo, saveSym;
    size_t i;
    word p;
    if (!z80active)
        return;
    /* module name + main entry + stack need */
    curInfoP = (offset_t)procInfo[1] + botInfo;
    curSymbolP = GetSymbol();
    if (curSymbolP != 0) {
        byte n = SymbolP(curSymbolP)->name[0];
        char tmp[36];
        if (n >= sizeof tmp)
            n = sizeof tmp - 1;
        memcpy(tmp, &SymbolP(curSymbolP)->name[1], n);
        tmp[n] = 0;
        snprintf(z80modRaw, sizeof z80modRaw, "%s", tmp);
        z80_san_plain(tmp, z80mod, sizeof z80mod);
    }
    z80mainAddr = GetLinkVal();
    z80stackNeed = 16;
    for (p = 1; p <= procCnt; p++) {
        word need;
        curInfoP = (offset_t)procInfo[p] + botInfo;
        if (GetType() != PROC_T || TestInfoFlag(F_EXTERNAL))
            continue;
        need = GetBaseVal();
        if (need > z80stackNeed)
            z80stackNeed = need;
    }
    /* prescan code labels (LABEL_T / PROC_T with symbols) */
    saveInfo = curInfoP;
    saveSym = curSymbolP;
    curInfoP = botInfo + 2;
    while (curInfoP != 0) {
        byte t = GetType();
        if ((t == LABEL_T || t == PROC_T) && GetSymbol() != 0) {
            char *nm = z80_symname();
            if (nm) {
                zent_t *e = z80_ent_new(nm);
                if (e) {
                    e->isCode = true;
                    e->pub = TestInfoFlag(F_PUBLIC) ? true : false;
                    e->ext = TestInfoFlag(F_EXTERNAL) ? true : false;
                    e->addrKnown = false;
                }
                free(nm);
            }
        }
        AdvNxtInfo();
    }
    curInfoP = saveInfo;
    curSymbolP = saveSym;
    /* pre-register helper entries (bodies are emitted late, calls may
       reference them earlier; module scoped like standalone bodies) */
    {
        static const char *hnames[] = {
            "AH", "APH", "B", "BBA", "BPH", "D", "DA", "DB", "DBP",
            "DH", "DHP", "DPH", "H", "PB", "PD", "PDA", "PDB", "PDH", "PH"
        };
        int h;
        for (h = 0; h <= 116; h++) {
            char tmp[16];
            zent_t *e;
            snprintf(tmp, sizeof tmp, "@P%04d", h);
            e = z80_ent_new(tmp);
            if (e) {
                e->isCode = true;
                e->pub = false;
                e->ext = false;
                e->addrKnown = false;
            }
        }
        for (h = 0; h < 19; h++) {
            zent_t *e = z80_ent_new(hnames[h]);
            if (e) {
                e->isCode = true;
                e->pub = false;
                e->ext = false;
                e->addrKnown = false;
            }
        }
    }
    /* initialized variables: create all entries first (phase 1) so
       forward references resolve, then assign + emit (phase 2) */
    for (i = 0; i < zvarsN; i++) {
        zvar_t *v = &zvars[i];
        zent_t *e;
        v->ent = NULL;
        if (!v->name)
            continue;
        e = z80_ent_new(v->name);
        if (!e)
            continue;
        e->isCode = false;
        e->space = v->seg == 1 ? 1 : 2;
        e->pub = v->pub;
        e->ext = v->ext;
        e->addr = v->addr;
        e->addrKnown = true;
        e->size = v->size;
        v->ent = (struct zent_s *)e;
    }
    /* assign labels in two passes (public/global first) so that
       cross-module references keep working whatever collides */
    for (i = 0; i < zentsN; i++) {
        if (!zents[i].label && (zents[i].pub || zents[i].ext))
            z80_assign(&zents[i]);
    }
    for (i = 0; i < zentsN; i++) {
        if (!zents[i].label)
            z80_assign(&zents[i]);
    }
    /* phase 2: emit initialized variables (labels pre-assigned above) */
    z80_put("\n; ---- data (initialized) ----\n");
    for (i = 0; i < zvarsN; i++) {
        zvar_t *v = &zvars[i];
        zent_t *e = (zent_t *)v->ent;
        size_t k;
        if (!v->name) {
            /* anonymous: still emit values to preserve layout */
            char ref[128];
            z80_put("; anonymous data:\n");
            for (k = 0; k < v->nItems; k++) {
                if (v->items[k].kind == 1) {
                    z80_dsym_text(&v->items[k], ref, sizeof ref);
                    z80_putf("\tdw\t%s\n", ref);
                    z80dataBytes += 2;
                } else {
                    char h2[16];
                    z80_hex(h2, v->items[k].b0);
                    z80_putf("\tdb\t%s\n", h2);
                    z80dataBytes++;
                }
            }
            continue;
        }
        if (!e)
            continue;
        z80_assign(e);
        if (!e->label)
            continue;
        z80_emit_data_var(v, e);
        e->done = true;
    }
    /* DS reservations (walk order) */
    z80_put("\n; ---- data (uninitialized) ----\n");
    for (i = 0; i < zentsN; i++) {
        zent_t *e = &zents[i];
        if (e->isCode || e->space != 2 || e->done)
            continue;
        if (!e->label)
            z80_assign(e);
        if (!e->label)
            continue;
        if (e->size == 0)
            e->size = 1;
        z80_putf("%s:\n\tds\t%u\n", e->label, (unsigned)e->size);
        z80dataBytes += e->size;
        e->done = true;
    }
    /* aliases (equ), after everything they may reference */
    z80_put("\n; ---- aliases ----\n");
    for (i = 0; i < zentsN; i++) {
        zent_t *e = &zents[i];
        char h[16];
        if (e->isCode || e->space != 3 || e->done)
            continue;
        if (!e->label)
            z80_assign(e);
        if (!e->label)
            continue;
        if (e->hasBase && e->base != 0) {
            offset_t svI = curInfoP, svS = curSymbolP;
            char *bn = NULL;
            word blink = 0;
            word disp;
            curInfoP = (offset_t)e->base + botInfo;
            while (GetType() != 0 && TestInfoFlag(F_MEMBER))
                curInfoP = GetParentOffset();
            bn = z80_symname();
            blink = GetLinkVal();
            curInfoP = svI;
            curSymbolP = svS;
            if (bn && bn[0]) {
                char san[96];
                zent_t *found[8];
                size_t nf, k;
                const char *bl;
                z80_sanname(bn, san, sizeof san);
                bl = san;
                nf = z80_find(san, found, 8);
                for (k = 0; k < nf; k++) {
                    if (found[k]->label && !found[k]->isCode) {
                        bl = found[k]->label;
                        break;
                    }
                }
                disp = (word)(e->addr - blink);
                if (disp != 0) {
                    z80_hex(h, disp);
                    z80_putf("%s:\tequ\t%s+%s\n", e->label, bl, h);
                } else
                    z80_putf("%s:\tequ\t%s\n", e->label, bl);
                free(bn);
            } else {
                if (bn)
                    free(bn);
                z80_hex(h, e->addr);
                z80_putf("%s:\tequ\t%s\n", e->label, h);
            }
        } else {
            z80_hex(h, e->addr);
            z80_putf("%s:\tequ\t%s\n", e->label, h);
        }
        e->done = true;
    }
    curInfoP = saveInfo;
    curSymbolP = saveSym;
    /* code follows the data bytes in the file: shift its base */
    z80codeBase += z80dataBytes;
}

/* ---------------- begin / end ---------------- */
void Z80_Begin(void)
{
    char tmp[20];
    size_t i, dot = 0;
    bool haveDot = false;
    word status;
    if (!OBJECT)
        return;
    z80haveMain = haveModuleLevelUnit ? true : false;
    /* derive "<stem>.ASM" from the object file name */
    for (i = 0; i < 16 && objFile.fNam[i] != 0; i++) {
        char c = (char)objFile.fNam[i];
        if (c == ' ')
            break;
        tmp[i] = c;
        if (c == '.') {
            dot = i;
            haveDot = true;
        }
    }
    tmp[i] = 0;
    if (haveDot) {
        size_t k = dot + 1;
        const char *ext = "ASM";
        while (*ext && k < 16)
            tmp[k++] = *ext++;
        tmp[k] = 0;
    } else {
        if (i > 12)
            i = 12;
        tmp[i] = 0;
        snprintf(tmp + i, sizeof tmp - i, ".ASM");
    }
    InitF(&asmFile, (pointer)"ASM   ", (pointer)tmp);
    Open(&asmFile.aftn, asmFile.fNam, 2, 0, &status);
    if (status != 0) {
        fprintf(stderr, "warning: cannot create %s, z80asm output disabled\n", tmp);
        return;
    }
    CreatF(&asmFile, asmBuf, sizeof asmBuf, 2);
    z80active = true;
    z80codeBase = z80haveMain ? 3 : 0;
    z80codePos = 0;
}

void Z80_End(void)
{
    char hdr[512];
    size_t hlen, off;
    if (!z80active) {
        z80active = false;
        return;
    }
    z80active = false;
    if (errorLevel != 0 || z80failed) {
        /* incomplete compilation: do not leave a misleading file behind */
        CloseF(&asmFile);
        DeletF(&asmFile);
        return;
    }
    /* stack reservation */
    z80_putf("\n; ---- stack ----\n%s_STACK:\n\tds\t%u\n%s_STACK_ORIGIN:\tequ\t%s_STACK+%u\n",
        z80mod[0] ? z80mod : "M", (unsigned)z80stackNeed,
        z80mod[0] ? z80mod : "M", z80mod[0] ? z80mod : "M", (unsigned)z80stackNeed);
    /* patch deferred `$` markers now that every address is final */
    {
        size_t m;
        for (m = 0; m < zmarksN; m++) {
            word caddr = zmarks[m].caddr;
            size_t pos = zmarks[m].pos;
            const char *lab = z80_label_at(caddr);
            long na = z80_map_lookup(caddr);
            char rep[64];
            size_t replen, k;
            if (lab)
                snprintf(rep, sizeof rep, "%s", lab);
            else if (na >= 0) {
                z80_hex(rep, (word)na);
            } else {
                /* leave the marker: the assembler will report it loudly */
                continue;
            }
            replen = strlen(rep);
            if (replen <= ZMARK_LEN) {
                for (k = replen; k < ZMARK_LEN; k++)
                    rep[k] = ' ';
                memcpy(z80out + pos, rep, ZMARK_LEN);
            } else {
                size_t tail = z80outLen - (pos + ZMARK_LEN);
                size_t need = z80outLen + (replen - ZMARK_LEN) + 1;
                char *nb = realloc(z80out, need);
                size_t t;
                if (!nb)
                    continue;
                for (t = m + 1; t < zmarksN; t++) {
                    if (zmarks[t].pos > pos)
                        zmarks[t].pos += replen - ZMARK_LEN;
                }
                z80out = nb;
                z80outCap = need;
                memmove(z80out + pos + replen, z80out + pos + ZMARK_LEN, tail);
                memcpy(z80out + pos, rep, replen);
                z80outLen += replen - ZMARK_LEN;
            }
        }
    }
    /* header (entry jump only when the entry was actually placed) */
    off = 0;
    off += (size_t)snprintf(hdr + off, sizeof hdr - off,
        "; %s.asm - Z80 assembly generated by the PL/M-80 compiler\n"
        "; Assemble with: z80asm -o %s.bin %s.asm\n"
        "; No `org` is emitted so modules compose with include files\n"
        "; (default origin is 0; prepend `org 100h` for CP/M).\n",
        z80modRaw[0] ? z80modRaw : "out",
        z80modRaw[0] ? z80modRaw : "out",
        z80modRaw[0] ? z80modRaw : "out");
    if (z80haveMain && z80entryPlaced)
        off += (size_t)snprintf(hdr + off, sizeof hdr - off, "\tjp\tASM_START\n");
    hlen = off;
    Fwrite(&asmFile, (pointer)hdr, (word)hlen);
    off = 0;
    while (off < z80outLen) {
        word chunk = (word)((z80outLen - off > 4096) ? 4096 : z80outLen - off);
        Fwrite(&asmFile, (pointer)(z80out + off), chunk);
        off += chunk;
    }
    Fflush(&asmFile);
    CloseF(&asmFile);
}