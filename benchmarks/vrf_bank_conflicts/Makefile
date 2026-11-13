# Pathfinder 7x512 (kernel run_vector_short_m4 de Ara) para la VPU de gem5.
# Enlaza los objetos compilados por Ara; ver README.md.
ARA_ROOT ?= $(abspath ../../../ara)
ARA_APP = $(ARA_ROOT)/apps/pathfinder_r7_c512
CLANG ?= clang
LINKER ?= $(firstword $(wildcard /opt/riscv/bin/riscv64-unknown-linux-gnu-ld) riscv64-linux-gnu-ld)
TARGET_FLAGS = --target=riscv64-unknown-elf -march=rv64gcv -mabi=lp64d
CFLAGS = $(TARGET_FLAGS) -mcmodel=medany -mno-relax -O2 -fno-vectorize -fno-slp-vectorize -ffreestanding -fno-builtin -Wall -Werror
# Mismo desplazamiento dentro de la página (y de la línea) que en Ara:
# wall en ...170 y result_v en ...5d0 (bin/pathfinder_r7_c512.dump).
LINK_FLAGS = -nostdlib -static --ld-path=$(LINKER) -Wl,-e,_start \
             -Wl,--section-start=.data=0x100150 -Wl,--section-start=.l2=0x1055d0

build/pathfinder_r7_c512: build/main_gem5.o $(ARA_APP)/kernel/pathfinder.c.o $(ARA_APP)/data.S.o
	$(CLANG) $(TARGET_FLAGS) $^ $(LINK_FLAGS) -o $@
	llvm-objdump -d --mattr=+v $@ > $@.dump

build/main_gem5.o: main_gem5.c Makefile
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build
