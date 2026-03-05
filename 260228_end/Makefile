CC := gcc
CFLAGS := -O2 -Wall -pthread
LDFLAGS :=

SRCS := rdma_opcode.c pkt_cache.c pkt_recv.c sr_control.c simulate.c main.c
TARGET := rdma_gateway

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
