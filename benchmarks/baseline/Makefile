ifneq ($(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-gcc),)
CROSS_COMPILE ?= /opt/riscv/bin/riscv64-unknown-linux-gnu-
else
CROSS_COMPILE ?= riscv64-linux-gnu-
endif
CC = $(CROSS_COMPILE)gcc
OBJDUMP = $(CROSS_COMPILE)objdump

BASELINE_FLAGS = -std=c11 -O2 -Wall -Wextra -Werror \
                 -march=rv64gc -mabi=lp64d \
                 -ffreestanding -fno-builtin -fno-stack-protector \
                 -fno-tree-vectorize -fno-tree-slp-vectorize \
                 -fno-tree-loop-distribute-patterns

.PHONY: all disasm run clean

all: build/baseline

build:
	mkdir -p $@

build/baseline: src/baseline.c Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BASELINE_FLAGS) $< \
	    $(LDFLAGS) -nostdlib -static -Wl,-e,_start -o $@

disasm: build/baseline
	$(OBJDUMP) -d $< > build/baseline.disasm

run:
	python3 run.py

clean:
	rm -f build/baseline build/baseline.disasm
# SPDX-License-Identifier: BSD-3-Clause
