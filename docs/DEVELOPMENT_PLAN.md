# XSAN 模块依赖关系与开发计划

## 1. 模块依赖关系

```
                    ┌─────────────────┐
                    │   应用层 (CLI)   │
                    └─────────┬───────┘
                              │
    ┌─────────────────────────┼─────────────────────────┐
    │                         │                         │
    ▼                         ▼                         ▼
┌─────────┐            ┌─────────────┐         ┌──────────────┐
│集群管理  │            │存储策略引擎  │         │虚拟化集成     │
│模块     │            │             │         │(libvirt)     │
└────┬────┘            └──────┬──────┘         └──────┬───────┘
     │                        │                       │
     │                        │                       │
     │     ┌─────────────────────────────────────────┐ │
     │     │            存储引擎核心                   │ │
     │     │  ┌─────────────┐  ┌─────────────────┐  │ │
     └─────┼──│  元数据管理  │  │   数据复制引擎   │  ├─┘
           │  │  (LevelDB)  │  │   (RAID/EC)    │  │
           │  └─────────────┘  └─────────────────┘  │
           │  ┌─────────────┐  ┌─────────────────┐  │
           │  │  块存储管理  │  │   I/O调度引擎   │  │
           │  │             │  │                 │  │
           │  └─────────────┘  └─────────────────┘  │
           └─────────────────────────────────────────┘
                              │
                    ┌─────────┼─────────┐
                    │                   │
                    ▼                   ▼
              ┌───────────┐      ┌──────────────┐
              │ 网络通信   │      │  底层工具库   │
              │ 模块      │      │ (utils/log)  │
              └───────────┘      └──────────────┘
```

## 2. 开发阶段规划

### Phase 1: 基础设施层 (2-3周)

**目标**: 搭建项目基础框架和核心工具库

**开发顺序**:
1. **工具库模块** (`src/utils/`)
   - 日志系统 (`log.c/.h`)
   - 内存管理 (`memory.c/.h`) 
   - 字符串工具 (`string_utils.c/.h`)
   - 时间工具 (`time_utils.c/.h`)
   - 配置解析 (`config.c/.h`)

2. **网络通信基础** (`src/network/`)
   - Socket 封装 (`socket.c/.h`)
   - 事件循环 (`event_loop.c/.h`)
   - 消息序列化 (`protocol.c/.h`)

3. **数据结构** (`src/common/`)
   - 哈希表 (`hashtable.c/.h`)
   - 双向链表 (`list.c/.h`)
   - 环形缓冲区 (`ring_buffer.c/.h`)

**验收标准**:
- [ ] 所有工具函数单元测试通过
- [ ] 网络通信可以收发基本消息
- [ ] 内存泄露检测通过

### Phase 2: 集群管理层 (3-4周)

**目标**: 实现节点发现、心跳检测和基础集群管理

**开发顺序**:
1. **节点管理** (`src/cluster/`)
   - 节点注册和发现 (`node_discovery.c/.h`)
   - 心跳检测 (`heartbeat.c/.h`) 
   - 节点状态管理 (`node_manager.c/.h`)

2. **简化的共识算法** (先实现基础版本)
   - 领导者选举 (`leader_election.c/.h`)
   - 配置同步 (`config_sync.c/.h`)

3. **集群 API** (`src/cluster/`)
   - 集群初始化 (`cluster_init.c/.h`)
   - 节点加入/移除 (`cluster_membership.c/.h`)

**验收标准**:
- [ ] 3节点集群可以正常启动
- [ ] 节点故障可以被检测
- [ ] 领导者选举正常工作

### Phase 3: 存储引擎核心 (4-5周)

**目标**: 实现基本的块存储和元数据管理

**开发顺序**:
1. **元数据管理** (`src/storage/`)
   - LevelDB 封装 (`metadata.c/.h`)
   - 卷管理 (`volume.c/.h`)
   - 块索引 (`block_index.c/.h`)

2. **块存储管理** (`src/storage/`)
   - 本地存储 (`local_storage.c/.h`)
   - 块分配器 (`block_allocator.c/.h`)
   - 存储设备管理 (`device_manager.c/.h`)

3. **数据分布** (`src/storage/`)
   - 一致性哈希 (`consistent_hash.c/.h`)
   - 数据放置算法 (`placement.c/.h`)

**验收标准**:
- [ ] 可以创建和删除存储卷
- [ ] 基本的读写操作正常
- [ ] 元数据持久化工作正常

### Phase 4: 数据复制引擎 (3-4周)

