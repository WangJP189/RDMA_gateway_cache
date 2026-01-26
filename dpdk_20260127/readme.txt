├── main.c                    # 主程序入口
├── global.h                  # 全局数据结构和宏定义
├── config.h                  # 配置参数和常量
├── packet_defs.h             # 报文结构定义
├── data_structures.h         # 数据结构定义
├── utils/
│   ├── utils.h               # 工具函数头文件
│   └── utils.c               # 工具函数实现
├── tables/
│   ├── connection_table.h    # 连接表相关
│   └── connection_table_v4.c
├── processing/
│   ├── packet_processing.h   # 报文处理
│   └── packet_processing.c
├── threads/
│   ├── thread_functions.h    # 线程函数
│   └── thread_functions.c
├── timer/
│   ├── timer_system.h        # 定时器系统
│   └── timer_system.c
├── network/
│   ├── network.h             # 网络接口
│   └── network.c
└── cleanup.c                 # 资源清理函数

#收包线程，从网卡批量读取报文（不解析），写入gd->rx_ring无锁队列
rx_thread_func

#报文解析线程，从gd->rx_ring无锁队列读取报文，根据报文类型做相应处理
parse_thread_func
数据报文: process_data_packet
控制报文: process_control_packet
         process_ack_v4
         process_rnr_v4
         process_nak_v4

#SR重传线程接收目的网关发送的SR重传请求做重传；接收NAK报文做相应处理
sr_retrans_thread_func

#老化线程，定时做连接级节点老化
aging_thread_func

#延时定时器线程，定时器延时到期后将定时事件写入gd->delay_task_ring无锁队列
timer_epoll_thread_func

#延时任务处理线程，从gd->delay_task_ring无锁队列读取任务，根据任务类型做相应处理
delay_proc_thread_func
NAK延时处理: process_delayed_nak_retry
RMR延时处理: process_delayed_rnr_retry

