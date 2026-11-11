# Pathfinder (RiVEC) para la VPU. Ver README.md.
#
# Uso:
#   make                     # INPUT=tiny, NUM_RUNS=1
#   make INPUT=small NUM_RUNS=4
#   make check               # desensamblado y perfil RVV

INPUT ?= tiny
NUM_RUNS ?= 1

CLANG ?= clang
OBJDUMP ?= llvm-objdump
PYTHON ?= python3
# No hay lld en todas las instalaciones; el ld de la toolchain GNU de
# RISC-V enlaza los objetos de Clang.
LINKER ?= $(firstword $(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-ld) \
                      riscv64-linux-gnu-ld)

# Opciones acordadas para Pathfinder: -O2 -fno-vectorize
# -fno-slp-vectorize dejan escalares la copia inicial y la verificación.
# El resto construye un ejecutable estático sin libc, como el baseline.
TARGET_FLAGS = --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d
PATHFINDER_FLAGS = $(TARGET_FLAGS) -std=c11 -O2 \
                   -fno-vectorize -fno-slp-vectorize \
                   -Wall -Wextra -Werror \
                   -ffreestanding -fno-builtin -fno-stack-protector
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start

BUILD = build/$(INPUT)-runs$(NUM_RUNS)
BINARY = $(BUILD)/pathfinder
HEADER = $(BUILD)/pathfinder_input.h
DISASM = $(BINARY).disasm

.PHONY: all disasm check clean

all: $(BINARY)

$(HEADER): input/data_$(INPUT).in tools/embed_input.py
	$(PYTHON) tools/embed_input.py $< $@

$(BINARY): src/pathfinder.c $(HEADER) Makefile
	$(CLANG) $(CPPFLAGS) $(CFLAGS) $(PATHFINDER_FLAGS) \
	    -DNUM_RUNS=$(NUM_RUNS) -I$(BUILD) $< \
	    $(LDFLAGS) $(LINK_FLAGS) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

disasm: $(DISASM)

# Los 7 tipos RVV del kernel de Pathfinder (plan de benchmarks).
KERNEL_TYPES = vsetvli,vle32.v,vse32.v,vslide1up.vx,vslide1down.vx,vmin.vv,vadd.vv

check: $(DISASM) ../common/check_profile.py
	$(PYTHON) ../common/check_profile.py --kernel runVector \
	    --types $(KERNEL_TYPES) $<

clean:
	rm -rf build
