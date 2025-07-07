# XSAN 技术选型分析

## 1. 核心框架选择

### 1.1 网络通信框架

**选项对比**：

| 方案 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| 原生 Socket + epoll | 最高性能，完全控制 | 开发复杂度高 | 数据平面高性能传输 |
| gRPC + Protocol Buffers | 易于开发，跨语言支持 | 性能稍差 | 控制平面管理接口 |
| 自定义二进制协议 | 性能优秀，协议精简 | 维护成本高 | 存储数据传输 |

**最终选择**：
- **控制平面**: gRPC (节点管理、配置同步)
- **数据平面**: 自定义二进制协议 + epoll (存储I/O)
- **心跳检测**: UDP + 组播

### 1.2 元数据存储

**选项对比**：

| 方案 | 优点 | 缺点 | 性能 |
|------|------|------|------|
| LevelDB | 高性能KV，嵌入式 | 单机存储 | 优秀 |
| RocksDB | 基于LevelDB增强 | 更复杂 | 优秀 |
| SQLite | SQL支持，事务 | 性能一般 | 中等 |
| 内存+WAL | 最高性能 | 实现复杂 | 最优 |

**最终选择**: **LevelDB** (简单可靠，性能足够)

### 1.3 共识算法

**选项对比**：

| 算法 | 优点 | 缺点 | 复杂度 |
|------|------|------|--------|
| Raft | 易理解，成熟 | 性能中等 | 中等 |
| Paxos | 理论完善 | 实现复杂 | 高 |
| PBFT | 拜占庭容错 | 消息复杂度高 | 很高 |

**最终选择**: **Raft** (平衡了复杂度和可靠性)

## 2. 存储引擎设计

### 2.1 数据分布算法

**一致性哈希 + 虚拟节点**：
```c
// 哈希环实现
typedef struct hash_ring_node {
    uint32_t hash_value;
    char node_id[UUID_SIZE];
    struct hash_ring_node *next;
} hash_ring_node_t;

typedef struct hash_ring {
    hash_ring_node_t *nodes;
    uint32_t virtual_nodes_per_physical;
    uint32_t total_virtual_nodes;
} hash_ring_t;
```

### 2.2 块存储布局

**设计原则**：
- 固定块大小: 4MB (平衡性能和管理复杂度)
- 分层存储: SSD (热数据) + HDD (冷数据)
- 写入优化: 日志结构存储

```c
#define XSAN_BLOCK_SIZE (4 * 1024 * 1024)  // 4MB

typedef struct xsan_block_header {
    uint64_t block_id;
    uint64_t volume_id;
    uint32_t size;
    uint32_t checksum;
    uint64_t timestamp;
    uint8_t compression_type;
    uint8_t encryption_type;
} __attribute__((packed)) xsan_block_header_t;
```

## 3. 内存管理策略

### 3.1 内存池设计

```c
// 分级内存池
typedef enum {
    XSAN_POOL_SMALL = 0,    // < 1KB
    XSAN_POOL_MEDIUM,       // 1KB - 64KB  
    XSAN_POOL_LARGE,        // 64KB - 4MB
    XSAN_POOL_HUGE,         // > 4MB
    XSAN_POOL_MAX
} xsan_pool_type_t;

typedef struct xsan_memory_pool {
    void *pool_base;
    size_t pool_size;
    size_t block_size;
    uint32_t free_blocks;
    pthread_mutex_t lock;
    void *free_list;
} xsan_memory_pool_t;
```

### 3.2 零拷贝I/O

**技术选择**：
- `splice()` 系统调用
- `sendfile()` 用于网络传输
- `mmap()` 用于大文件操作
- `io_uring` (如果可用)

## 4. 并发控制设计

### 4.1 锁策略

**分层锁设计**：
```c
// 全局锁 (最少使用)
extern pthread_rwlock_t g_cluster_lock;

// 节点级锁
typedef struct xsan_node_locks {
    pthread_rwlock_t metadata_lock;
    pthread_mutex_t io_lock;
    pthread_spinlock_t stats_lock;
} xsan_node_locks_t;

// 卷级锁
typedef struct xsan_volume_locks {
    pthread_rwlock_t volume_lock;
    pthread_mutex_t *block_locks;  // 细粒度块锁
} xsan_volume_locks_t;
```

### 4.2 无锁数据结构

