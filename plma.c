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


#include "plm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>


static byte signonMsg[] = "\r\nISIS-II PL/M-80 COMPILER ";
static byte noMemMsg[] = "NOT ENOUGH MEMORY FOR A COMPILATION";
static byte aIxi[] = ".IXI";
static byte aObj[] = ".OBJ";
static byte aAsm[] = ".ASM";
static byte aLst[] = ".LST";
static byte aInvocationComm[] = "INVOCATION COMMAND DOES NOT END WITH <CR><LF>";
static byte aIncorrectDevice[] = "INCORRECT DEVICE SPEC";
static byte aSourceFileNotDisk[] = "SOURCE FILE NOT A DISKETTE FILE";
static byte aSourceFileName[] = "SOURCE FILE NAME INCORRECT";
static byte aSourceFileBadExt[] = "SOURCE FILE EXTENSION INCORRECT";
static byte aIllegalCommand[] = "ILLEGAL COMMAND TAIL SYNTAX";

static byte  ioBuffer[MAXLL + 2];    // holds a whole command tail line
pointer cmdTextP;


static void SkipSpace()
{
    while (*cmdTextP == ' ' || *cmdTextP == '&') {
        if (*cmdTextP == ' ')
            cmdTextP++;
        else if (cmdLineP->link) {
            cmdLineP = cmdLineP->link;
            cmdTextP = &cmdLineP->pstr[1];
        }
    }
} /* SkipSpace() */

/*
  !!! ���㡠�� �� �������������

static bool TestToken(pointer str, byte len)
{
    pointer p;

    p = cmdTextP;
    while (len-- != 0) {
        if ((*cmdTextP++ & 0x5F) != *str++) {
            cmdTextP = p;
            return false;
        }
    }
    return true;
}
*/


static void SkipAlphaNum()
{
    while ('A' <= *cmdTextP && *cmdTextP <= 'Z' || 'a' <= *cmdTextP && *cmdTextP <= 'z'
                || '0' <= *cmdTextP && *cmdTextP <= '9' || *cmdTextP == '_')
        cmdTextP++;
} /* SkipAlphaNum() */

/* characters allowed in a file name: as SkipAlphaNum(), plus '-' and ' ',
   which are common in Unix file names (a quoted name may contain spaces) */
static bool IsFileChar(int ch)
{
    return ('A' <= ch && ch <= 'Z') || ('a' <= ch && ch <= 'z')
            || ('0' <= ch && ch <= '9') || ch == '_' || ch == '-' || ch == ' ';
}



static void GetCmdLine()
{
    word actual, status;
    byte i;
    bool inQuote;
    cmd_t *ptr;

    Rescan(1, &status);     // no need for LocalRescan
    if (status != 0)
        FatlIO(&conFile, status);
    startCmdLineP = NULL;
    cmdLineP = NULL;

    for (;;) {
        ReadF(&conFile, ioBuffer, MAXLL + 2, &actual);
        if (ioBuffer[actual - 1] != '\n' || ioBuffer[actual - 2] != '\r')
            Fatal(aInvocationComm, Length(aInvocationComm));

        ptr = malloc(actual + 2 + sizeof(cmd_t));
        if (startCmdLineP == NULL)
            startCmdLineP = ptr;
        else
            cmdLineP->link = ptr;

        cmdLineP = ptr;
        cmdLineP->pstr[0] = (byte)actual;
        memmove(&cmdLineP->pstr[1], ioBuffer, actual);

        inQuote = false;
        for (i = 0; i < actual; i++) {
            if (ioBuffer[i] == QUOTE)
                inQuote = ! inQuote;
            else if (ioBuffer[i] == '&')
                if (! inQuote)
                    goto extend;
        }
        cmdLineP->link = NULL;
        cmdLineP = startCmdLineP;
        return;
    extend:
        PrintStr("**", 2);
    }
} /* GetCmdLine() */


static void ParseInvokeName()
{
    //pointer startP;
    //word len;
    //word p;

    SkipSpace();
//    debugFlag = TestToken("DEBUG", 5);        // cannot occur under windows
//    SkipSpace();
    //startP = cmdTextP;
    if (*cmdTextP == ':')
        cmdTextP += 4;    // skip drive
    SkipAlphaNum();
    //if ((len = (word)(cmdTextP - startP)) > 10)
    //    len = 10;
    /* plm had the overlay & invokename (ov0) split out
     * I have combined them to avoid spaces in address ranges
     * note invokeName is now overlay[0]
     * overlayN is now overlay[N]
     * In the end this code is not actually needed as overlays are
     * handled differently
     */
    //for (p = 0; p <= 6; p++) {
    //    memmove(overlay[p], startP, len);               // copy fileName
    //    memmove(overlay[p] + len, overlay[p] + 10, 5);  // move up the ext
    //}
} /* ParseInvokeName() */


