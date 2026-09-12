CC := gcc
CFLAGS := -O2 -Wall -std=gnu11
TARGET := behavior_bench

.PHONY: all clean

all: $(TARGET)

$(TARGET): behavior_bench.c
	$(CC) $(CFLAGS) behavior_bench.c -o $(TARGET) -lm

clean:
	rm -f $(TARGET)
