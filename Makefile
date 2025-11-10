CC = gcc
CFLAGS = -Wall -O2 -g
SRCS = 251103/pkt_cache.c 251103/pkt_recv.c 251103/rdma_opcode.c 251103/retransmit_stub.c
TARGET = rdma_gateway
LDLIBS = -lpthread

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f $(TARGET) *.o
