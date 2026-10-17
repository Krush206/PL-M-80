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


// vim:ts=4:shiftwidth=4:expandtab:
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include <unistd.h>
#include <errno.h>

#include <string.h>

#ifdef _WIN32
#include <io.h>
#define close   _close
#define open    _open
#define read    _read
#define write   _write
#define unlink  _unlink
#define lseek   _lseek
#else
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#define _MAX_PATH 4096
#define O_BINARY    0
#endif


#include "plm.h"

#define RANDOM_ACCESS   4       // additional access mode

typedef struct {
    byte        deviceId; // 23 for the :CI: console, 0 for plain files
    byte        modes;    // supported modes READ_MODE, WRITE_MODE, UPDATE_MODE and RANDOM_ACCESS
    byte        name[_MAX_PATH];
    byte        altName[_MAX_PATH]; // other place to look when reading (Unix)
} osfile_t;

#define STDIN   0
#define STDOUT  1

#define CO_DEV  0
#define CI_DEV  1
#define BB_DEV  2               // always provide BB it is harmless and simplifies things

#define AFTSIZE 20              // more files than usually available to ISIS

/*
   Text files (the console and the listing file) are written with <LF> line
   endings on Unix/Linux.  A <CR> is held back until the next byte is known so
   that a <CR><LF> pair collapses to <LF> even when it straddles two writes.
*/
#ifdef _WIN32
#define TEXT_CONSOLE 0
#define textOut(n)   0
#else
#define TEXT_CONSOLE 1
static int textOut(const char *name);
#endif

static struct {
    int fd;
    int mode;
    byte text;                  // <CR><LF> -> <LF> translation needed
    byte pendCR;                // a <CR> is waiting for the byte after it
} aft[AFTSIZE] = { { 1 , WRITE_MODE, TEXT_CONSOLE, 0 }, {0, READ_MODE, 0, 0}, {0, UPDATE_MODE, 0, 0} };      // CO, CI, BB

/* Directory that plain Unix paths are looked up in when the file name itself
   carries no directory (see above). */
static bool srcDirSet = false;
static char srcDir[ISIS_PATH_LEN];
static char srcBase[NAME_SLOT + 4];    /* the source file name itself, to tell the
                               source open apart from an $INCLUDE of a file
                               with the same name */

static char _commandLine[MAXLL + 1];
static char *_commandLinePtr;
word ovl = 255;   // current ovl

int errorLevel;

/* append a component to a path, inserting a '/' only where one is needed */
static int addPart(char *dst, size_t size, const char *part)
{
    size_t n;

    if (part == NULL || *part == 0)
        return 0;
    n = strlen(dst);
    if (n != 0 && dst[n - 1] != '/' && dst[n - 1] != '\\') {
        if (n + 1 >= size)
            return -1;
        dst[n++] = '/';
        dst[n] = 0;
    }
    if (strlen(part) >= size - n)
        return -1;
    strcpy(dst + n, part);
    return 0;
}