void isis2dos(char *src, char *dst)
{
    char *s, *d;
    int i;

    s = src;
    d = dst;
    if (*s == ':')
    {
        *d++ = *s++;
        *d++ = *s++;
        *d++ = *s++;
        *d++ = *s++;
    }
    for (i = 0; i < 8; i++)
    {
        if (*s == '.')
            break;
        if (*s < ' ')
        {
            *d = 0;
            return;
        }
        *d++ = *s++;
    }
    for (i = 0; i < 4; i++)
    {
        if (*s <= ' ')
            break;
        *d++ = *s++;
    }
    *d = 0;
}



static void ParseSrcFileName()
{
    pointer fullName;       /* the file name as it appears in the command tail */
    pointer fileName;       /* its 8.3 part, without any directory */
    pointer p;              /* scanning pointer */
    pointer end;            /* end of the file name, start of the controls */
    pointer nameEnd;        /* end of the file name itself */
    pointer slash = NULL;   /* last '/' of the file name, if any */
    pointer baseName;       /* the name itself, after any directory */
    word nameLen;
    char fname[MAXLL + 4];
    char dev[4];
    char dir[ISIS_PATH_LEN];
    byte devLen = 0;
    bool haveDir = false;

    SkipSpace();
    fullName = cmdTextP;

    /* The file name ends at the first space - the controls follow it.  It
       may be quoted, in which case it ends at the matching quote and may
       contain spaces, e.g. 'sub dir/prog.plm'. */
    if (*fullName == '\'' || *fullName == '"') {
        char quote = *fullName;

        fullName++;
        for (end = fullName; *end != quote && *end != '\r' && *end != '&'; end++)
            if (*end == '/' || *end == '\\')
                slash = end;
        if (*end != quote)
            Fatal(aSourceFileName, Length(aSourceFileName));
        nameEnd = end;
        if (*++end != ' ' && *end != '\r' && *end != '&')    /* junk after the quote */
            Fatal(aSourceFileName, Length(aSourceFileName));
    } else {
        for (end = fullName; *end != ' ' && *end != '\r' && *end != '&'; end++)
            if (*end == '/' || *end == '\\')
                slash = end;
        nameEnd = end;
    }

    // �뢮��� ��� ��室���� 䠩��
    nameLen = (word)(nameEnd - fullName);
    memcpy(fname, fullName, nameLen);
    fname[nameLen] = 0;
    strcat(fname, "\n");
    PrintStr(fname, strlen(fname));

    dev[0] = 'F';
    dev[1] = '0';
    dev[2] = 0;
    dir[0] = 0;
    fileName = fullName;
    if (*fileName == ':') {
        if (nameEnd - fileName < 4 || fileName[3] != ':')
            Fatal(aIncorrectDevice, Length(aIncorrectDevice));
        if (toupper((unsigned char)fileName[1]) != 'F')
            Fatal(aSourceFileNotDisk, Length(aSourceFileNotDisk));
        dev[0] = toupper((unsigned char)fileName[1]);
        dev[1] = toupper((unsigned char)fileName[2]);
        devLen = 4;
        fileName += 4;
    }

    /* Unix: a directory in front of the name is where the source file is
       read from and where the output files are written to */
    if (slash != NULL) {
        p = fullName + devLen;
        if (slash == p)            /* /prog.plm - the root directory */
            strcpy(dir, "/");
        else {
            nameLen = (word)(slash - p);
            if (nameLen >= sizeof dir)
                Fatal(aSourceFileName, Length(aSourceFileName));
            memcpy(dir, p, nameLen);
            dir[nameLen] = 0;
        }
        fileName = slash + 1;
        haveDir = true;
    }

    for (p = fileName; p < nameEnd && IsFileChar(*p); p++)
        ;
    if ((nameLen = (word)(p - fileName)) == 0 || nameLen > 8)
        Fatal(aSourceFileName, Length(aSourceFileName));
    srcStemLen = devLen + nameLen;
    memset(srcStemName, ' ', 12);
    memmove(srcStemName, fullName, devLen);         /* ":F1:" if given */
    memmove(srcStemName + devLen, fileName, nameLen);

    baseName = fileName;                            /* the name, without any directory */
    if (p < nameEnd && *p == '.') {
        p++;
        for (fileName = p; p < nameEnd && IsFileChar(*p); p++)
            ;
        if ((nameLen = (word)(p - fileName)) == 0 || nameLen > 3)
            Fatal(aSourceFileBadExt, Length(aSourceFileBadExt));
    }
    if (p != nameEnd)                               /* anything else is an error */
        Fatal(aSourceFileName, Length(aSourceFileName));

    if (haveDir) {
        char srcName[16];               /* bounded copy - the command tail
                                           ends at CR, not at a NUL */

        nameLen = (word)(p - baseName);
        memcpy(srcName, baseName, nameLen);
        srcName[nameLen] = 0;
        SetDevDir(dev, dir, srcName);
    }

    srcFileIdx = 0;
    memset(srcFileTable, ' ', 18);
    memmove(srcFileTable, fullName, devLen);
    memmove((byte *)srcFileTable + devLen, baseName, (size_t)(p - baseName));
    memset(&srcFileTable[8], 0, 4);
    cmdTextP = end;
    SkipSpace();
    if (*cmdTextP == '$')
        Fatal(aIllegalCommand, Length(aIllegalCommand));
    if (*cmdTextP == '\r')
        offFirstChM1 = 0;
    else
        offFirstChM1 = ((word)(cmdTextP - (pointer) cmdLineP)) - 1;
} /* ParseSrcFileName() */

