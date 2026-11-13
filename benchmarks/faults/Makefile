# Fault de página a mitad de un acceso unit-stride (criterio 8 del hito 2).
# Ver README.md.
#
# Uso:
#   make help                          # esta ayuda
#   make                               # VARIANT=load INSIDE=4
#   make VARIANT=store
#   make check                         # desensamblado y perfil RVV
#   make clean
#
# Para compilar y simular en gem5, usa run.py:
#   python3 benchmarks/faults/run.py --help

VARIANT ?= load
INSIDE ?= 4

CLANG ?= clang
OBJDUMP ?= llvm-objdump
PYTHON ?= python3
# No hay lld en todas las instalaciones; el ld de la toolchain GNU de
# RISC-V enlaza los objetos de Clang.
LINKER ?= $(firstword $(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-ld) \
                      riscv64-linux-gnu-ld)

ifeq ($(VARIANT),load)
VARIANT_FLAG = -DVARIANT_LOAD
KERNEL_TYPES = vsetvli,vle32.v
else ifeq ($(VARIANT),store)
VARIANT_FLAG = -DVARIANT_STORE
KERNEL_TYPES = vsetvli,vle32.v,vse32.v
else
$(error VARIANT debe ser load o store)
endif

TARGET_FLAGS = --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d
BENCH_FLAGS = $(TARGET_FLAGS) -std=c11 -O2 \
              -fno-vectorize -fno-slp-vectorize \
              -Wall -Wextra -Werror \
              -ffreestanding -fno-builtin -fno-stack-protector \
              -I../common
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start

BUILD = build/$(VARIANT)-i$(INSIDE)
BINARY = $(BUILD)/faults
DISASM = $(BINARY).disasm

.PHONY: all help disasm check clean

all: $(BINARY)

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

$(BINARY): src/faults.c ../common/vpu_runtime.h Makefile
	mkdir -p $(BUILD)
	$(CLANG) $(CPPFLAGS) $(CFLAGS) $(BENCH_FLAGS) $(VARIANT_FLAG) \
	    -DINSIDE=$(INSIDE) $< $(LDFLAGS) $(LINK_FLAGS) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

disasm: $(DISASM)

check: $(DISASM) ../common/check_profile.py
	$(PYTHON) ../common/check_profile.py --kernel runKernel \
	    --types $(KERNEL_TYPES) $<

clean:
	rm -rf build
