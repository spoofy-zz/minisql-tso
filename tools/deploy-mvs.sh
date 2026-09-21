#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

DRY_RUN=0
if [ "${1:-}" = "--dry-run" ] || [ "${1:-}" = "-n" ]; then
	DRY_RUN=1
fi

ENV_FILE="${MINISQL_ENV_FILE:-${ROOT_DIR}/.env}"
if [ ! -f "$ENV_FILE" ]; then
	echo "ERROR: ${ENV_FILE} not found. Copy .env.example to .env and edit it." >&2
	exit 1
fi

set -a
# shellcheck source=/dev/null
. "$ENV_FILE"
set +a

MVS_HOST="${MBT_MVS_HOST:-}"
MVS_PORT="${MBT_MVS_PORT:-1080}"
MVS_USER="${MBT_MVS_USER:-IBMUSER}"
MVS_PASS="${MBT_MVS_PASS:-}"
MVS_PROTOCOL="${MBT_MVS_PROTOCOL:-http}"
MVS_REJECT_UNAUTHORIZED="${MBT_MVS_REJECT_UNAUTHORIZED:-false}"
MVS_HLQ="${MBT_MVS_HLQ:-IBMUSER}"

if [ -n "${MINISQL_TOOLCHAIN_BIN:-}" ]; then
	export PATH="${MINISQL_TOOLCHAIN_BIN}:${PATH}"
fi

MINISQL_LOADLIB="${MINISQL_LOADLIB:-${MVS_HLQ}.MINISQL.LOAD}"
MINISQL_XMIT_IN="${MINISQL_XMIT_IN:-${MVS_HLQ}.MBT.XMIT.IN}"
MINISQL_LOAD_VOLUME="${MINISQL_LOAD_VOLUME:-TSO003}"
MINISQL_CMDPROC="${MINISQL_CMDPROC:-SYS2.CMDPROC}"

if [ -z "$MVS_HOST" ]; then
	echo "ERROR: set MBT_MVS_HOST in .env" >&2
	exit 1
fi

if [ "$DRY_RUN" != "1" ] && [ -z "$MVS_PASS" ]; then
	echo "ERROR: set MBT_MVS_PASS in .env" >&2
	exit 1
fi

ZOWE_CONN=(--host "$MVS_HOST" --port "$MVS_PORT" --user "$MVS_USER"
	--password "$MVS_PASS" --protocol "$MVS_PROTOCOL"
	--reject-unauthorized "$MVS_REJECT_UNAUTHORIZED")

run_cmd() {
	echo "+ $*"
	if [ "$DRY_RUN" = "1" ]; then
		return 0
	fi
	"$@"
}

upload_member() {
	local src="$1"
	local dsn="$2"
	local member="$3"
	local attempt

	for attempt in 1 2 3; do
		echo "+ upload ${src} -> ${dsn}(${member})"
		if [ "$DRY_RUN" = "1" ]; then
			return 0
		fi
		if zowe zos-files upload stdin-to-data-set "${dsn}(${member})" \
			"${ZOWE_CONN[@]}" < "$src"; then
			return 0
		fi
		echo "WARN: upload failed for ${dsn}(${member}), attempt ${attempt}/3" >&2
		sleep 1
	done
	return 1
}

receive_loadlib() {
	local jcl
	local attempt

	echo "+ upload build/minisql.deploy.xmit -> ${MINISQL_XMIT_IN}"
	if [ "$DRY_RUN" = "1" ]; then
		echo "+ submit RECEIVE ${MINISQL_XMIT_IN} -> ${MINISQL_LOADLIB}"
		return 0
	fi
	for attempt in 1 2 3; do
		if zowe zos-files upload file-to-data-set build/minisql.deploy.xmit \
			"$MINISQL_XMIT_IN" --binary "${ZOWE_CONN[@]}"; then
			break
		fi
		if [ "$attempt" = "3" ]; then
			return 1
		fi
		echo "WARN: upload failed for ${MINISQL_XMIT_IN}, attempt ${attempt}/3" >&2
		sleep 1
	done

	jcl="$(mktemp /tmp/minisql-receive.XXXXXX.jcl)"
	{
		echo "//MSQRECV  JOB (ACCT),'MINISQL',CLASS=A,MSGCLASS=X,NOTIFY=&SYSUID"
		echo "//* Receive MBT-generated XMIT into the load library."
		echo "//DEL      EXEC PGM=IEFBR14"
		echo "//OLDLOAD  DD DSN=${MINISQL_LOADLIB},DISP=(MOD,DELETE,DELETE),"
		echo "//             UNIT=SYSDA,SPACE=(TRK,(1,1))"
		echo "//RECV     EXEC PGM=IKJEFT01"
		echo "//SYSTSPRT DD SYSOUT=*"
		echo "//SYSTSIN  DD *"
		echo " RECEIVE INDSN('${MINISQL_XMIT_IN}') -"
		echo "  DATASET('${MINISQL_LOADLIB}') -"
		echo "  VOLUME('${MINISQL_LOAD_VOLUME}')"
		echo "/*"
	} > "$jcl"
	echo "+ submit ${jcl}"
	zowe zos-jobs submit local-file "$jcl" --wait-for-output "${ZOWE_CONN[@]}"
	rm -f "$jcl"
}

echo "Deploy host: ${MVS_PROTOCOL}://${MVS_HOST}:${MVS_PORT} as ${MVS_USER}"
echo "Loadlib:     ${MINISQL_LOADLIB}"
echo "Staging:     ${MINISQL_XMIT_IN}"
echo "CLIST:       ${MINISQL_CMDPROC}(MSQL)"
[ "$DRY_RUN" = "1" ] && echo "(dry run: no MVS changes)"

echo "+ make"
make

if [ "$DRY_RUN" = "1" ]; then
	echo "+ make deploy ARGS=--dry-run --target ${MINISQL_LOADLIB}"
	make deploy ARGS="--dry-run --target ${MINISQL_LOADLIB}"
	receive_loadlib
elif [ "$MVS_PROTOCOL" = "http" ]; then
	echo "+ make deploy ARGS=--target ${MINISQL_LOADLIB}"
	make deploy ARGS="--target ${MINISQL_LOADLIB}"
else
	echo "+ make deploy ARGS=--dry-run --target ${MINISQL_LOADLIB}"
	make deploy ARGS="--dry-run --target ${MINISQL_LOADLIB}"
	receive_loadlib
fi

upload_member clist/MSQL.clist "$MINISQL_CMDPROC" MSQL

echo "Deploy complete."
