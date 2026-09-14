CC := gcc
CFLAGS := -O2 -Wall -std=gnu11
TARGET := zeroreorder_bench

.PHONY: all run plot clean

all: $(TARGET)

$(TARGET): zeroreorder_bench.c
	$(CC) $(CFLAGS) zeroreorder_bench.c -o $(TARGET) -lm

# one-shot: build + run the full N sweep (5 reps) -> out/zeroreorder_scaling.csv
run: $(TARGET)
	mkdir -p out
	./$(TARGET) -o out

# draw fig_zeroreorder_scaling.pdf from the CSV
plot: out/zeroreorder_scaling.csv
	python3 plot_zeroreorder.py

clean:
	rm -f $(TARGET)
