CC = gcc
CFLAGS = -O2 -g -Wall -march=native
LDFLAGS = -lpthread -lrte_eal -lrte_mempool -lrte_mbuf -lrte_ring \
          -lrte_ethdev -lrte_hash -lrte_timer -lrte_net

TARGET = rocev2_gateway
SRCS = main.c \
       utils/utils.c \
       tables/connection_table_v4.c \
       processing/packet_processing.c \
       threads/thread_functions.c \
       timer/timer_system.c \
       network/network.c

OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

INCLUDES = -I. -Iutils -Itables -Iprocessing -Ithreads -Itimer -Inetwork

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)

-include $(DEPS)