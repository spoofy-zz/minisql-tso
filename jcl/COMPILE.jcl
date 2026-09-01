//MSQCOMP  JOB (ACCT),'MINISQL',CLASS=A,MSGCLASS=X,NOTIFY=&SYSUID
//*
//* MiniSQL compile/deploy job for the cc370/mbt flow.
//*
//* Build is done on the workstation with the local mbt toolchain:
//*
//*   make VERBOSE=1
//*   make deploy ARGS=--dry-run VERBOSE=1
//*   zowe zos-files upload file-to-data-set build/minisql.deploy.xmit +
//*     IBMUSER.MBT.XMIT.IN --binary --zosmf-profile hercules
//*
//* This JCL receives that uploaded XMIT into IBMUSER.MINISQL.LOAD.
//*
//OLDLOAD  EXEC PGM=IEFBR14
//OLD      DD DSN=IBMUSER.MINISQL.LOAD,DISP=(MOD,DELETE,DELETE),
//            UNIT=SYSDA,VOL=SER=TSO003,SPACE=(TRK,1)
//RECV     EXEC PGM=IKJEFT01
//SYSTSPRT DD SYSOUT=*
//SYSTSIN  DD *
  RECEIVE INDSN('IBMUSER.MBT.XMIT.IN') -
  DATASET('IBMUSER.MINISQL.LOAD') -
  VOLUME('TSO003')
/*
