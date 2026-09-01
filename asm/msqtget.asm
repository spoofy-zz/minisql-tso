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
*
* Return strlen(buf), stopping at the first NUL left by the clear above.
*
         SR    15,15
LENLOOP  LR    4,2
         AR    4,15
         CLI   0(4),X'00'
         BE    DONE
         LA    15,1(15)
         CR    15,3
         BL    LENLOOP
         B     DONE
BAD      L     15,=F'-1'
DONE     LM    14,12,12(13)
         BR    14
*
         LTORG
         END   MSQTGET
