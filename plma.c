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
static byte aIxi[] = ".ixi";
static byte aObj[] = ".obj";
static byte aAsm[] = ".asm";
static byte aLst[] = ".lst";
static byte aInvocationComm[] = "INVOCATION COMMAND DOES NOT END WITH <CR><LF>";
static byte aSourceFileName[] = "SOURCE FILE NAME INCORRECT";
static byte aIllegalCommand[] = "ILLEGAL COMMAND TAIL SYNTAX";

static byte  ioBuffer[MAXLL + 2];    // holds a whole command tail line
pointer cmdTextP;


static void SkipSpace()
{
    while (*cmdTextP == ' ' || *cmdTextP == '\t' || *cmdTextP == '&') {
        if (*cmdTextP == ' ' || *cmdTextP == '\t')
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
    while (('A' <= *cmdTextP && *cmdTextP <= 'Z') || ('a' <= *cmdTextP && *cmdTextP <= 'z')
                || ('0' <= *cmdTextP && *cmdTextP <= '9') || *cmdTextP == '_')
        cmdTextP++;
} /* SkipAlphaNum() */

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
        PrintStr((pointer)"**", 2);
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


static void ParseSrcFileName()
{
    pointer fullName;       /* the file name as it appears in the command tail */
    pointer fileName;       /* the base name, without any directory */
    pointer p;              /* scanning pointer */
    pointer end;            /* end of the file name, start of the controls */
    pointer nameEnd;        /* end of the file name itself */
    pointer slash = NULL;   /* last '/' of the file name, if any */
    pointer baseName;       /* the name itself, after any directory */
    pointer dot;            /* last '.' of the base name, if any */
    word nameLen;
    word baseLen;
    char fname[MAXLL + 4];
    char dir[ISIS_PATH_LEN];
    bool haveDir = false;

    SkipSpace();
    fullName = cmdTextP;

    /* The file name ends at the first space or tab - the controls follow
       it.  It may be quoted, in which case it ends at the matching quote
       and may contain spaces, e.g. 'sub dir/prog.plm'.  Plain Unix paths:
       no :Fn: devices, no 8.3 limits, case preserved. */
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
        for (end = fullName; *end != ' ' && *end != '\t' &&
                             *end != '\r' && *end != '&'; end++)
            if (*end == '/' || *end == '\\')
                slash = end;
        nameEnd = end;
    }

    nameLen = (word)(nameEnd - fullName);
    memcpy(fname, fullName, nameLen);
    fname[nameLen] = 0;
    strcat(fname, "\n");
    PrintStr((pointer)fname, strlen(fname));

    dir[0] = 0;
    fileName = fullName;

    /* Unix: a directory in front of the name is where the source file is
       read from and where the output files are written to */
    if (slash != NULL) {
        p = fullName;
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

    /* the base name is kept whole, with the original extension; outputs
       reuse the stem (up to the last dot) with their own suffix */
    baseLen = (word)(nameEnd - fileName);
    if (baseLen == 0 || baseLen > NAME_SLOT)
        Fatal(aSourceFileName, Length(aSourceFileName));
    dot = NULL;
    for (p = fileName; p < nameEnd; p++)
        if (*p == '.')
            dot = p;
    if (dot != NULL && dot != fileName)
        nameLen = (word)(dot - fileName);       /* the stem, without extension */
    else
        nameLen = baseLen;
    if (nameLen > NAME_SLOT - 4)
        Fatal(aSourceFileName, Length(aSourceFileName));
    srcStemLen = (byte)nameLen;
    memset(srcStemName, 0, NAME_SLOT + 4);
    memmove(srcStemName, fileName, nameLen);

    baseName = fileName;                            /* the name, without any directory */
    if (haveDir) {
        char srcName[NAME_SLOT + 4];    /* bounded copy - the command tail
                                            ends at CR, not at a NUL */

        if (baseLen >= sizeof srcName)
            Fatal(aSourceFileName, Length(aSourceFileName));
        memcpy(srcName, baseName, baseLen);
        srcName[baseLen] = 0;
        SetSrcDir(dir, srcName);
    }

    srcFileIdx = 0;
    memset(srcFileTable, ' ', NAME_SLOT);
    memmove((pointer)srcFileTable, baseName, baseLen);
    memset(&srcFileTable[SRC_LOC_OFF], 0, 4);
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
    memset(ixiFileName, 0, NAME_SLOT + 4);
    memmove(ixiFileName, srcStemName, srcStemLen);
    memmove(&ixiFileName[srcStemLen], aIxi, 4);
    InitF(&ixiFile, (pointer)"IXREF ", ixiFileName);
    objBlk = objByte = 0;
    memset(objFileName, 0, NAME_SLOT + 4);
    memmove(objFileName, srcStemName, srcStemLen);
    memmove(&objFileName[srcStemLen], aObj, 4);
    InitF(&objFile, (pointer)"OBJECT", objFileName);
    memset(asmFileName, 0, NAME_SLOT + 4);
    memmove(asmFileName, srcStemName, srcStemLen);
    memmove(&asmFileName[srcStemLen], aAsm, 4);
    InitF(&asmFile, (pointer)"ASM   ", asmFileName);
    memset(lstFileName, 0, NAME_SLOT + 4);
    memmove(lstFileName, srcStemName, srcStemLen);
    memmove(&lstFileName[srcStemLen], aLst, 4);
    InitF(&lstFil, (pointer)"LIST ", lstFileName);
    InitF(&tx1File, (pointer)"UT1 ", (pointer)"PLMTX1.TMP ");
    InitF(&tx2File, (pointer)"UT2 ", (pointer)"PLMTX2.TMP ");
    InitF(&atFile, (pointer)"AT  ", (pointer)"PLMAT.TMP ");
    InitF(&nmsFile, (pointer)"NAMES ", (pointer)"PLMNMS.TMP ");
    InitF(&xrfFile, (pointer)"XREF ", (pointer)"PLMXRF.TMP ");
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
    SetDate((pointer)" ", 1);
    SetPageLen(57);
    SetMarkerInfo(20, '-', 21);
    SetPageNo(0);
    SetMarginAndTabW(0xFF, 4);
    SetTitle((pointer)" ", 1);
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
    InitF(&conFile, (pointer)"CONSOL", (pointer)":CI: ");
    OpenF(&conFile, 1);
    topMem = MemCk() - 12;
    if (topMem < 0xC000)
        Fatal(noMemMsg, Length(noMemMsg));
    GetCmdLine();
    PrintStr(signonMsg, Length(signonMsg));
    PrintStr(szVersion, 4);
    PrintStr((pointer)"\r\n", 2);
    cmdTextP = &cmdLineP->pstr[1];
    blkSize1 = topMem - blkSize1 - 256;
    blkSize2 = topMem - blkSize2 - 256;
    ParseInvokeName();
    ParseSrcFileName();
    InitFilesAndDefaults();
} /* SignOnAndGetSourceName() */
