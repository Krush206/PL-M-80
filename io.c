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
#include <strings.h>               /* strcasecmp() */
#define _MAX_PATH 4096
#define O_BINARY    0
#endif


#include "plm.h"

#define RANDOM_ACCESS   4       // additional access mode

typedef struct {
    byte        deviceId; // isis device Id
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

/* Directory that files on an ISIS device are looked up in when the file name
   itself carries no directory.  Set from the source file name, so that the
   object, listing and index files are created next to the source. */
static int  srcDirDev = -1;
static char srcDir[ISIS_PATH_LEN];
static char srcBase[16];    /* the source file name itself, to tell the
                               source open apart from an $INCLUDE of a file
                               with the same name */

static char _commandLine[MAXLL + 1];
static char *_commandLinePtr;
word ovl = 255;   // current ovl

int errorLevel;

struct {
    char *dev;
    unsigned char devtype;
} deviceMap[] = {
    { "F0", 3 },{ "F1", 3 },{ "F2", 3 },{ "F3", 3 },    // 0 - 3
    { "F4", 3 },{ "F5", 3 },{ "TI", 0 },{ "TO", 1 },    // 4 - 7
    { "VI", 0 },{ "VO", 1 },{ "I1", 0 },{ "O1", 1 },    // 8 - 11
    { "TR", 0 },{ "HR", 0 },{ "R1", 0 },{ "R2", 0 },    // 12 - 15
    { "TP", 1 },{ "HP", 1 },{ "P1", 1 },{ "P2", 1 },    // 16 - 19
    { "LP", 1 },{ "L1", 1 },{ "BB", 2 },{ "CI", 0 },    // 20 - 23
    { "CO", 1 },{ "F6", 3 },{ "F7", 3 },{ "F8", 3 },    // 24 - 27
    { "F9", 3 } };                                                                              // 28

const int nDevices = (sizeof(deviceMap) / sizeof(deviceMap[0]));

/* characters allowed in a file name or extension.  '_' and '-' are not part
   of the ISIS charset, but are common in Unix file names, and ' ' is allowed
   in a quoted name (trailing padding spaces are removed before parsing) */
static bool isNameChar(int ch)
{
    return isalnum((unsigned char)ch) != 0 || ch == '_' || ch == '-' || ch == ' ';
}

/* preps the deviceId and filename of an spath info record
   returns standard error codes as appropriate
*/
static word ParseIsisName(spath_t *pInfo, const char *isisPath)
{
    int i;
    char dev[3];
    const char *base;
    const char *end;        /* end of the name, after any padding */
    const char *slash = NULL;
    size_t len;

    /* names come space padded from the file tables - drop that, but keep
       spaces that are part of the name itself */
    len = strlen(isisPath);
    while (len > 0 && isisPath[len - 1] == ' ')
        len--;
    end = isisPath + len;

    strcpy(dev,"F0");   // default device is F0

    memset(pInfo, 0, sizeof(spath_t));
    pInfo->deviceId = 0xff;             // prefill incase of error

    if (isisPath[0] == ':') {   // check for :XX:
        if (end - isisPath < 4 || isisPath[3] != ':')
            return ERROR_BADFILENAME;
        dev[0] = toupper((unsigned char)isisPath[1]);
        dev[1] = toupper((unsigned char)isisPath[2]);
        isisPath += 4;
    }

    for (i = 0; i < nDevices; i++) // look up device
        if (strcmp(dev, deviceMap[i].dev) == 0)
            break;
    if (i >= nDevices)
        return ERROR_BADDEVICE;
    pInfo->deviceId = i;
    pInfo->deviceType = deviceMap[i].devtype;

    base = isisPath;
    if (pInfo->deviceType == 3) { // parse file name if file device
        /* Unix: a name may carry a directory part, e.g. src/prog.plm */
        for (i = 0; base + i < end; i++)
            if (base[i] == '/' || base[i] == '\\')
                slash = base + i;
        if (slash != NULL) {
            if (slash == base)          /* /prog.plm - the root directory */
                strcpy((char *)pInfo->dir, "/");
            else {
                len = (size_t)(slash - base);
                if (len >= sizeof(pInfo->dir))
                    return ERROR_BADFILENAME;
                memcpy(pInfo->dir, base, len);
                pInfo->dir[len] = 0;
            }
            base = slash + 1;
        }

        for (i = 0; i < 8 && base < end && isNameChar(*base); i++)
            pInfo->name[i] = toupper((unsigned char)*base++);
        if (i == 0)
            return ERROR_BADFILENAME;
        if (base < end && *base == '.') {
            base++;
            for (i = 0; i < 3 && base < end && isNameChar(*base); i++)
                pInfo->ext[i] = toupper((unsigned char)*base++);
            if (i == 0)
                return ERROR_BADEXT;
        }
    }
    return base < end && isNameChar(*base) ? ERROR_BADFILENAME : ERROR_SUCCESS;
}

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

#ifndef _WIN32
static int scanDir(const char *dir, const char *base, char *out, size_t size);

/* Look a path up as it exists on a case sensitive file system: every
   component that is not there as written is matched ignoring case, so that
   SRC/PROG.PLM finds src/prog.plm.  The path is left alone when nothing
   matches, as it may be a file that is about to be created. */
static void resolvePath(char *path, size_t size)
{
    char comp[_MAX_PATH];
    char cand[_MAX_PATH];
    char resolved[_MAX_PATH];
    const char *p;
    const char *slash;
    size_t n;

    if (*path == 0 || access(path, F_OK) == 0)
        return;

    p = path;
    if (*p == '/')
        strcpy(resolved, "/");
    else
        resolved[0] = 0;

    while (*p == '/')
        p++;

    while (*p != 0) {
        slash = strchr(p, '/');
        n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        if (n >= sizeof comp)
            return;
        memcpy(comp, p, n);
        comp[n] = 0;

        snprintf(cand, sizeof cand, "%s", resolved);
        if (addPart(cand, sizeof cand, comp) != 0)
            return;
        if (access(cand, F_OK) != 0)
            scanDir(resolved, comp, cand, sizeof cand);    /* cand gets the name from disk */
        snprintf(resolved, sizeof resolved, "%s", cand);

        if (slash == NULL)
            break;
        for (p = slash + 1; *p == '/'; p++)        /* skip repeated separators */
            ;
    }
    if (strcmp(path, resolved) != 0 && strlen(resolved) < size)
        strcpy(path, resolved);
}
#endif


/*
    map an isis file into a real file
    the realFile contains is prefixed with two bytes
    deviceId and deviceType
    return isis error status
 */

static word MapFile(osfile_t *osfileP, const char *isisPath)
{
    spath_t info;
    const char *src;
    char buf[8];
    char base[16];
    char dir[ISIS_PATH_LEN];
    char *s;
    int status;
    int i;
    static byte modes[] = { READ_MODE, WRITE_MODE, UPDATE_MODE, UPDATE_MODE + RANDOM_ACCESS };

    if ((status = ParseIsisName(&info, isisPath)) != ERROR_SUCCESS) // get canocial name
        return status;

    osfileP->deviceId = info.deviceId;
    osfileP->modes = modes[info.deviceType];

    if (22 <= info.deviceId && info.deviceId <= 24) // BB, CI, CO
        return ERROR_SUCCESS;

    /* see if user has provided a device map */
    snprintf(buf, sizeof buf, ":%s:", deviceMap[info.deviceId].dev);    // look for any mapping provided
    src = getenv(buf);

    if (info.deviceType != 3) {     /* not a file device - the mapping is the device itself */
        if (src == NULL || *src == 0) {
            fprintf(stderr, "No mapping for :%s:\n", deviceMap[info.deviceId].dev);
            return ERROR_NOTREADY;
        }
        snprintf((char *)osfileP->name, sizeof osfileP->name, "%s", src);
        return ERROR_SUCCESS;
    }

    /* file devices: names are lower cased, as Unix file systems are
       case sensitive and the compiler generates them itself */
    for (s = base, i = 0; i < 8 && info.name[i]; i++)
        *s++ = tolower(info.name[i]);
    if (info.ext[0]) {
        *s++ = '.';
        for (i = 0; i < 3 && info.ext[i]; i++)
            *s++ = tolower(info.ext[i]);
    }
    *s = 0;

    /* Directory of the name itself, or - when the name carries none - the
       directory the source file name was taken from, so that the output
       files land next to the source.  An absolute directory replaces the
       device mapping, as it does on Unix. */
    if ((char)info.dir[0] == '/') {
        snprintf(dir, sizeof dir, "%s", (char *)info.dir);
    } else {
        dir[0] = 0;
        if (info.dir[0] == 0 && info.deviceId == srcDirDev) {
            if (addPart(dir, sizeof dir, srcDir) != 0)
                return ERROR_BADFILENAME;
        } else if (addPart(dir, sizeof dir, (char *)info.dir) != 0) {
            return ERROR_BADFILENAME;
        }
    }

    osfileP->name[0] = 0;
    if (dir[0] != '/' && addPart((char *)osfileP->name, sizeof osfileP->name, src) != 0)
        return ERROR_BADFILENAME;
    if (addPart((char *)osfileP->name, sizeof osfileP->name, dir) != 0 ||
        addPart((char *)osfileP->name, sizeof osfileP->name, base) != 0)
        return ERROR_BADFILENAME;

    /* Unix: record the other place this file may be found when reading.
       A name with a directory is looked up in the current directory first,
       so the source directory is the alternative; a name without one was
       looked up in the source directory first (except for the source file
       itself, which is named explicitly), so the current directory is the
       alternative.  Writes always use the primary name. */
    osfileP->altName[0] = 0;
    if (info.deviceId == srcDirDev && srcDir[0] != 0 && (char)info.dir[0] != '/' &&
            !(info.dir[0] == 0 && srcBase[0] != 0 &&
              strcasecmp(base, srcBase) == 0)) {
        char altDir[ISIS_PATH_LEN];

        altDir[0] = 0;
        if (info.dir[0] != 0 &&
            (addPart(altDir, sizeof altDir, srcDir) != 0 ||
             addPart(altDir, sizeof altDir, (char *)info.dir) != 0))
            altDir[0] = 0;
        if (info.dir[0] == 0 || altDir[0] != 0) {
            char alt[_MAX_PATH];

            alt[0] = 0;
            if ((altDir[0] != '/' && addPart(alt, sizeof alt, src) != 0) ||
                addPart(alt, sizeof alt, altDir) != 0 ||
                addPart(alt, sizeof alt, base) != 0)
                alt[0] = 0;
            if (alt[0] != 0 && strcmp(alt, (char *)osfileP->name) != 0)
                snprintf((char *)osfileP->altName, sizeof osfileP->altName,
                         "%s", alt);
        }
    }

#ifndef _WIN32
    resolvePath((char *)osfileP->name, sizeof osfileP->name);
    if (osfileP->altName[0] != 0)
        resolvePath((char *)osfileP->altName, sizeof osfileP->altName);
#endif
    return ERROR_SUCCESS;
}

/* Unix: remember the directory the source file was found in, so that the
   object, listing and index files are created next to it */
void SetDevDir(const char *dev, const char *dir, const char *name)
{
    char d[3];
    int i;

    d[0] = toupper((unsigned char)dev[0]);
    d[1] = toupper((unsigned char)dev[1]);
    d[2] = 0;
    for (i = 0; i < nDevices; i++)
        if (strcmp(d, deviceMap[i].dev) == 0)
            break;
    if (i >= nDevices || deviceMap[i].devtype != 3)
        return;
    srcDirDev = i;
    snprintf(srcDir, sizeof srcDir, "%s", dir);
    snprintf(srcBase, sizeof srcBase, "%s", name);
}

#ifndef _WIN32
/* the listing is the only file the compiler writes as text */
static int textOut(const char *name)
{
    const char *p = strrchr(name, '.');

    return p != NULL && strcasecmp(p, ".lst") == 0;
}
#endif



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

void Load(pointer pathP, word LoadOffset, word swt, wpointer entryP, wpointer statusP)
{
    fprintf(stderr, "load not implmented\n");
    exit(1);
}


#ifndef _WIN32
/* look for 'base' in 'dir', ignoring case, and return the full name */
static int scanDir(const char *dir, const char *base, char *out, size_t size)
{
    DIR *dp;
    struct dirent *de;
    int found = 0;

    if ((dp = opendir(*dir != 0 ? dir : ".")) == NULL)
        return 0;
    while ((de = readdir(dp)) != NULL) {
        if (strcasecmp(de->d_name, base) != 0)
            continue;
        *out = 0;
        if (addPart(out, size, dir) == 0 && addPart(out, size, de->d_name) == 0)
            found = 1;
        break;
    }
    closedir(dp);
    return found;
}

/* Open a file for reading.  Two Unix habits are tolerated: the .plm
   extension may be left off the command line and the case of the name on
   disk may differ from the one given (file systems here are case
   sensitive). */
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
        if (scanDir(dir, cand, path, sizeof path)) {
            if ((fd = open(path, mode, perm)) >= 0)
                return fd;
            if (errno != ENOENT)
                return -1;
        }
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

    if (osfile.deviceId == 23) { // CI
        *connP = CI_DEV;
        return;
    }
    else if (osfile.deviceId == 24) { // CO
        *connP = CO_DEV;
        return;
    }
    else if (osfile.deviceId == 22) { // BB
        *connP = BB_DEV;
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


void Spath(pointer pathP, spath_t *infoP, wpointer statusP)
{
    if ((*statusP = ParseIsisName(infoP, (const char *)pathP)) != ERROR_SUCCESS)
        return;
    if (infoP->deviceType == 3)
        infoP->driveType = 4;
}


FILE *Fopen(char *isisPath, char *mode)
{
    osfile_t fname;
    if (MapFile(&fname, isisPath) != ERROR_SUCCESS)
        return NULL;
    return fopen((const char *)fname.name, mode);
}
