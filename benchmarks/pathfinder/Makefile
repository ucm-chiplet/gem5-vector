# Pathfinder (RiVEC) para la VPU. Ver README.md.
#
# Uso:
#   make help                # esta ayuda
#   make                     # INPUT=tiny, NUM_RUNS=1
#   make INPUT=small NUM_RUNS=4
#   make ALIGN=64            # datos y resultado alineados a 64 bytes
#   make check               # desensamblado y perfil RVV
#   make clean
#
# Para compilar y simular en gem5 (offload y native), usa run.py:
#   python3 benchmarks/pathfinder/run.py --help

INPUT ?= tiny
NUM_RUNS ?= 1
# Alineamiento en bytes de pathfinderWall, pathfinderReference y
# vectorResult; vacío deja el alineamiento natural (4 bytes).
ALIGN ?=

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
# -mcmodel=medany y -mno-relax permiten enlazar los mismos objetos en el
# programa de gem5 (SE) y en el de Ara RTL, cargado en 0x80000000, sin que
# el enlazador reescriba instrucciones del kernel (ara/README.md).
PATHFINDER_FLAGS = $(TARGET_FLAGS) -mcmodel=medany -mno-relax \
                   -std=c11 -O2 \
                   -fno-vectorize -fno-slp-vectorize \
                   -Wall -Wextra -Werror \
                   -ffreestanding -fno-builtin -fno-stack-protector
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start

BUILD = build/$(INPUT)-runs$(NUM_RUNS)$(if $(ALIGN),-align$(ALIGN))
BINARY = $(BUILD)/pathfinder
HEADER = $(BUILD)/pathfinder_input.h
DISASM = $(BINARY).disasm
ALIGN_FLAGS = $(if $(ALIGN),-DPATHFINDER_DATA_ALIGN=$(ALIGN))

# Objetos comunes a gem5 y Ara (kernel, datos, referencia escalar y
# comprobaciones) y objeto propio del programa de gem5.
SHARED_OBJECTS = $(BUILD)/kernel.o $(BUILD)/data.o $(BUILD)/check.o
GEM5_OBJECTS = $(BUILD)/main.o

.PHONY: all help disasm check shared clean

all: $(BINARY)

shared: $(SHARED_OBJECTS)

$(HEADER): input/data_$(INPUT).in tools/embed_input.py
	$(PYTHON) tools/embed_input.py $< $@

$(BUILD)/%.o: src/%.c src/pathfinder.h $(HEADER) Makefile
	$(CLANG) $(CPPFLAGS) $(CFLAGS) $(PATHFINDER_FLAGS) $(ALIGN_FLAGS) \
	    -DNUM_RUNS=$(NUM_RUNS) -I$(BUILD) -c $< -o $@

$(BINARY): $(SHARED_OBJECTS) $(GEM5_OBJECTS)
	$(CLANG) $(TARGET_FLAGS) $^ $(LDFLAGS) $(LINK_FLAGS) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

disasm: $(DISASM)

# Los 7 tipos RVV del kernel de Pathfinder (plan de benchmarks).
KERNEL_TYPES = vsetvli,vle32.v,vse32.v,vslide1up.vx,vslide1down.vx,vmin.vv,vadd.vv

check: $(DISASM) ../common/check_profile.py
	$(PYTHON) ../common/check_profile.py --kernel runVector \
	    --types $(KERNEL_TYPES) $<

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

clean:
	rm -rf build
