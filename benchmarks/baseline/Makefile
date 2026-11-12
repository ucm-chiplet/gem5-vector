# Baseline funcional del hito 1 (GCC de la toolchain RISC-V). Ver README.md.
#
# Uso:
#   make help                # esta ayuda
#   make                     # compila baseline y baseline-simple
#   make disasm              # desensamblado de baseline
#   make disasm-simple       # desensamblado de baseline-simple
#   make run                 # run.py: baseline en offload y native
#   make run-simple          # run.py --program baseline-simple
#   make clean
#
# Opciones del runner: python3 benchmarks/baseline/run.py --help

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

.PHONY: all help disasm disasm-simple run run-simple clean

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

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

clean:
	rm -f $(BINARIES) $(addsuffix .disasm,$(BINARIES))
# SPDX-License-Identifier: BSD-3-Clause
