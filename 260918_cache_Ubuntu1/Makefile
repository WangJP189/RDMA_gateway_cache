# 260916_cache_AddExperiment1 —— 单 shot 编译（make 一条命令出全部产物）
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wmissing-field-initializers -std=gnu11 -Iinclude
LDLIBS  := -pthread -lm
# 实验二进制加 -DNDEBUG：禁用热路径 pool_for 的 assert(gen==...)（exp1a/1b 计时卫生；机制逻辑不变）。
# selftest / baseline_smoke 保持带断言，用于正确性验证。
BENCH_CFLAGS := $(CFLAGS) -DNDEBUG

SRC_CORE := src/config.c src/util.c src/pool.c src/dynblock.c
SIM_SRC  := sim/traffic.c sim/loss.c sim/nak.c sim/sim_main.c
SRC_BASE := baselines/baseline.c

BIN := build/selftest build/sim build/baseline_smoke build/exp1a_store build/exp1b_lookup build/exp2_space build/range_equiv_test build/alloc_equiv_test build/alloc_probe build/sr_order_test

.PHONY: all smoke run-exp1a run-exp1b run-exp2 clean

all: $(BIN)

build/selftest: $(SRC_CORE) bench/selftest.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/sim: $(SRC_CORE) $(SIM_SRC)
	@mkdir -p build
	$(CC) $(BENCH_CFLAGS) $^ -o $@ $(LDLIBS)

build/baseline_smoke: $(SRC_CORE) $(SRC_BASE) bench/baseline_smoke.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

build/exp1a_store: $(SRC_CORE) $(SRC_BASE) bench/exp1a_store.c
	@mkdir -p build
	$(CC) $(BENCH_CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

run-exp1a: build/exp1a_store
	./build/exp1a_store

build/exp1b_lookup: $(SRC_CORE) $(SRC_BASE) bench/exp1b_lookup.c
	@mkdir -p build
	$(CC) $(BENCH_CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

run-exp1b: build/exp1b_lookup
	./build/exp1b_lookup

build/exp2_space: $(SRC_CORE) $(SRC_BASE) bench/exp2_space.c
	@mkdir -p build
	$(CC) $(BENCH_CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

run-exp2: build/exp2_space
	./build/exp2_space

build/range_equiv_test: $(SRC_CORE) bench/range_equiv_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/alloc_equiv_test: $(SRC_CORE) $(SRC_BASE) bench/alloc_equiv_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

build/alloc_probe: $(SRC_CORE) $(SRC_BASE) bench/alloc_probe.c
	@mkdir -p build
	$(CC) $(BENCH_CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

build/sr_order_test: $(SRC_CORE) $(SRC_BASE) bench/sr_order_test.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

# A13 运行时部分：sim_link_gbps=1 与 1000 各跑同一序列，S_trace.csv 逐字节一致。
smoke: $(BIN)
	@echo "== selftest ==" && ./build/selftest
	@echo "== baseline_smoke ==" && ./build/baseline_smoke
	@echo "== sim (gbps=1) ==" && ./build/sim --out=out/smoke_gbps1 --sim-link-gbps=1 >/dev/null
	@echo "== sim (gbps=1000) ==" && ./build/sim --out=out/smoke_gbps1000 --sim-link-gbps=1000 >/dev/null
	@cmp out/smoke_gbps1/S_trace.csv out/smoke_gbps1000/S_trace.csv \
		&& echo "SMOKE PASS: S_trace.csv 逐字节一致" \
		|| echo "SMOKE FAIL: S_trace.csv 不一致"

clean:
	rm -rf build out/smoke_gbps1 out/smoke_gbps1000
