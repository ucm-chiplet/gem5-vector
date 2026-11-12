# Ejemplos al cierre del hito 2 (plan de evolución). Ver README.md.
#
# Uso:
#   make help                          # esta ayuda
#   make                               # VARIANT=int16, ELEMENTS=100
#   make VARIANT=fp32 ELEMENTS=37
#   make check                         # desensamblado y perfil RVV
#   make clean
#
# Para compilar y simular en gem5 (offload y native), usa run.py:
#   python3 benchmarks/examples/run.py --help

VARIANT ?= int16
ELEMENTS ?= 100

CLANG ?= clang
OBJDUMP ?= llvm-objdump
PYTHON ?= python3
# No hay lld en todas las instalaciones; el ld de la toolchain GNU de
# RISC-V enlaza los objetos de Clang.
LINKER ?= $(firstword $(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-ld) \
                      riscv64-linux-gnu-ld)

# Tipos RVV exactos de cada variante; check_profile.py exige que aparezcan
# todos, ninguno más y sólo dentro de runKernel.
ifeq ($(VARIANT),int16)
FP32 = 0
KERNEL_TYPES = vsetvli,vle16.v,vse16.v,vadd.vv,vmul.vv,vslide1down.vx
else ifeq ($(VARIANT),fp32)
FP32 = 1
KERNEL_TYPES = vsetvli,vle32.v,vse32.v,vfmul.vv,vfadd.vv
else
$(error VARIANT must be int16 or fp32)
endif

# Mismas opciones que Pathfinder: preparación y verificación escalares,
# ejecutable estático sin libc. -ffp-contract=off evita que la referencia
# escalar FP32 se fusione en FMA y cambie el redondeo.
TARGET_FLAGS = --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d
BENCH_FLAGS = $(TARGET_FLAGS) -std=c11 -O2 \
              -fno-vectorize -fno-slp-vectorize -ffp-contract=off \
              -Wall -Wextra -Werror \
              -ffreestanding -fno-builtin -fno-stack-protector \
              -I../common
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start

BUILD = build/$(VARIANT)-n$(ELEMENTS)
BINARY = $(BUILD)/examples
DISASM = $(BINARY).disasm

.PHONY: all help disasm check clean

all: $(BINARY)

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

$(BINARY): src/examples.c ../common/vpu_runtime.h Makefile
	mkdir -p $(BUILD)
	$(CLANG) $(CPPFLAGS) $(CFLAGS) $(BENCH_FLAGS) \
	    -DELEMENTS=$(ELEMENTS) -DFP32=$(FP32) $< \
	    $(LDFLAGS) $(LINK_FLAGS) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

disasm: $(DISASM)

check: $(DISASM) ../common/check_profile.py
	$(PYTHON) ../common/check_profile.py --kernel runKernel \
	    --types $(KERNEL_TYPES) $<

clean:
	rm -rf build