static word MapFile(osfile_t *osfileP, const char *path)
{
    /* Plain Unix paths: case preserved, no devices, no 8.3 splitting.
       Table and control names arrive space padded (fixed slots); only the
       padding is stripped.  The sole :XX: form still recognised is the :CI:
       console that feeds the command tail. */
    char clean[NAME_SLOT + 4];
    char base[NAME_SLOT + 4];
    char dir[ISIS_PATH_LEN];
    const char *slash;
    const char *back;
    size_t len;

    len = strlen(path);
    while (len > 0 && path[len - 1] == ' ')
        len--;
    if (len >= sizeof clean)
        return ERROR_BADFILENAME;
    memcpy(clean, path, len);
    clean[len] = 0;

    if (clean[0] == ':') {
        /* the only devices still known: the two consoles.  :CI: feeds the
           command tail, :CO: is standard output (the default listing). */
        int isC = (clean[1] == 'C' || clean[1] == 'c');
        int isI = (clean[2] == 'I' || clean[2] == 'i');
        int isO = (clean[2] == 'O' || clean[2] == 'o');

        if (clean[4] == 0 && clean[3] == ':' && isC && isI) {
            osfileP->deviceId = 23; /* CI, see Open() */
            osfileP->modes = READ_MODE;
            osfileP->altName[0] = 0;
            return ERROR_SUCCESS;
        }
        if (clean[4] == 0 && clean[3] == ':' && isC && isO) {
            osfileP->deviceId = 24; /* CO, see Open() */
            osfileP->modes = WRITE_MODE + RANDOM_ACCESS;
            osfileP->altName[0] = 0;
            return ERROR_SUCCESS;
        }
        return ERROR_BADDEVICE;
    }

    osfileP->deviceId = 0;
    osfileP->modes = UPDATE_MODE + RANDOM_ACCESS;

    slash = strrchr(clean, '/');
    back = strrchr(clean, '\\');
    if (back != NULL && (slash == NULL || back > slash))
        slash = back;
    if (slash != NULL) {
        if (slash == clean)             /* /prog.plm - the root directory */
            strcpy(dir, "/");
        else {
            len = (size_t)(slash - clean);
            if (len >= sizeof dir)
                return ERROR_BADFILENAME;
            memcpy(dir, clean, len);
            dir[len] = 0;
        }
        snprintf(base, sizeof base, "%s", slash + 1);
    } else {
        dir[0] = 0;
        snprintf(base, sizeof base, "%s", clean);
    }
    if (base[0] == 0)
        return ERROR_BADFILENAME;

    /* Directory of the name itself, or - when the name carries none - the
       directory the source file came from, so that the output files land
       next to the source. */
    {
        char useDir[ISIS_PATH_LEN];

        if (dir[0] == '/')
            snprintf(useDir, sizeof useDir, "%s", dir);
        else if (dir[0] == 0 && srcDirSet)
            snprintf(useDir, sizeof useDir, "%s", srcDir);
        else
            snprintf(useDir, sizeof useDir, "%s", dir);
        osfileP->name[0] = 0;
        if (addPart((char *)osfileP->name, sizeof osfileP->name, useDir) != 0 ||
            addPart((char *)osfileP->name, sizeof osfileP->name, base) != 0)
            return ERROR_BADFILENAME;
    }

    /* Record the other place this file may be found when reading.  A name
       with a directory is looked up in the current directory first, so the
       source directory is the alternative; a name without one was looked
       up in the source directory first (except for the source file itself,
       which is named explicitly), so the current directory is the
       alternative.  Writes always use the primary name. */
    osfileP->altName[0] = 0;
    if (srcDirSet && srcDir[0] != 0 && dir[0] != '/' &&
            !(dir[0] == 0 && srcBase[0] != 0 &&
              strcmp(base, srcBase) == 0)) {
        char altDir[ISIS_PATH_LEN];

        altDir[0] = 0;
        if (dir[0] != 0 &&
            (addPart(altDir, sizeof altDir, srcDir) != 0 ||
             addPart(altDir, sizeof altDir, dir) != 0))
            altDir[0] = 0;
        if (dir[0] == 0 || altDir[0] != 0) {
            char alt[_MAX_PATH];

            alt[0] = 0;
            if (addPart(alt, sizeof alt, altDir) != 0 ||
                addPart(alt, sizeof alt, base) != 0)
                alt[0] = 0;
            if (alt[0] != 0 && strcmp(alt, (char *)osfileP->name) != 0)
                snprintf((char *)osfileP->altName, sizeof osfileP->altName,
                         "%s", alt);
        }
    }
    return ERROR_SUCCESS;
}

/* Remember the directory the source file was found in, so that the object,
   listing and index files are created next to it */
void SetSrcDir(const char *dir, const char *name)
{
    srcDirSet = true;
    snprintf(srcDir, sizeof srcDir, "%s", dir);
    snprintf(srcBase, sizeof srcBase, "%s", name);
}

#ifndef _WIN32
/* the listing is the only file the compiler writes as text */
static int textOut(const char *name)
{
    const char *p = strrchr(name, '.');

    return p != NULL && strcmp(p, ".lst") == 0;
}
#endif



/* Parse an org() address: Intel hex with H suffix (0100h), 0x hex, decimal.
   Returns true and stores 0..0xFFFF in *out on success. */
bool parseOrgAddr(const char *s, word *out)
{
    size_t n = strlen(s);
    unsigned long v;
    char *end;
    char buf[16];
    size_t i;
    if (n == 0 || n >= sizeof buf)
        return false;
    for (i = 0; i <= n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        buf[i] = c;
    }
    if (buf[n - 1] == 'H') {
        /* Intel hex demands a leading digit (as does z80asm) */
        if (buf[0] < '0' || buf[0] > '9')
            return false;
        buf[n - 1] = 0;
        v = strtoul(buf, &end, 16);
        if (*end != 0)
            return false;
    } else if (buf[0] == '0' && (buf[1] == 'X' || buf[1] == 'x')) {
        v = strtoul(buf + 2, &end, 16);
        if (*end != 0 || end == buf + 2)
            return false;
    } else {
        v = strtoul(buf, &end, 10);
        if (*end != 0)
            return false;
    }
    if (v > 0xFFFF)
        return false;
    *out = (word)v;
    return true;
}