**环形队列** (用于I/O请求队列)：
```c
typedef struct lockfree_ring_buffer {
    volatile uint64_t head;
    volatile uint64_t tail;
    uint32_t size;
    uint32_t mask;
    void **entries;
} lockfree_ring_buffer_t;
```

## 5. 错误处理体系

### 5.1 错误码设计

```c
typedef enum {
    XSAN_SUCCESS = 0,
    
    // 通用错误 (1-99)
    XSAN_ERROR_NOMEM = 1,
    XSAN_ERROR_INVALID_PARAM = 2,
    XSAN_ERROR_NOT_FOUND = 3,
    
    // 集群错误 (100-199)  
    XSAN_ERROR_NODE_UNREACHABLE = 100,
    XSAN_ERROR_CLUSTER_NOT_READY = 101,
    XSAN_ERROR_CONSENSUS_FAILED = 102,
    
    // 存储错误 (200-299)
    XSAN_ERROR_VOLUME_NOT_FOUND = 200,
    XSAN_ERROR_BLOCK_CORRUPTED = 201,
    XSAN_ERROR_INSUFFICIENT_SPACE = 202,
    
    // I/O错误 (300-399)
    XSAN_ERROR_READ_FAILED = 300,
    XSAN_ERROR_WRITE_FAILED = 301,
    XSAN_ERROR_NETWORK_TIMEOUT = 302,
    
    XSAN_ERROR_MAX
} xsan_error_t;
```

### 5.2 日志系统

**多级日志**：
```c
typedef enum {
    XSAN_LOG_FATAL = 0,
    XSAN_LOG_ERROR = 1, 
    XSAN_LOG_WARN = 2,
    XSAN_LOG_INFO = 3,
    XSAN_LOG_DEBUG = 4,
    XSAN_LOG_TRACE = 5
} xsan_log_level_t;

#define XSAN_LOG(level, fmt, ...) \
    xsan_log_write(level, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
```

## 6. 性能优化策略

### 6.1 I/O优化

**异步I/O栈**：
1. **应用层**: 异步接口设计
2. **缓存层**: 写合并、预读优化  
3. **调度层**: I/O优先级调度
4. **驱动层**: 批量提交、中断合并

### 6.2 网络优化

**零拷贝网络传输**：
```c
// 使用 sendfile() 进行零拷贝传输
ssize_t xsan_sendfile_block(int sockfd, int fd, off_t offset, size_t count);

// 使用 splice() 进行管道传输
ssize_t xsan_splice_block(int in_fd, int out_fd, size_t len);
```

### 6.3 NUMA优化

**NUMA感知内存分配**：
```c
// 根据CPU亲和性分配内存
void *xsan_numa_alloc(size_t size, int node_id);

// 线程绑定到特定NUMA节点
int xsan_bind_thread_to_numa(pthread_t thread, int node_id);
```

## 7. 监控和调试

### 7.1 性能统计

```c
typedef struct xsan_perf_stats {
    atomic_uint64_t read_ops;
    atomic_uint64_t write_ops;
    atomic_uint64_t read_bytes;
    atomic_uint64_t write_bytes;
    atomic_uint64_t read_latency_us;
    atomic_uint64_t write_latency_us;
    atomic_uint64_t network_bytes_sent;
    atomic_uint64_t network_bytes_recv;
} xsan_perf_stats_t;
```

### 7.2 调试支持

**内建调试命令**：
- `xsan-debug cluster-status` - 集群状态
- `xsan-debug volume-info <vol_id>` - 卷信息
- `xsan-debug block-location <block_id>` - 块位置
- `xsan-debug perf-stats` - 性能统计

## 8. 配置管理

### 8.1 配置文件格式 (JSON)

```json
{
    "cluster": {
        "cluster_id": "xsan-cluster-001",
        "node_id": "node-001", 
        "bind_ip": "192.168.1.100",
        "bind_port": 8080,
        "data_dir": "/var/lib/xsan"
    },
    "storage": {
        "devices": [
            {"path": "/dev/nvme0n1", "type": "ssd", "role": "cache"},
            {"path": "/dev/sda", "type": "hdd", "role": "capacity"}
        ],
        "block_size": 4194304,
        "default_replica_count": 2
    },
    "network": {
        "heartbeat_interval": 1000,
        "election_timeout": 5000,
        "max_connections": 1000
    }
}
```

---

**总结**: 这个技术选型平衡了性能、复杂度和可维护性。接下来我们可以开始具体的模块实现。你觉得这个架构设计如何？有什么需要调整的地方吗？
