         TITLE 'MSQTPUT - TSO LINE OUTPUT AND SCREEN RESET'
*
* C-callable: int msqtput(char *buf, int len)
*
MSQTPUT  CSECT
         STM   14,12,12(13)
         LR    12,15
         USING MSQTPUT,12
*
         LR    11,1              Save C argument list
         L     2,0(11)           R2 = buffer
         L     3,4(11)           R3 = length
         LTR   3,3
         BNP   DONEOK
*
         TPUT  (2),(3),EDIT,WAIT
         LTR   15,15
         BNZ   DONE
DONEOK   SR    15,15
DONE     L     14,12(13)
         LM    0,12,20(13)
         BR    14
*
         LTORG
*
* C-callable: int msqtscr(char *buf, int len)
         DROP  12
MSQTSCR  CSECT
         STM   14,12,12(13)
         LR    12,15
         USING MSQTSCR,12
         L     2,0(1)
         L     3,4(1)
         TPUT  (2),(3),FULLSCR,,HOLD
         L     14,12(13)
         LM    0,12,20(13)
         BR    14
*
* C-callable: int msqtclr(void)
* Reset the next TPUT EDIT to line 1 and leave full-screen mode.
* Inline expansion of SYS1.MACLIB STLINENO LINE=1,MODE=OFF on TK5.
* STLINENO is not shipped in the local cross-assembler macro library.
*
         DROP  12
MSQTCLR  CSECT
         STM   14,12,12(13)
         LR    12,15
         USING MSQTCLR,12
         LA    2,CLSCR
         LA    3,L'CLSCR
         TPUT  (2),(3),FULLSCR,,HOLD
         LTR   15,15
         BNZ   CLRFAIL
         SR    15,15
CLRFAIL  L     14,12(13)
         LM    0,12,20(13)
         BR    14
* Repeat an EBCDIC blank across the screen.  NUL is ignored by some
* 3270 implementations, which leaves the old display contents intact.
CLSCR    DC    X'C11140403C40404013'
*
* Full-screen erase clears the display and returns the cursor home.
         DROP  12
MSQTCLR1 CSECT
         STM   14,12,12(13)
         LA    1,1               Next output starts at screen line 1
         LA    0,19              STLINENO terminal control function
         SLL   0,24
         SVC   94
         L     14,12(13)
         LM    0,12,20(13)
         BR    14
*
* C-callable: int msqtline(int line)
* Set the next TPUT EDIT output line through STLINENO.
*
MSQTLINE CSECT
         STM   14,12,12(13)
         LR    12,15
         USING MSQTLINE,12
         LR    11,1
         L     1,0(11)
         LA    0,19
         SLL   0,24
         SVC   94
         L     14,12(13)
         LM    0,12,20(13)
         BR    14
*
         END   MSQTPUT