int main(int argc, char **argv)
{
    int i;
    char run;
    size_t len;
    char *s, *progname;

    if (argc == 1)
    {
        showVersion(stdout, 0);
        exit(0);
    }
    if (argc == 2 && strcmp(argv[1], "-v") == 0)
    {
        showVersion(stdout, argv[1][1] == 'V');
        exit(0);
    }
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), O_BINARY);
    (void)_setmode(_fileno(stdout), O_BINARY);
#endif
    /* find program name */
    for (progname = argv[0]; (s = strpbrk(progname, ":/\\")); progname = s + 1)
        ;
    /* only allow alpha numeric char names otherwise it may confuse isis program */
    /* .exe is also excluded*/
    for (s = _commandLine, i = 0; isalnum(*progname) && i < 6; i++)
        *s++ = *progname++;
    *s = 0;

    len = strlen(_commandLine);

    for (i = 1; i < argc && len + strlen(argv[i]) + 1 < MAXLL - 2; i++) {       // add args if room
            len += strlen(argv[i]) + 1;
            strcat(_commandLine, " ");
            strcat(_commandLine, argv[i]);
        }
    if (i < argc)
            fprintf(stderr, "command line truncated\n");

    _commandLinePtr = strcat(_commandLine, "\r\n");

    initMem();
    ovl = 255;
    #ifdef _DEBUG
        symMode = 1;        // standard sym_t
        infoMode = 0;
    #endif
    errorLevel = 0;
    run = 1;
    while (run)
    {
        switch (ovl)
        {
            case 255:
                ovl = Start();
                break;
            case 0:
                ovl = Start0();
                #ifdef _DEBUG
                    DumpLexStream();
                #endif
                break;
            case 1:
                ovl = Start1();
                break;
            case 2:
                ovl = Start2();
                break;
            case 3:
                ovl = Start3();
                break;
            case 4:
                ovl = Start4();
                break;
            case 5:
                ovl = Start5();
                break;
            case 6:
                ovl = Start6();
                break;
            case 254:
                EndCompile();
                run = 0;
                break;

            default: fprintf(stderr, "unexpected overlay %d\n", ovl);
                errorLevel = 1;
                run = 0;
                break;
        }
        // dumpAllInfo();
    }
    return errorLevel;
}


void FatalError(byte errNum)
{
    switch (ovl) {
    case 255: FatalError_main(errNum); break;
    case 0:   FatalError_ov0(errNum); break;
    case 1:   FatalError_ov1(errNum); break;
    case 2:
    case 3:
    case 5:
    case 4:
    case 6:   FatalError_ov46(errNum); break;
    }
}



word ChkMode(word conn, word mode)
{
    if (conn >= AFTSIZE)
        return ERROR_BADPARAM;
    if (aft[conn].mode == 0)
        return ERROR_NOTOPEN;
    if (mode != 0 && (mode & aft[conn].mode) == 0) {
        if (mode == READ_MODE)
            return ERROR_NOREAD;
        if (mode == WRITE_MODE)
            return ERROR_NOWRITE;
        if (mode == RANDOM_ACCESS)
            return ERROR_CANTSEEKDEV;
    }
    return ERROR_SUCCESS;
}


void Close(word conn, wpointer statusP)
{
    if ((*statusP = ChkMode(conn, 0)) != ERROR_SUCCESS)
        return;

    if (conn <= BB_DEV)                                 // ingore requests to close CO, CI or BB
        return;

    if (aft[conn].pendCR) {             // flush the <CR> of a trailing <CR><LF>
        (void)!write(aft[conn].fd, "\r", 1);
        aft[conn].pendCR = 0;
    }
    if (close(aft[conn].fd) < 0)
        *statusP = ERROR_NOTOPEN;
    aft[conn].mode = 0;
    aft[conn].text = 0;
}


void Delete(pointer pathP, wpointer statusP)
{
    osfile_t osfile;

    if ((*statusP = MapFile(&osfile, (const char *)pathP)) == ERROR_SUCCESS) {
        if (!(osfile.modes & RANDOM_ACCESS))
            *statusP = ERROR_ISDEVICE;
        else if (unlink((const char *)osfile.name) < 0) {
            if (errno == EACCES)
                *statusP = ERROR_PERMISSIONS;
            else if (errno == ENOENT)
                *statusP = ERROR_FILENOTFOUND;
            else
                *statusP = ERROR_NOTREADY;
        }
    }
}