**目标**: 实现数据冗余和基础的一致性保证

**开发顺序**:
1. **复制管理** (`src/replication/`)
   - 副本管理器 (`replica_manager.c/.h`)
   - 同步复制 (`sync_replication.c/.h`)
   - 副本状态跟踪 (`replica_tracker.c/.h`)

2. **一致性控制** (`src/replication/`)
   - 读写一致性 (`consistency.c/.h`)
   - 冲突检测 (`conflict_detection.c/.h`)

3. **故障恢复** (`src/replication/`)
   - 数据修复 (`repair.c/.h`)
   - 副本重建 (`rebuild.c/.h`)

**验收标准**:
- [ ] 2副本数据写入正常
- [ ] 单节点故障数据不丢失
- [ ] 数据修复功能正常

### Phase 5: 虚拟化集成 (2-3周)

**目标**: 与 libvirt/KVM 集成，提供虚拟机存储

**开发顺序**:
1. **libvirt 集成** (`src/virtualization/`)
   - libvirt 接口封装 (`libvirt_wrapper.c/.h`)
   - 存储池管理 (`storage_pool.c/.h`)
   - 存储卷操作 (`storage_volume.c/.h`)

2. **块设备接口** (`src/virtualization/`)
   - 虚拟块设备 (`virtual_block_device.c/.h`)
   - I/O 请求处理 (`io_handler.c/.h`)

**验收标准**:
- [ ] VM 可以使用 XSAN 存储启动
- [ ] VM I/O 操作正常
- [ ] 热插拔存储正常

### Phase 6: 存储策略和优化 (3-4周)

**目标**: 实现存储策略、QoS 和性能优化

**开发顺序**:
1. **存储策略** (`src/policy/`)
   - 策略定义 (`policy_definition.c/.h`)
   - 策略引擎 (`policy_engine.c/.h`)
   - QoS 控制 (`qos_controller.c/.h`)

2. **I/O 优化** (`src/io/`)
   - I/O 调度器 (`io_scheduler.c/.h`)
   - 缓存管理 (`cache_manager.c/.h`)
   - 预读算法 (`readahead.c/.h`)

3. **性能监控** (`src/monitoring/`)
   - 性能统计 (`stats.c/.h`)
   - 性能分析 (`profiling.c/.h`)

**验收标准**:
- [ ] 存储策略可以正常应用
- [ ] QoS 限制生效
- [ ] I/O 性能达到预期

## 3. 技术难点与解决方案

### 3.1 并发控制

**挑战**: 高并发下的锁竞争和死锁风险
**解决方案**:
- 无锁数据结构 (环形队列、原子操作)
- 分层锁设计 (减少锁粒度)
- 锁排序 (防止死锁)

### 3.2 网络分区处理

**挑战**: 脑裂和数据一致性问题
**解决方案**:
- Raft 算法保证强一致性
- 仲裁机制 (需要超过半数节点)
- 自动故障检测和隔离

### 3.3 性能优化

**挑战**: 平衡功能完整性和性能
**解决方案**:
- 异步 I/O 和零拷贝
- 批量操作减少系统调用
- NUMA 感知的内存管理

## 4. 测试策略

### 4.1 单元测试
- 每个模块独立测试
- 使用 CUnit 测试框架
- 内存泄露检测 (Valgrind)

### 4.2 集成测试
- 多节点集群测试
- 故障注入测试
- 性能基准测试

### 4.3 压力测试
- 高并发 I/O 测试
- 长时间稳定性测试
- 资源消耗测试

## 5. 工具和环境

### 5.1 开发工具
- **编译器**: GCC 9+ (支持 C99)
- **构建系统**: CMake + Make
- **调试工具**: GDB + Valgrind
- **静态分析**: Cppcheck + Clang Static Analyzer

### 5.2 依赖管理
```bash
# Ubuntu/Debian
sudo apt-get install libvirt-dev libleveldb-dev \
    libjson-c-dev uuid-dev pthread libprotobuf-c-dev

# CentOS/RHEL  
sudo yum install libvirt-devel leveldb-devel \
    json-c-devel libuuid-devel protobuf-c-devel
```

### 5.3 开发环境
- **OS**: Ubuntu 20.04+ / CentOS 8+
- **内核**: Linux 5.4+ (支持 io_uring)
- **内存**: 至少 8GB (开发) / 16GB+ (测试)

---

这个开发计划按照依赖关系逐步推进，每个阶段都有明确的目标和验收标准。你觉得这个规划如何？我们可以从哪个阶段开始？
