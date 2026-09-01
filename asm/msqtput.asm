         TITLE 'MSQTPUT - TSO TPUT LINE OUTPUT'
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
         END   MSQTPUT
