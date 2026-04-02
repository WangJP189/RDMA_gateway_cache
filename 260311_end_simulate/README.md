
## 运行流程：
### 步骤 1：创建流表
请输入源IP: 192.168.30.129
请输入目的IP: 192.168.50.129
请输入源QP号: 10
请输入目的QP号: 10
请输入PKey (Hex, e.g., ffff): ffff
[FLOW] 规则条目: 192.168.30.129(QP:10) <--> 192.168.50.129(QP:10)
流表规则创建成功！步骤 2：配置重传相关
### 步骤 2：配置重传相关
设置 AF_PACKET 重传的全局 socket（选项 7）：提示创建 AF_PACKET socket，最终打印：[INFO] AF_PACKET重传socket创建成功（无报错），回到主菜单  
设置网关 AF_PACKET 接口名称（选项 8）：输入源网关接口名（eth2）；输入目的网关接口名（eth2）
### 步骤 3：启动报文接收
选择“3”，启动eth3网卡
### 步骤 4：启动性能监控（开始统计）
 在主菜单输入10并回车  
### 步骤 5：触发报文收发（在另外 2 台虚拟机执行）
在ubuntu-vmware4上运行ib_send_bw -d rxe_receiver
在ubuntu-vmware2运行ib_send_bw -d rxe_sender 192.168.50.129
### 步骤 6：停止性能监控并查看统计（核心验证）
 在主菜单输入11并回车  
===== 性能测试统计结果 =====
总处理报文数: 1684
总处理字节数: 1820026 bytes (1.74 MB)
丢包数: 0 (丢包率: 0.00%)
重传报文数: 0 (重传率: 0.00%)
平均吞吐量: 0.06 MB/s
平均处理时延: 15.8914 ms


vmware1：
1、选项1：建立双向2个流表
源：192.168.30.129；目的：192.168.50.129；源QP、目的QP：根据实际情况；Qkey：ffff
源：192.168.50.129；目的：192.168.30.129；源QP、目的QP：根据实际情况；Qkey：ffff
2、选项7
3、选项8：eth3、eth3、eth2
4、选项5：ip是192.168.40.128；端口是9090
5、选项3：监听eth3

vmware3：
1、选项1：建立双向2个流表
源：192.168.30.129；目的：192.168.50.129；源QP、目的QP：根据实际情况；Qkey：ffff
源：192.168.50.129；目的：192.168.30.129；源QP、目的QP：根据实际情况；Qkey：ffff
2、选项7
3、选项8：eth2、eth2、eth3
4、选项6：ip是192.168.40.128；端口是9090
5、选项3：监听eth3



## 配置流程------传统方法（不用rdma网关）
sudo modprobe rdma_rxe
sudo modprobe ib_uverbs
sudo modprobe ib_verbs

### 步骤1：配置rdma网卡
在虚拟机1（ubuntu-vmware1）
sudo rdma link add rxe132 type rxe netdev eth0
在虚拟机2（ubuntu-vmware2）
sudo rdma link add rxe134 type rxe netdev eth0

### 步骤2：启动性能评估程序，监听这个网卡


### 步骤3：启动softroce
在虚拟机1（eth0的ip=192.168.239.132）的终端2上运行：
ib_send_bw -d rxe132
在虚拟机2（eth0的ip=192.168.239.134）的终端2上运行：
ib_send_bw -d rxe134 192.168.239.132


## 配置流程------使用rdma网关
sudo modprobe rdma_rxe
sudo modprobe ib_uverbs
sudo modprobe ib_verbs

在虚拟机2（ubuntu-vmware2）
sudo rdma link add rxe_sender type rxe netdev eth2
在虚拟机4（ubuntu-vmware4）
sudo rdma link add rxe_receiver type rxe netdev eth2




# RDMA网关
## 整体网络拓扑
发送方主机(vmware2:192.168.30.128)---发送方网关(vmware1:192.168.30.129)
                                        |
                                        |
接收方网关(vmware3:192.168.40.129)---接收方主机(vmware4:192.168.50.129)

子网划分：
VMnet3：192.168.30.0/24（发送方侧网络）

VMnet4：192.168.40.0/24（网关间网络）

VMnet5：192.168.50.0/24（接收方侧网络）

