CC = gcc

# 1. 自动获取DPDK需要的头文件路径、宏定义
DPDK_CFLAGS = $(shell pkg-config --cflags libdpdk)

# 2. 自动获取DPDK的连接库，自动包含--whole-archive和所有隐藏的底层依赖库?
DPDK_LDFLAGS = $(shell pkg-config --libs libdpdk)

# 加入DPDK的CFLAGS
CFLAGS = -O2 -g -Wall -march=native $(DPDK_CFLAGS)

# 保留-lpthread并加入DPDK的LDFLAGS
LDFLAGS = -lpthread $(DPDK_LDFLAGS)

TARGET = rocev2_gateway
SRCS = main.c \
       system/system.c \
       network/network.c \
	   threads/thread_functions.c \
	   processing/packet_processing.c \
	   tables/flow_table.c \
	   tables/conn_table.c \

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