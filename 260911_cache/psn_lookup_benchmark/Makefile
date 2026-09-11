CC := gcc
CFLAGS := -O2 -Wall -std=gnu11
TARGET := psn_bench

.PHONY: all clean

all: $(TARGET)

$(TARGET): psn_bench.c
	$(CC) $(CFLAGS) psn_bench.c -o $(TARGET)

clean:
	rm -f $(TARGET)