static const char *error_strings[] =
{
    "Success",
    "No memory available for buffer",
    "File is not open",
    "No more file handles available",
    "Invalid pathname",
    "Bad device name in filename",
    "Trying to write a file opened in read mode",
    "Disk is full",
    "Trying to read a file opened in write mode",
    "Cannot create file",
    "Cannot rename across devices",
    "Destination file exists",
    "File is already open",
    "File not found",
    "Permissions error",
    "Attempting to overwrite operating system",
    "Invalid executable image",
    "Attempt to rename or delete a device",
    "Invalid function number",
    "Can't seek on a device",
    "Can't seek to before start of file",
    "File is not open in line mode",
    "Bad access mode",
    "No filename specified",
    "Disk error",
    "Invalid echo file specified",
    "Bad attribute",
    "Bad seek mode",
    "Null file extension",
    "End of file on console input",
    "Drive not ready",
    "Can't seek in a write-only file",
    "Can't delete an open file",
    "Bad system call parameter",
    "Bad switch argument in load",
    "Cannot seek past EOF in file open for read",
};

#define MAXERROR (sizeof(error_strings) / sizeof(error_strings[0]))


void Error(word ErrorNum)
{
//    if (ErrorNum >= 0 && ErrorNum < MAXERROR)
    if (ErrorNum < MAXERROR)
        fprintf(stderr, "%s.\n", error_strings[ErrorNum]);
    else
        fprintf(stderr, "Unknown error %d.\n", ErrorNum);
}

NORETURN(Exit())
{
#ifdef _DEBUG
    (void)getchar();
#endif
    exit(1);
}

#ifndef _WIN32

/* Open a file for reading.  The .plm extension may be left off the
   command line (source files are normally .plm); anything else is opened
   exactly as written. */
static int openRead(const char *name, int mode, int perm)
{
    char dir[_MAX_PATH], base[_MAX_PATH], cand[_MAX_PATH + 8], path[_MAX_PATH];
    const char *slash;
    size_t len;
    int fd;
    int i;

    if ((fd = open(name, mode, perm)) >= 0 || errno != ENOENT)
        return fd;

    if ((slash = strrchr(name, '/')) != NULL) {
        len = (size_t)(slash - name);
        if (len >= sizeof dir)
            return -1;
        memcpy(dir, name, len);
        dir[len] = 0;
        snprintf(base, sizeof base, "%s", slash + 1);
    } else {
        dir[0] = 0;
        snprintf(base, sizeof base, "%s", name);
    }

    for (i = 0; i < 2; i++) {
        if (i == 0)
            snprintf(cand, sizeof cand, "%s", base);
        else if (strchr(base, '.') == NULL)
            snprintf(cand, sizeof cand, "%s.plm", base);   /* source files are normally .plm */
        else
            break;

        path[0] = 0;
        if (addPart(path, sizeof path, dir) != 0 || addPart(path, sizeof path, cand) != 0)
            return -1;
        if ((fd = open(path, mode, perm)) >= 0)
            return fd;
        if (errno != ENOENT && errno != ENOTDIR)
            return -1;
    }
    errno = ENOENT;
    return -1;
}
#endif


