# 260916_cache_AddExperiment1 —— 单 shot 编译（make 一条命令出全部产物）
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wmissing-field-initializers -std=gnu11 -Iinclude
LDLIBS  := -pthread -lm

SRC_CORE := src/config.c src/util.c src/pool.c src/dynblock.c
SIM_SRC  := sim/traffic.c sim/loss.c sim/nak.c sim/sim_main.c
SRC_BASE := baselines/baseline.c

BIN := build/selftest build/sim build/baseline_smoke build/exp1a_store

.PHONY: all smoke run-exp1a clean

all: $(BIN)

build/selftest: $(SRC_CORE) bench/selftest.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/sim: $(SRC_CORE) $(SIM_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/baseline_smoke: $(SRC_CORE) $(SRC_BASE) bench/baseline_smoke.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

build/exp1a_store: $(SRC_CORE) $(SRC_BASE) bench/exp1a_store.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibaselines $^ -o $@ $(LDLIBS)

run-exp1a: build/exp1a_store
	./build/exp1a_store

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
