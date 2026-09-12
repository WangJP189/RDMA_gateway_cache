CC := gcc
CFLAGS := -O2 -Wall -std=gnu11
TARGET := space_bench

.PHONY: all clean

all: $(TARGET)

$(TARGET): space_bench.c
	$(CC) $(CFLAGS) space_bench.c -o $(TARGET)

clean:
	rm -f $(TARGET)
