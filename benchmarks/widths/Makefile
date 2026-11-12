# Anchos y grupos: suma y memoria unit-stride en SEW 8/16/32/64 con todos
# los LMUL del perfil (criterios 3 y 10 del hito 2). Ver README.md.
#
# Uso:
#   make help                          # esta ayuda
#   make                               # ELEMENTS=300
#   make ELEMENTS=500
#   make check                         # desensamblado y perfil RVV
#   make clean
#
# Para compilar y simular en gem5 (offload y native), usa run.py:
#   python3 benchmarks/widths/run.py --help

ELEMENTS ?= 300

CLANG ?= clang
OBJDUMP ?= llvm-objdump
PYTHON ?= python3
# No hay lld en todas las instalaciones; el ld de la toolchain GNU de
# RISC-V enlaza los objetos de Clang.
LINKER ?= $(firstword $(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-ld) \
                      riscv64-linux-gnu-ld)

# Tipos RVV exactos: check_profile.py exige que aparezcan todos, ninguno
# más y sólo dentro de runKernel.
LOAD_TYPES = vle8.v,vle16.v,vle32.v,vle64.v
STORE_TYPES = vse8.v,vse16.v,vse32.v,vse64.v
KERNEL_TYPES = vsetvli,$(LOAD_TYPES),$(STORE_TYPES),vadd.vv,vadd.vx

# Mismas opciones que Pathfinder: preparación y verificación escalares,
# ejecutable estático sin libc.
TARGET_FLAGS = --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d
BENCH_FLAGS = $(TARGET_FLAGS) -std=c11 -O2 \
              -fno-vectorize -fno-slp-vectorize \
              -Wall -Wextra -Werror \
              -ffreestanding -fno-builtin -fno-stack-protector \
              -I../common
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start

BUILD = build/n$(ELEMENTS)
BINARY = $(BUILD)/widths
DISASM = $(BINARY).disasm

.PHONY: all help disasm check clean

all: $(BINARY)

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

$(BINARY): src/widths.c ../common/vpu_runtime.h Makefile
	mkdir -p $(BUILD)
	$(CLANG) $(CPPFLAGS) $(CFLAGS) $(BENCH_FLAGS) \
	    -DELEMENTS=$(ELEMENTS) $< $(LDFLAGS) $(LINK_FLAGS) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

disasm: $(DISASM)

check: $(DISASM) ../common/check_profile.py
	$(PYTHON) ../common/check_profile.py --kernel runKernel \
	    --types $(KERNEL_TYPES) $<

clean:
	rm -rf build
