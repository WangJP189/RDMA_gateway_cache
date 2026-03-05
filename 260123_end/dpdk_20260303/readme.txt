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

==========================================================
#收包线程：从网卡批量读取报文（不解析），写入gd->rx_ring无锁队列
==========================================================
rx_thread_func

==========================================================
#报文解析线程：从gd->rx_ring无锁队列读取报文，根据报文类型做相应处理
==========================================================
parse_thread_func
数据报文: process_data_packet
控制报文: process_control_packet
         process_ack_v4
         process_rnr_v4
         process_nak_v4

==========================================================
#SR重传线程：接收目的网关发送的SR重传请求做重传；接收NAK报文做相应处理
==========================================================
sr_retrans_thread_func

==========================================================
#老化线程：定时做连接级节点老化
==========================================================
aging_thread_func

==========================================================
#定时器创建：将定时器fd以及delay_task_info信息添加到gd->timer_epoll_fd
==========================================================
create_nak_delay_timer
只有目的网关收到nak并且缓存了psn报文才创建此定时器。

create_rnr_timer
只有目的网关会创建rnr延时定时器，源网关收到nak rnr报文时做如下处理
01.未缓存nak_psn报文，直接构造nak rnr报文发送给源主机
02.缓存nak_psn报文，直接返回，等待sr重传请求

==========================================================
#定时器调度线程：从gd->timer_epoll_fd获取到期后的定时事件写入gd->delay_task_ring无锁队列，从epoll中删除定时器fd，并close
==========================================================
timer_epoll_thread_func

==========================================================
#定时任务处理线程：从gd->delay_task_ring无锁队列读取任务，根据任务类型做相应处理
==========================================================
delay_proc_thread_func
NAK延时处理: process_delayed_nak_retry
RMR延时处理: process_delayed_rnr_retry

==========================================================
#清理所有待处理的定时器任务：ctrl+c等优雅退出场景，将gd->timer_epoll_fd，gd->delay_task_ring内资源全部清理
==========================================================
cleanup_all_pending_timers

==========================================================
#某个连接老化时如何清理待处理的定时器任务？
==========================================================
01.基于待老化entry中携带的 rnr_timer_fd 和 nak_timer_fd, 从 gd->timer_epoll_fd 中删除。
02.对于已经加入gd->delay_task_ring队列的定时任务：
   1）基于任务中携带的key查询不到entry，直接返回。
   2）基于任务中携带的key查询到entry，如果entry中 rnr_timer_fd 和 nak_timer_fd 无效，直接返回。
