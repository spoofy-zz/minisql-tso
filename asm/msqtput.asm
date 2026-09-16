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
DONE     LM    14,12,12(13)
         BR    14
*
         LTORG
*
* C-callable: int msqtclr(void)
* Reset the next TPUT EDIT to line 1 and leave full-screen mode.
* Inline expansion of SYS1.MACLIB STLINENO LINE=1,MODE=OFF on TK5.
* STLINENO is not shipped in the local cross-assembler macro library.
*
         DROP  12
MSQTCLR  CSECT
         STM   14,12,12(13)
         LA    1,1               Next output starts at screen line 1
         LA    0,19              STLINENO terminal control function
         SLL   0,24
         SVC   94
         L     14,12(13)         Restore caller; keep SVC result in R15
         LM    0,12,20(13)
         BR    14
*
         END   MSQTPUT
