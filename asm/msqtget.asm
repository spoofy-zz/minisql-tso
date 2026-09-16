         TITLE 'MSQTGET - TSO TGET LINE INPUT'
*
* C-callable: int msqtget(char *buf, int max)
*
* R1 points to a C argument list:
*   0(R1) = buffer address
*   4(R1) = buffer length
*
MSQTGET  CSECT
         STM   14,12,12(13)
         LR    12,15
         USING MSQTGET,12
*
         LR    11,1              Save C argument list
         L     2,0(11)           R2 = buffer
         L     3,4(11)           R3 = max length
         LTR   3,3
         BNP   BAD
*
         BCTR  3,0               Reserve one byte for C NUL
         LTR   3,3
         BNP   BAD
*
         TGET  (2),(3),ASIS,WAIT
         LTR   15,15
         BNZ   BAD
* R1 is the actual byte count, including embedded NULs in 3270 data.
         LR    15,1
         B     DONE
BAD      L     15,=F'-1'
* Preserve the return value in R15 (LM 14,12 would overwrite it).
DONE     L     14,12(13)
         LM    0,12,20(13)
         BR    14
*
         LTORG
         END   MSQTGET
