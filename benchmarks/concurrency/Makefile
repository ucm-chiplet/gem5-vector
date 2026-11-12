# Micro-kernel de concurrencia aritmética para la VPU. Ver README.md.
#
# Uso:
#   make help                    # esta ayuda
#   make                         # VARIANT=integer, ELEMENTS=1024
#   make VARIANT=mixed ELEMENTS=256
#   make check                   # desensamblado y perfil RVV
#   make clean
#
# Para compilar y simular en gem5 (offload y native), usa run.py:
#   python3 benchmarks/concurrency/run.py --help

VARIANT ?= integer
ELEMENTS ?= 1024

CLANG ?= clang
OBJDUMP ?= llvm-objdump
PYTHON ?= python3
# No hay lld en todas las instalaciones; el ld de la toolchain GNU de
# RISC-V enlaza los objetos de Clang.
LINKER ?= $(firstword $(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-ld) \
                      riscv64-linux-gnu-ld)

ifeq ($(VARIANT),integer)
MIXED = 0
KERNEL_TYPES = vsetvli,vle32.v,vse32.v,vadd.vv,vmul.vv,vmin.vv
else ifeq ($(VARIANT),mixed)
MIXED = 1
KERNEL_TYPES = vsetvli,vle32.v,vse32.v,vadd.vv,vmin.vv,vfadd.vv,vfmul.vv
else
$(error VARIANT must be integer or mixed)
endif

# Mismas opciones que Pathfinder: la preparación y la verificación quedan
# escalares y el ejecutable es estático sin libc.
TARGET_FLAGS = --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d
BENCH_FLAGS = $(TARGET_FLAGS) -std=c11 -O2 \
              -fno-vectorize -fno-slp-vectorize \
              -Wall -Wextra -Werror \
              -ffreestanding -fno-builtin -fno-stack-protector \
              -I../common
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start

BUILD = build/$(VARIANT)-n$(ELEMENTS)
BINARY = $(BUILD)/concurrency
DISASM = $(BINARY).disasm

.PHONY: all help disasm check clean

all: $(BINARY)

$(BINARY): src/concurrency.c ../common/vpu_runtime.h Makefile
	mkdir -p $(BUILD)
	$(CLANG) $(CPPFLAGS) $(CFLAGS) $(BENCH_FLAGS) \
	    -DELEMENTS=$(ELEMENTS) -DMIXED=$(MIXED) $< \
	    $(LDFLAGS) $(LINK_FLAGS) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

disasm: $(DISASM)

check: $(DISASM) ../common/check_profile.py
	$(PYTHON) ../common/check_profile.py --kernel runKernel \
	    --types $(KERNEL_TYPES) $<

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

clean:
	rm -rf build
