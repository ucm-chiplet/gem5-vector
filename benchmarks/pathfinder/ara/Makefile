# Pathfinder para Ara RTL (Verilator). Ver README.md de esta carpeta.
#
# Uso:
#   make help
#   make                         # INPUT=tiny, ALIGN=64, 2 lanes, VLEN 256
#   make INPUT=small SKIP_SCALAR=1
#   make check                   # perfil RVV y kernel idéntico al de gem5
#   make clean
#
# Para compilar, simular y extraer métricas, usa run_ara.py:
#   python3 benchmarks/pathfinder/ara/run_ara.py --help
#
# Enlaza los mismos kernel.o, data.o y check.o que el programa de gem5
# (los compila ../Makefile con Clang) con el runtime bare-metal de Ara.

ARA_ROOT ?= $(abspath ../../../../ara)
INPUT ?= tiny
NUM_RUNS ?= 1
ALIGN ?= 64
NR_LANES ?= 2
VLEN ?= 256
SKIP_SCALAR ?= 0

PYTHON ?= python3
OBJDUMP ?= llvm-objdump
ARA_LLVM = $(ARA_ROOT)/install/riscv-llvm/bin
ARA_CC = $(ARA_LLVM)/clang
ARA_COMMON = $(ARA_ROOT)/apps/common

SHARED_DIR = ../build/$(INPUT)-runs$(NUM_RUNS)$(if $(ALIGN),-align$(ALIGN))
SHARED_OBJECTS = $(SHARED_DIR)/kernel.o $(SHARED_DIR)/data.o \
                 $(SHARED_DIR)/check.o
GEM5_BINARY = $(SHARED_DIR)/pathfinder

BUILD = $(SHARED_DIR)/ara-$(NR_LANES)l-v$(VLEN)$(if $(filter 1,$(SKIP_SCALAR)),-noscalar)
BINARY = $(BUILD)/pathfinder
DISASM = $(BINARY).disasm
LINK_SCRIPT = $(BUILD)/link.ld

# Opciones del runtime de Ara (apps/common/runtime.mk) para el runtime y
# para main_ara.c. El kernel no se compila aquí.
ARA_FLAGS = -march=rv64gcv_zfh_zvfh -mabi=lp64d -mno-relax \
            -fno-vectorize -mllvm -scalable-vectorization=off \
            -mllvm -riscv-v-vector-bits-min=0 -mno-implicit-float \
            -mcmodel=medany -O3 -ffast-math -fno-common -fno-builtin-printf \
            -ffunction-sections -fdata-sections -std=gnu99 \
            -DNR_LANES=$(NR_LANES) -DVLEN=$(VLEN) -I$(ARA_COMMON)
ARA_LINK = -fuse-ld=lld -static -nostartfiles -Wl,--gc-sections \
           -T$(LINK_SCRIPT)

RUNTIME = crt0.S printf.c string.c serial.c util.c
RUNTIME_OBJECTS = $(addprefix $(BUILD)/rt-,$(addsuffix .o,$(RUNTIME)))

KERNEL_TYPES = vsetvli,vle32.v,vse32.v,vslide1up.vx,vslide1down.vx,vmin.vv,vadd.vv

.PHONY: all help shared check clean FORCE

all: $(BINARY)

# Los objetos comunes los construye ../Makefile; se invoca siempre para
# que sus dependencias decidan si hay que recompilar.
$(SHARED_OBJECTS) $(GEM5_BINARY): FORCE
	$(MAKE) -C .. INPUT=$(INPUT) NUM_RUNS=$(NUM_RUNS) ALIGN=$(ALIGN) \
	    shared all

$(BUILD):
	mkdir -p $@

$(LINK_SCRIPT): $(ARA_COMMON)/arch.link.ld | $(BUILD)
	cp $< $@
	bash $(ARA_COMMON)/script/align_sections.sh $(NR_LANES) $@

$(BUILD)/rt-%.o: $(ARA_COMMON)/% | $(BUILD)
	$(ARA_CC) $(ARA_FLAGS) -c $< -o $@

$(BUILD)/main_ara.o: main_ara.c ../src/pathfinder.h $(SHARED_OBJECTS) | $(BUILD)
	$(ARA_CC) $(ARA_FLAGS) -DNUM_RUNS=$(NUM_RUNS) \
	    -DPATHFINDER_SKIP_SCALAR=$(SKIP_SCALAR) \
	    -I../src -I$(SHARED_DIR) -c $< -o $@

$(BINARY): $(BUILD)/main_ara.o $(SHARED_OBJECTS) $(RUNTIME_OBJECTS) \
           $(LINK_SCRIPT)
	$(ARA_CC) $(ARA_FLAGS) $(BUILD)/main_ara.o $(SHARED_OBJECTS) \
	    $(RUNTIME_OBJECTS) $(ARA_LINK) -o $@

$(DISASM): $(BINARY)
	$(OBJDUMP) -d --mattr=+v $< > $@

# Perfil RVV del binario de Ara (sólo los 7 tipos, todos en runVector) y
# kernel idéntico, salvo direcciones, en kernel.o, gem5 y Ara.
check: $(DISASM) $(GEM5_BINARY)
	$(PYTHON) ../../common/check_profile.py --kernel runVector \
	    --types $(KERNEL_TYPES) $(DISASM)
	$(PYTHON) ../tools/compare_kernel.py --json $(BUILD)/kernel-check.json \
	    $(SHARED_DIR)/kernel.o $(GEM5_BINARY) $(BINARY)

help:
	@sed -n '1,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

clean:
	rm -rf ../build/*/ara-*
