ifneq (,$(wildcard .env))
include .env
endif

ifeq ($(strip $(MINISQL_TOOLCHAIN_BIN)),)
ifneq (,$(wildcard $(HOME)/.local/bin/cc370))
MINISQL_TOOLCHAIN_BIN := $(HOME)/.local/bin
endif
endif

ifneq ($(strip $(MINISQL_TOOLCHAIN_BIN)),)
export PATH := $(MINISQL_TOOLCHAIN_BIN):$(PATH)
CC := $(MINISQL_TOOLCHAIN_BIN)/cc370
AS := $(MINISQL_TOOLCHAIN_BIN)/as370
LD := $(MINISQL_TOOLCHAIN_BIN)/ld370
AR := $(MINISQL_TOOLCHAIN_BIN)/ar370
endif

MBT_ROOT := mbt
include $(MBT_ROOT)/mk/mbt.mk

.PHONY: deploy-mvs deploy-mvs-dry-run

deploy-mvs:
	@tools/deploy-mvs.sh

deploy-mvs-dry-run:
	@tools/deploy-mvs.sh --dry-run
