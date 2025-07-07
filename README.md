# XSAN - 分布式存储系统

## 项目概述

XSAN 是一个基于 KVM 虚拟化环境的分布式存储系统，类似于 VMware vSAN 架构。它将多台物理主机的本地存储资源聚合成一个统一的存储池，为虚拟机提供高性能、高可用的块存储服务。

## 核心特性

- **分布式架构**: 多节点集群，横向扩展
- **高可用性**: 数据副本、自动故障恢复
- **KVM 集成**: 与 libvirt/KVM 深度集成
- **存储策略**: 灵活的存储策略和 QoS 控制
- **高性能**: 异步 I/O、零拷贝、NUMA 优化

## 系统要求

### 硬件要求
- **CPU**: x86_64, 4核+ 推荐
- **内存**: 8GB+ (开发), 16GB+ (生产)
- **存储**: SSD + HDD 混合配置
- **网络**: 千兆网络, 万兆推荐

### 软件要求
- **操作系统**: Ubuntu 20.04+ / CentOS 8+ / RHEL 8+
- **内核版本**: Linux 5.4+ (支持 io_uring)
- **编译器**: GCC 9+ (支持 C99)

## 依赖安装

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake \
    libvirt-dev libleveldb-dev \
    libjson-c-dev uuid-dev \
    libprotobuf-c-dev protobuf-c-compiler \
    libpthread-stubs0-dev \
    valgrind cunit-dev
```

### CentOS/RHEL
```bash
sudo yum install -y \
    gcc cmake make \
    libvirt-devel leveldb-devel \
    json-c-devel libuuid-devel \
    protobuf-c-devel \
    valgrind CUnit-devel
```

## 编译构建

```bash
# 克隆仓库
git clone https://github.com/your-org/xsan.git
cd xsan

# 创建构建目录
mkdir build && cd build

# 配置构建
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 编译
make -j$(nproc)

# 运行测试
make test
```

## 快速开始

### 1. 初始化集群

```bash
# 节点1 (192.168.1.10)
sudo ./xsan-node --init-cluster \
    --cluster-id "xsan-cluster-001" \
    --node-id "node-001" \
    --bind-ip "192.168.1.10" \
    --data-dir "/var/lib/xsan"

# 节点2 (192.168.1.11) 
sudo ./xsan-node --join-cluster \
    --cluster-id "xsan-cluster-001" \
    --node-id "node-002" \
    --bind-ip "192.168.1.11" \
    --join-address "192.168.1.10:8080" \
    --data-dir "/var/lib/xsan"

# 节点3 (192.168.1.12)
sudo ./xsan-node --join-cluster \
    --cluster-id "xsan-cluster-001" \
    --node-id "node-003" \
    --bind-ip "192.168.1.12" \
    --join-address "192.168.1.10:8080" \
    --data-dir "/var/lib/xsan"
```

### 2. 创建存储卷

```bash
# 创建存储卷
./xsan-cli volume create \
    --name "vm-storage-001" \
    --size "100GB" \
    --replica-count 2 \
    --policy "high-performance"

# 查看卷信息
./xsan-cli volume list
./xsan-cli volume info vm-storage-001
```

### 3. 配置虚拟机存储

```bash
# 将 XSAN 卷挂载为 libvirt 存储池
./xsan-cli libvirt create-pool \
    --pool-name "xsan-pool" \
    --volume-name "vm-storage-001"

# 创建虚拟机磁盘
virsh vol-create-as xsan-pool vm1-disk.img 50G
```

## 配置文件

### 节点配置 (`/etc/xsan/node.conf`)

```json
{
    "cluster": {
        "cluster_id": "xsan-cluster-001",
        "node_id": "node-001",
        "bind_ip": "192.168.1.10",
        "bind_port": 8080,
        "data_dir": "/var/lib/xsan"
    },
    "storage": {
        "devices": [
            {
                "path": "/dev/nvme0n1",
                "type": "ssd", 
                "role": "cache"
            },
            {
                "path": "/dev/sda",
                "type": "hdd",
                "role": "capacity"
            }
        ],
        "block_size": 4194304,
        "default_replica_count": 2
    },
    "network": {
        "heartbeat_interval": 1000,
        "election_timeout": 5000,
        "max_connections": 1000
    },
    "logging": {
        "level": "info",
        "file": "/var/log/xsan/node.log",
        "max_size": "100MB",
        "max_files": 10
    }
}
```

## 管理命令

### 集群管理
```bash
# 查看集群状态
./xsan-cli cluster status

# 查看节点信息
./xsan-cli node list
./xsan-cli node info <node-id>

# 移除节点
./xsan-cli node remove <node-id>
```

### 存储管理
```bash
# 存储卷操作
./xsan-cli volume create --name <name> --size <size>
./xsan-cli volume delete <volume-name>
./xsan-cli volume resize <volume-name> --size <new-size>

# 存储策略
./xsan-cli policy list
./xsan-cli policy create --name <name> --config <config-file>
```

### 监控和调试
```bash
# 性能统计
./xsan-cli stats show
./xsan-cli stats reset

# 调试信息
./xsan-debug cluster-topology
./xsan-debug volume-layout <volume-name>
./xsan-debug block-location <block-id>
```

## 开发指南

请参考以下文档：
- [架构设计](docs/ARCHITECTURE.md)
- [技术选型](docs/TECHNICAL_DESIGN.md)
- [开发计划](docs/DEVELOPMENT_PLAN.md)
- [API 文档](docs/API.md)
- [贡献指南](CONTRIBUTING.md)

## 许可证

本项目采用 Apache 2.0 许可证。详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交 Issue 和 Pull Request！请确保：
1. 代码符合项目编码规范
2. 添加相应的单元测试
3. 更新相关文档

## 支持

- **Issue 跟踪**: [GitHub Issues](https://github.com/your-org/xsan/issues)
- **讨论区**: [GitHub Discussions](https://github.com/your-org/xsan/discussions)
- **邮件列表**: xsan-dev@your-org.com
