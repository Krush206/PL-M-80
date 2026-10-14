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

jmp_buf exception;

static void FinishLexPass()
{
    if (afterEOF)
        SyntaxError(ERR87);     /* MISSING 'end' , end-OF-FILE ENCOUNTERED */
    WriteLineInfo();
    WrByte(L_EOF);
    RewindTx1();
    TellF(&srcFil, (loc_t *)&srcFileTable[srcFileIdx + 9]);
    Backup((loc_t *)&srcFileTable[srcFileIdx + 9], offLastCh - offCurCh);
    CloseF(&srcFil);
} /* FinishLexPass() */


/* Dummy first byte: ParseCommandLine() points inChrP at it so that the
   leading inChrP++ in GNxtCh() lands on '\n' (previously this was done
   with out-of-bounds arithmetic on a string literal). */
static byte firstCh[2] = { 0, '\n' };

static void ParseCommandLine()
{
    InitF(&srcFil, (pointer)"SOURCE", (pointer)&srcFileTable[srcFileIdx]);
    OpenF(&srcFil, 1);
    SeekF(&srcFil,  (loc_t *)&srcFileTable[srcFileIdx + 9]);
    offCurCh = offLastCh;
    if (offFirstChM1 != 0)
        while (cmdLineP != NULL) {
            ParseControlLine((pointer) cmdLineP + offFirstChM1);   // ParseControlLine will move to first char
            offFirstChM1 = 2;   // offset to first char - 1
            cmdLineP = cmdLineP->link;                    // continuation line
        }
    offFirstChM1 = 0;       // 0 if no more cmd line
    curScopeP = (wpointer)curScope;
    inChrP = &firstCh[0];      // GNxtCh will see \n and get a non blank line
    blockDepth = 1;
    procChains[1] = 0;
    GNxtCh();               // get the first char
} /* ParseCommandLine() */

static void LexPass()
{
    ParseCommandLine();
    ParseProgram();
} /* LexPass() */


word Start0()
{
    if (setjmp(exception) == 0) {
        state = 20;     /* 9B46 */
        LexPass();
    } else {
        errorLevel = 1;
    }
    FinishLexPass();            /* exception goes to here */
    return 1; // Chain(overlay[1]);
}