static void InitFilesAndDefaults()
{
    LEFTMARGIN = 1;
    memset(ixiFileName, ' ', FILE_NAME_LEN);
    memmove(ixiFileName, srcStemName, srcStemLen);
    memmove(&ixiFileName[srcStemLen], aIxi, 4);
    InitF(&ixiFile, "IXREF ", ixiFileName);
    objBlk = objByte = 0;
    memset(objFileName, ' ', FILE_NAME_LEN);
    memmove(objFileName, srcStemName, srcStemLen);
    memmove(&objFileName[srcStemLen], aObj, 4);
    InitF(&objFile, "OBJECT", objFileName);
    memset(asmFileName, ' ', FILE_NAME_LEN);
    memmove(asmFileName, srcStemName, srcStemLen);
    memmove(&asmFileName[srcStemLen], aAsm, 4);
    InitF(&asmFile, "ASM   ", asmFileName);
    memset(lstFileName, ' ', FILE_NAME_LEN);
    memmove(lstFileName, srcStemName, srcStemLen);
    memmove(&lstFileName[srcStemLen], aLst, 4);
    InitF(&lstFil, "LIST ", lstFileName);
    InitF(&tx1File, "UT1 ", ":F1:PLMTX1.TMP ");
    InitF(&tx2File, "UT2 ", ":F1:PLMTX2.TMP ");
    InitF(&atFile, "AT  ", ":F1:PLMAT.TMP ");
    InitF(&nmsFile, "NAMES ", ":F1:PLMNMS.TMP ");
    InitF(&xrfFile, "XREF ", ":F1:PLMXRF.TMP ");
    IXREF = false;
    IXREFSet = false;
    PRINT = true;
    PRINTSet = true;
    XREF = false;
    SYMBOLS = false;
    DEBUG = false;
    PAGING = true;
    OBJECT = true;
    OBJECTSet = true;
    OPTIMIZE = true;
    SetDate(" ", 1);
    SetPageLen(57);
    SetMarkerInfo(20, '-', 21);
    SetPageNo(0);
    SetMarginAndTabW(0xFF, 4);
    SetTitle(" ", 1);
    SetPageWidth(120);
} /* InitFilesAndDefaults() */

void SignOnAndGetSourceName()
{
    memmove(szVersion, verNo, 4);
    /*
      clear files structures
    */
    memset(&srcFil , 0, sizeof(file_t));
    memset(&lstFil , 0, sizeof(file_t));
    memset(&objFile, 0, sizeof(file_t));
    memset(&asmFile, 0, sizeof(file_t));
    memset(&conFile, 0, sizeof(file_t));
    memset(&tx1File, 0, sizeof(file_t));
    memset(&tx2File, 0, sizeof(file_t));
    memset(&atFile , 0, sizeof(file_t));
    memset(&nmsFile, 0, sizeof(file_t));
    memset(&xrfFile, 0, sizeof(file_t));
    memset(&ixiFile, 0, sizeof(file_t));
    //
    InitF(&conFile, "CONSOL", ":CI: ");
    OpenF(&conFile, 1);
    topMem = MemCk() - 12;
    if (topMem < 0xC000)
        Fatal(noMemMsg, Length(noMemMsg));
    GetCmdLine();
    PrintStr(signonMsg, Length(signonMsg));
    PrintStr(szVersion, 4);
    PrintStr("\r\n", 2);
    cmdTextP = &cmdLineP->pstr[1];
    blkSize1 = topMem - blkSize1 - 256;
    blkSize2 = topMem - blkSize2 - 256;
    ParseInvokeName();
    ParseSrcFileName();
    InitFilesAndDefaults();
} /* SignOnAndGetSourceName() */