void Open(wpointer connP, pointer pathP, word access, word echo, wpointer statusP)
{
    int mode, conn;
    osfile_t osfile;
    int handle;

    *connP = -1;                // unused conn
    if ((*statusP = MapFile(&osfile, (const char *)pathP)) != ERROR_SUCCESS)
        return;

    switch (access) {
    case READ_MODE: mode = O_RDONLY | O_BINARY; break;
    case WRITE_MODE: mode = O_WRONLY | O_CREAT | O_TRUNC | O_BINARY; break;
    case UPDATE_MODE: mode = O_RDWR | O_CREAT | O_TRUNC | O_BINARY; break;
    default: fprintf(stderr, "bad access mode %d for %s\n", access, osfile.name);
        *statusP = ERROR_BADPARAM;
        return;
    }

    if ((osfile.modes & access) != access) {
        *statusP = ERROR_BADACCESS;
        return;
    }

    if (osfile.deviceId == 23) { // CI, the console behind the command tail
        *connP = CI_DEV;
        return;
    }
    else if (osfile.deviceId == 24) { // CO, standard output (listing)
        aft[CO_DEV].mode = access;
        *connP = CO_DEV;
        return;
    }

    // allocate a handle
    for (handle = 2; handle < AFTSIZE; handle++)
        if (aft[handle ].mode == 0)
            break;

    if (handle >= AFTSIZE) {
        *statusP = ERROR_NOHANDLES;
        return;
    }

    /* the umask decides the permissions of a new file */
#ifdef _WIN32
    conn = open(osfile.name, mode, S_IREAD | S_IWRITE);
    if (conn < 0 && mode == O_RDONLY && osfile.altName[0] != 0 &&
        (errno == ENOENT || errno == ENOTDIR))
        conn = open((char *)osfile.altName, mode, S_IREAD | S_IWRITE);
#else
    conn = mode == O_RDONLY ? openRead((char *)osfile.name, mode, 0666)
                            : open((char *)osfile.name, mode, 0666);
    /* reading: when the file is not where the name says first, it may be
       in the other of the two directories recorded by MapFile */
    if (conn < 0 && mode == O_RDONLY && osfile.altName[0] != 0 &&
        (errno == ENOENT || errno == ENOTDIR))
        conn = openRead((char *)osfile.altName, mode, 0666);
#endif

    if (conn < 0)
        switch (errno) {
        case EACCES: *statusP = ERROR_PERMISSIONS; break;
        case EEXIST: *statusP = ERROR_NOWRITE; break;
        case EMFILE: *statusP = ERROR_NOHANDLES; break;
        case ENOENT: *statusP = ERROR_FILENOTFOUND; break;
        default: fprintf(stderr, "unknown error %d for open %s", errno, osfile.name);
            *statusP = ERROR_NOTREADY;
        }
    else {
        aft[handle].fd = conn;
        aft[handle].mode = access + (osfile.modes & RANDOM_ACCESS);
        aft[handle].text = textOut((const char *)osfile.name);
        aft[handle].pendCR = 0;
        *connP = handle;
    }
}

char *ReadLine(char *buf)
{
    int c;
    int i;
    bool trunc = false;

    i = 0;
    while ((c = getchar()) != EOF && c != '\n' && c != '\r') {
        if (i < MAXLL - 2)
            buf[i++] = c;
        else
            trunc = true;
    }
    if (c == '\r' && (c = getchar()) != '\n')           // pick up \r\n else singleton \r
        ungetc(c, stdin);
    strcpy(buf + i, "\r\n");
    if (trunc)
        fprintf(stderr, ":CI: line truncated\n");
    return buf;
}


void Read(word conn, pointer buffP, word count, wpointer actualP, wpointer statusP)
{
    int actual;

    if ((*statusP = ChkMode(conn, READ_MODE)) != ERROR_SUCCESS)
        return;

    if (conn == CI_DEV) {
        if (!*_commandLinePtr)
            _commandLinePtr = ReadLine(_commandLine);

        for (actual = 0; actual < count && *_commandLinePtr; actual++)
            *buffP++ = *_commandLinePtr++;
        *actualP = actual;
        return;
    }
    else if (conn == BB_DEV) {
        *actualP = 0;
        return;
    }
    do {
        actual = read(aft[conn].fd, buffP, count);
    } while (actual < 0 && errno == EINTR);

    if (actual >= 0) {
        *actualP = actual;
        *statusP = 0;
    }
    else {
        *actualP = 0;
        if (errno == EBADF)
            *statusP = ERROR_NOTOPEN;
        else if (errno == EINVAL)
            *statusP = ERROR_BADPARAM;
        else
            *statusP = ERROR_NOTREADY;
    }
}
void Rescan(word conn, wpointer statusP)
{
    if (conn == CI_DEV) {
        _commandLinePtr = _commandLine;
        *statusP = ERROR_SUCCESS;
    }
    else
        *statusP = ERROR_NOTLINEMODE;
}



