#ifndef CONFIG_H
#define CONFIG_H

#define SUCCESS (0)
#define ERROR (-1)

#define ERR_INIT_SHUTDOWN (-11)    // 优雅退出机制初始化失败
#define ERR_INIT_DPDK (-12)        // DPDK初始化失败
#define ERR_INIT_GW_RESOURCE (-13) // 网关资源初始化失败
#define ERR_INIT_THREADS (-14)     // 工作线程启动失败

#define ERR_PACKET_WRONG (-101) // 包错误(非RoCEv2包)

#define ERR_FLOW_RULE_ADD (-201) // 流规则添加失败
#define ERR_FLOW_RULE_DEL (-202) // 流规则删除失败
#define ERR_CONN_CTX_DEL (-211)  // 连接上下文删除失败

// WJP
// 定义重传处理结果的枚举类型(也可以改成宏定义的整数常量)
typedef enum {
    RETRANS_SUCCESS = 0,             // 处理成功
    RETRANS_INVALID_PARAM = -1,      // 参数无效
    RETRANS_NO_VALID_PSN_RANGE = -2, // 无有效PSN范围
    RETRANS_NO_CACHED_PACKETS = -3,  // 未缓存任何数据包
    RETRANS_NO_NEED = -4,            // 无需处理重传（start_psn >= epsn）
    RETRANS_DATA_ALLOC_FAIL = -5,    // connection_cache_array数据分配失败
    RETRANS_NO_PACKET = -6,          // 指定ring_buffer位置无数据包
    RETRANS_PACkET_PSN_MISMATCH = -7 // 指定ring_buffer位置数据包PSN不匹配
} retransmit_process_result;

// --- 内存池配置 ---
#define NUM_MBUFS 8191      // 内存池容量 (推荐2^n-1)
#define MBUF_CACHE_SIZE 512 // 本地缓存大小
// 逻辑推理(参考rte_mbuf_core.h中RTE_MBUF_DEFAULT_BUF_SIZE)
// // RoCEv2基础长度(Ethernet+IP+UDP+BTH+FCS)
// #define RoCEv2_BASE_LENGTH (14 + 20 + 8 + 12 + 4)
// // RoCEv2最大PMTU
// #define RoCEv2_MAX_PMTU 4096
// // RoCEv2最大帧长度(实际会更长RETH、AETH)
// #define ROCEV2_MAX_FRAME_LEN (RoCEv2_MAX_PMTU + RoCEv2_BASE_LENGTH)
// // 强制64上取整
// #define MBUF_DATA_ROOM RTE_ALIGN_CEIL(ROCEV2_MAX_FRAME_LEN, 64)
// // 强制预留128的Headroom
// #define MBUF_BUF_SIZE (MBUF_DATA_ROOM + 128)
#define MBUF_BUF_SIZE 2176

// --- 队列配置(硬件环) ---
#define SAFE_ROCEV2_MTU 1500  // 网卡硬件接收阈值(不开启Scatter)
#define NIC_RX_RING_SIZE 1024 // 网卡硬件接收描述符环大小
#define NIC_TX_RING_SIZE 1024 // 网卡硬件发送描述符环大小
#define QUEUE_COUNT 1         // 网卡队列数量

// --- 软件环配置 ---
#define USR_RING_SIZE 4096 // 无锁队列长度(必须是2的幂)
#define RX_BURST_SIZE 128  // 收包线程每次最大读取报文个数
#define TASK_BURST_SIZE 16 // 任务调度线程每次最大拉取任务个数

// --- 表项配置 ---
#define MAX_FLOW_ENTRIES 2048
#define MAX_CONN_ENTRIES 1024
#define PSN_INVALID 0x01000000      // 无效PSN
#define MAX_MBUF_ARRAY 4096         // 连接上下文mbuf指针数组容量
#define ARRAY_INDEX_MASK 0x00000FFF // 连接上下文mbuf指针数组掩码

// --- 时延配置 ---
#define AGING_INTERVAL 500 // 500ms 老化周期
#define SR_REQ_INTERVAL 1  // 1ms SR重传请求延时

// --- 缓存配置 ---
#define PSN_MASK 0xFFFFFF       // 24位PSN掩码（0~16777215）
#define PSN_HALF_CYCLE 0x800000 // 24位PSN的半周期（判断回绕的阈值）
#define PSN_MAX_VALUE PSN_MASK  // PSN最大值（2^24-1）

#endif // CONFIG_H