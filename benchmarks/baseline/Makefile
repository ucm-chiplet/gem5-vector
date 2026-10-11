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

PROGRAMS = baseline baseline-simple
BINARIES = $(addprefix build/,$(PROGRAMS))

.PHONY: all disasm disasm-simple run run-simple clean

all: $(BINARIES)

build:
	mkdir -p $@

$(BINARIES): build/%: src/%.c Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BASELINE_FLAGS) $< \
	    $(LDFLAGS) -nostdlib -static -Wl,-e,_start -o $@

disasm: build/baseline
	$(OBJDUMP) -d $< > build/baseline.disasm

disasm-simple: build/baseline-simple
	$(OBJDUMP) -d $< > build/baseline-simple.disasm

run-simple:
	python3 run.py --program baseline-simple

run:
	python3 run.py

clean:
	rm -f $(BINARIES) $(addsuffix .disasm,$(BINARIES))
# SPDX-License-Identifier: BSD-3-Clause