void Seek(word conn, word mode, wpointer blockP, wpointer byteP, wpointer statusP)
{
    long offset;
    int origin;

    if (conn <= BB_DEV) {   /* consoles and bit bucket: no-op, always succeeds */
        if (mode == SEEKTELL) {
            *blockP = 0;
            *byteP = 0;
        }
        *statusP = 0;
        return;
    }

    if ((*statusP = ChkMode(conn, RANDOM_ACCESS)) != ERROR_SUCCESS)
        return;

    if (aft[conn].mode == RANDOM_ACCESS + WRITE_MODE) {
        *statusP = ERROR_SEEKWRITE;
        return;
    }

    if (mode != SEEKTELL)
        offset = *blockP * 128 + *byteP;

    switch (mode) {
    case SEEKTELL:
        offset = lseek(aft[conn].fd, 0L, SEEK_CUR);
        *blockP = (word) (offset / 128);
        *byteP = offset % 128;
        *statusP = 0;
        return;
    case SEEKABS:       origin = SEEK_SET; break;
    case SEEKBACK:      offset = -offset;
    case SEEKFWD:       origin = SEEK_CUR; break;
    case SEEKEND:       origin = SEEK_END; offset = 0;  break;
    default: fprintf(stderr, "Unsupported seek mode %d\n", mode);
        *statusP = ERROR_BADMODE;
        return;
    }
    if (lseek(aft[conn].fd, offset, origin) >= 0)
        *statusP = 0;
    else
        *statusP = ERROR_BADPARAM;
}
/* write() may transfer fewer bytes than asked for and may be interrupted */
static int writeAll(int fd, const void *buf, size_t len)
{
    int n;

    while (len != 0) {
        if ((n = (int)write(fd, buf, len)) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        buf = (const char *)buf + n;
        len -= (size_t)n;
    }
    return 0;
}

static void wrStatus(int err, wpointer statusP)
{
    switch (err) {
    case ENOSPC: *statusP = ERROR_DISKFULL; break;
    case EINVAL: *statusP = ERROR_BADPARAM; break;
    default: *statusP = ERROR_NOTREADY; break;
    }
}

/* add a byte to a full buffer, emptying the buffer when it is full */
static int putByte(int fd, byte *buf, size_t *n, size_t size, byte c)
{
    buf[(*n)++] = c;
    if (*n == size) {
        if (writeAll(fd, buf, *n) != 0)
            return -1;
        *n = 0;
    }
    return 0;
}

void Write(word conn, pointer buffP, word count, wpointer statusP)
{
    byte buf[512];
    size_t n = 0;
    size_t i;
    byte c;

    if ((*statusP = ChkMode(conn, WRITE_MODE)) != ERROR_SUCCESS)
        return;

    if (conn == BB_DEV)
        return;

    if (!aft[conn].text) {
        if (writeAll(aft[conn].fd, buffP, count) != 0)
            wrStatus(errno, statusP);
        return;
    }

    /* text output: the listing is written with <LF> line endings */
    for (i = 0; i < count; i++) {
        c = buffP[i];
        if (aft[conn].pendCR) {
            aft[conn].pendCR = 0;
            /* a held back <CR> is dropped when a <LF> follows it */
            if (c != '\n' && putByte(aft[conn].fd, buf, &n, sizeof buf, (byte)'\r') != 0) {
                wrStatus(errno, statusP);
                return;
            }
        }
        if (c == '\r') {
            aft[conn].pendCR = 1;               /* held back, it may be a <CR><LF> */
            continue;
        }
        if (putByte(aft[conn].fd, buf, &n, sizeof buf, c) != 0) {
            wrStatus(errno, statusP);
            return;
        }
    }
    if (n != 0 && writeAll(aft[conn].fd, buf, n) != 0)
        wrStatus(errno, statusP);
}

void Rename(pointer oldP, pointer newP, wpointer statusP)
{
    osfile_t oldFile, newFile;

    if ((*statusP = MapFile(&oldFile, (const char *)oldP)) != ERROR_SUCCESS || (*statusP = MapFile(&newFile, (const char *)newP)) != ERROR_SUCCESS)
        return;
    if (oldFile.deviceId != newFile.deviceId)
        *statusP = ERROR_RENACROSS;
    else if ((oldFile.modes & RANDOM_ACCESS) == 0)
        *statusP = ERROR_ISDEVICE;
    else if (rename((const char *)oldFile.name, (const char *)newFile.name) != 0)
        switch (errno) {
        case EACCES: *statusP = ERROR_EXISTS; break;
        case ENOENT: *statusP = ERROR_FILENOTFOUND; break;
        case EINVAL: *statusP = ERROR_BADFILENAME; break;
        default: *statusP = ERROR_BADPARAM; break;
        }
}


FILE *Fopen(char *isisPath, char *mode)
{
    osfile_t fname;
    if (MapFile(&fname, isisPath) != ERROR_SUCCESS)
        return NULL;
    return fopen((const char *)fname.name, mode);
}
