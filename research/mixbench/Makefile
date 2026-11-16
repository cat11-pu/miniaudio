CC ?= cc
CFLAGS_COMMON = -std=c11 -Wall -Wextra -ffp-contract=off

all: mixbench mixbench_o3

mixbench: main.c mixbench.c mixbench.h
	$(CC) $(CFLAGS_COMMON) -O2 -mavx2 -mavx512f main.c mixbench.c -o mixbench -lm

mixbench_o3: main.c mixbench.c mixbench.h
	$(CC) $(CFLAGS_COMMON) -O3 -mavx2 -mavx512f main.c mixbench.c -o mixbench_o3 -lm

# Profile-guided build of the O2 configuration. Produces mixbench_pgo.
pgo:
	mkdir -p build_pgo
	$(CC) $(CFLAGS_COMMON) -O2 -mavx2 -mavx512f -fprofile-generate=build_pgo -c mixbench.c -o build_pgo/mb.o
	$(CC) $(CFLAGS_COMMON) -O2 -mavx2 -mavx512f -fprofile-generate=build_pgo -c main.c -o build_pgo/mn.o
	$(CC) -fprofile-generate=build_pgo build_pgo/mb.o build_pgo/mn.o -o build_pgo/gen -lm
	./build_pgo/gen > /dev/null
	$(CC) $(CFLAGS_COMMON) -O2 -mavx2 -mavx512f -fprofile-use=build_pgo -c mixbench.c -o build_pgo/mb.o
	$(CC) $(CFLAGS_COMMON) -O2 -mavx2 -mavx512f -fprofile-use=build_pgo -c main.c -o build_pgo/mn.o
	$(CC) build_pgo/mb.o build_pgo/mn.o -o mixbench_pgo -lm

clean:
	rm -f mixbench mixbench_o3 mixbench_pgo asan dbg pgo_gen
	rm -rf build_pgo pgo pgo2 gen use

.PHONY: all clean pgo
