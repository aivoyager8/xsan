# XSAN 分布式存储系统架构设计

## 1. 系统概述

XSAN 是一个基于 SPDK 的高性能分布式块存储系统，聚合多节点本地存储资源，面向虚拟化和高性能场景，提供统一的块存储服务。

- **核心接口**：SPDK bdev API、NVMe-oF（TCP/RDMA）、自定义 KVM vhost 驱动
- **架构特性**：Share Everything 元数据、对象分布、故障域隔离、一致性哈希、可扩展性

## 2. 总体架构

```
┌────────────────────────────────────────────────────────────┐
│                   XSAN Distributed Storage Cluster         │
├────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │   Node 1    │    │   Node 2    │    │   Node N    │     │
│  │ ┌─────────┐ │    │ ┌─────────┐ │    │ ┌─────────┐ │     │
│  │ │NVMe-oF  │ │    │ │NVMe-oF  │ │    │ │NVMe-oF  │ │     │
│  │ │Clients  │ │    │ │Clients  │ │    │ │Clients  │ │     │
│  │ └─────────┘ │    │ └─────────┘ │    │ └─────────┘ │     │
│  │ ┌─────────┐ │    │ ┌─────────┐ │    │ ┌─────────┐ │     │
│  │ │ XSAN    │ │    │ │ XSAN    │ │    │ │ XSAN    │ │     │
│  │ │ Agent   │ │    │ │ Agent   │ │    │ │ Agent   │ │     │
│  │ └─────────┘ │    │ └─────────┘ │    │ └─────────┘ │     │
│  │ ┌─────────┐ │    │ ┌─────────┐ │    │ ┌─────────┐ │     │
│  │ │ Local   │ │    │ │ Local   │ │    │ │ Local   │ │     │
│  │ │Storage  │ │    │ │Storage  │ │    │ │Storage  │ │     │
│  │ │SSD/HDD  │ │    │ │SSD/HDD  │ │    │ │SSD/HDD  │ │     │
│  │ └─────────┘ │    │ └─────────┘ │    │ └─────────┘ │     │
│  └─────────────┘    └─────────────┘    └─────────────┘     │
└────────────────────────────────────────────────────────────┘
```

## 3. 核心模块

- 集群管理：节点发现、健康检查、Raft 共识、配置同步
- 存储引擎：块分配、对象分布、一致性哈希、元数据管理
- 复制引擎：多副本、同步/异步复制、故障恢复
- 策略引擎：存储策略、QoS、分层、调度
- 客户端接口：SPDK bdev API、NVMe-oF、KVM vhost 驱动

### 3.1 集群管理模块

```c
// 集群节点信息
typedef struct xsan_node {
    node_id_t node_id;
    char hostname[256];
    uint32_t ip_address;
    uint16_t port;
    node_state_t state;
    uint64_t last_heartbeat;
    uint32_t cpu_cores;
    uint64_t memory_size;
    uint32_t disk_count;
    struct xsan_disk *disks;
    struct xsan_node *next;
} xsan_node_t;

// 集群状态管理
typedef struct xsan_cluster {
    uint32_t cluster_id;
    uint32_t node_count;
    xsan_node_t *nodes;
    raft_context_t *raft_ctx;
    pthread_mutex_t cluster_lock;
    uint64_t config_version;
} xsan_cluster_t;

// 节点健康检查
int xsan_node_health_check(xsan_node_t *node) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    uint64_t current_time = now.tv_sec * 1000000 + now.tv_nsec / 1000;
    if (current_time - node->last_heartbeat > HEARTBEAT_TIMEOUT) {
        node->state = NODE_STATE_FAILED;
        return -1;
    }
    
    // 执行存储健康检查
    for (int i = 0; i < node->disk_count; i++) {
        if (xsan_disk_health_check(&node->disks[i]) != 0) {
            node->state = NODE_STATE_DEGRADED;
        }
    }
    
    return 0;
}
```

### 3.2 存储引擎模块

```c
// 存储池配置
typedef struct xsan_pool {
    pool_id_t pool_id;
    char name[64];
    uint32_t replica_count;
    uint32_t stripe_size;
    uint32_t object_size;
    failure_domain_t failure_domain;
    qos_policy_t qos_policy;
    uint64_t total_capacity;
    uint64_t used_capacity;
    hash_ring_t *hash_ring;
} xsan_pool_t;

// 卷信息
typedef struct xsan_volume {
    volume_id_t volume_id;
    char name[64];
    pool_id_t pool_id;
    uint64_t size;
    uint32_t block_size;
    uint32_t object_count;
    volume_state_t state;
    struct xsan_object *objects;
    pthread_rwlock_t volume_lock;
} xsan_volume_t;

// 对象元数据
typedef struct xsan_object {
    object_id_t object_id;
    volume_id_t volume_id;
    uint64_t offset;
    uint32_t size;
    uint32_t stripe_count;
    struct xsan_stripe *stripes;
    object_state_t state;
    uint64_t version;
} xsan_object_t;

// 条带信息
typedef struct xsan_stripe {
    stripe_id_t stripe_id;
    object_id_t object_id;
    uint32_t stripe_index;
    node_id_t primary_node;
    node_id_t replica_nodes[MAX_REPLICAS];
    disk_id_t disk_ids[MAX_REPLICAS];
    uint64_t physical_offset[MAX_REPLICAS];
    stripe_state_t state;
} xsan_stripe_t;

// 块分配算法
int xsan_allocate_object(xsan_pool_t *pool, xsan_object_t *object) {
    // 使用一致性哈希选择节点
    uint32_t hash = murmurhash3(&object->object_id, sizeof(object->object_id));
    
    for (int i = 0; i < object->stripe_count; i++) {
        xsan_stripe_t *stripe = &object->stripes[i];
        
        // 为每个条带选择副本节点
        uint32_t stripe_hash = hash + i * HASH_RING_SIZE / object->stripe_count;
        
        if (xsan_select_replica_nodes(pool->hash_ring, stripe_hash, 
                                     pool->replica_count, stripe->replica_nodes) != 0) {
            return -1;
        }
        
        // 为每个副本分配磁盘空间
        for (int j = 0; j < pool->replica_count; j++) {
            xsan_node_t *node = xsan_find_node(stripe->replica_nodes[j]);
            if (xsan_allocate_disk_space(node, stripe->stripe_id, 
                                        pool->stripe_size, &stripe->disk_ids[j], 
                                        &stripe->physical_offset[j]) != 0) {
                return -1;
            }
        }
    }
    
    return 0;
}
```

### 3.3 复制引擎模块

```c
// 复制策略
typedef enum {
    REPLICATION_SYNC,     // 同步复制
    REPLICATION_ASYNC,    // 异步复制
    REPLICATION_QUORUM    // 仲裁复制
} replication_mode_t;

// 复制上下文
typedef struct xsan_replication_ctx {
    replication_mode_t mode;
    uint32_t replica_count;
    uint32_t quorum_size;
    uint32_t consistency_level;
    uint64_t timeout_us;
    struct spdk_thread *thread;
} xsan_replication_ctx_t;

// 异步复制请求
typedef struct xsan_replica_request {
    stripe_id_t stripe_id;
    node_id_t target_node;
    disk_id_t target_disk;
    uint64_t offset;
    uint32_t length;
    void *data;
    xsan_completion_cb completion_cb;
    void *cb_arg;
    struct xsan_replica_request *next;
} xsan_replica_request_t;

// 同步复制写入
int xsan_replicate_write_sync(xsan_replication_ctx_t *ctx, 
                             xsan_stripe_t *stripe, 
                             void *data, uint32_t length) {
    int completed = 0;
    int failed = 0;
    
    // 并行写入所有副本
    for (int i = 0; i < ctx->replica_count; i++) {
        xsan_replica_request_t *req = malloc(sizeof(xsan_replica_request_t));
        req->stripe_id = stripe->stripe_id;
        req->target_node = stripe->replica_nodes[i];
        req->target_disk = stripe->disk_ids[i];
        req->offset = stripe->physical_offset[i];
        req->length = length;
        req->data = data;
        
        if (xsan_submit_replica_write(req) == 0) {
            completed++;
        } else {
            failed++;
        }
    }
    
    // 等待所有副本完成
    if (ctx->mode == REPLICATION_SYNC) {
        return (completed == ctx->replica_count) ? 0 : -1;
    } else if (ctx->mode == REPLICATION_QUORUM) {
        return (completed >= ctx->quorum_size) ? 0 : -1;
    }
    
    return 0;
}

// 副本恢复
int xsan_recover_replica(stripe_id_t stripe_id, node_id_t failed_node, 
                        node_id_t target_node) {
    xsan_stripe_t *stripe = xsan_find_stripe(stripe_id);
    if (!stripe) return -1;
    
    // 从健康副本读取数据
    void *data = malloc(stripe->size);
    node_id_t source_node = INVALID_NODE_ID;
    
    for (int i = 0; i < MAX_REPLICAS; i++) {
        if (stripe->replica_nodes[i] != failed_node && 
            xsan_node_is_healthy(stripe->replica_nodes[i])) {
            source_node = stripe->replica_nodes[i];
            break;
        }
    }
    
    if (source_node == INVALID_NODE_ID) {
        free(data);
        return -1;
    }
    
    // 读取源数据
    if (xsan_read_stripe_data(stripe, source_node, data) != 0) {
        free(data);
        return -1;
    }
    
    // 写入目标节点
    if (xsan_write_stripe_data(stripe, target_node, data) != 0) {
        free(data);
        return -1;
    }
    
    // 更新副本信息
    for (int i = 0; i < MAX_REPLICAS; i++) {
        if (stripe->replica_nodes[i] == failed_node) {
            stripe->replica_nodes[i] = target_node;
            break;
        }
    }
    
    free(data);
    return 0;
}
```

## 4. 数据分布与寻址策略

### 4.1 寻址模式选择

XSAN采用**计算式寻址**而非MDS查询式寻址，主要考虑：

| 对比维度 | 计算式寻址 | MDS查询式寻址 |
|----------|------------|----------------|
| 性能 | 本地计算，延迟<1μs | 网络查询，延迟10-100μs |
| 可扩展性 | 无瓶颈，线性扩展 | MDS成为瓶颈 |
| 可用性 | 无单点故障 | MDS故障影响全局 |
| 一致性 | 最终一致性 | 强一致性 |
| 复杂度 | 简单，确定性算法 | 复杂，需要分布式元数据 |

### 4.2 计算式寻址实现

```c
// 数据寻址上下文
typedef struct xsan_addressing_ctx {
    hash_ring_t *hash_ring;
    uint32_t replica_count;
    failure_domain_t failure_domain;
    uint32_t object_size;
    uint32_t stripe_size;
    pthread_rwlock_t ctx_lock;
} xsan_addressing_ctx_t;

// 主寻址函数：从逻辑地址计算物理位置
int xsan_calculate_data_placement(xsan_addressing_ctx_t *ctx,
                                 volume_id_t volume_id,
                                 uint64_t logical_offset,
                                 uint32_t length,
                                 xsan_placement_info_t *placement) {
    // 1. 计算对象ID和对象内偏移
    uint64_t object_id = logical_offset / ctx->object_size;
    uint64_t object_offset = logical_offset % ctx->object_size;
    
    // 2. 计算涉及的对象范围
    uint64_t end_offset = logical_offset + length - 1;
    uint64_t end_object_id = end_offset / ctx->object_size;
    uint32_t object_count = end_object_id - object_id + 1;
    
    placement->object_count = object_count;
    placement->objects = malloc(object_count * sizeof(xsan_object_placement_t));
    
    // 3. 为每个对象计算物理位置
    for (uint32_t i = 0; i < object_count; i++) {
        uint64_t current_object_id = object_id + i;
        xsan_object_placement_t *obj_placement = &placement->objects[i];
        
        // 使用一致性哈希计算节点选择
        if (xsan_calculate_object_placement(ctx, current_object_id, 
                                           obj_placement) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// 对象级寻址计算
int xsan_calculate_object_placement(xsan_addressing_ctx_t *ctx,
                                   uint64_t object_id,
                                   xsan_object_placement_t *placement) {
    // 1. 基于对象ID计算哈希值
    uint32_t hash = murmurhash3(&object_id, sizeof(object_id));
    
    // 2. 在哈希环上查找副本节点
    pthread_rwlock_rdlock(&ctx->ctx_lock);
    
    if (xsan_find_object_nodes(ctx->hash_ring, object_id, 
                              ctx->replica_count, placement->replica_nodes) != 0) {
        pthread_rwlock_unlock(&ctx->ctx_lock);
        return -1;
    }
    
    // 3. 计算条带分布
    uint32_t stripe_count = ctx->object_size / ctx->stripe_size;
    placement->stripe_count = stripe_count;
    placement->stripes = malloc(stripe_count * sizeof(xsan_stripe_placement_t));
    
    for (uint32_t i = 0; i < stripe_count; i++) {
        xsan_stripe_placement_t *stripe = &placement->stripes[i];
        
        // 为每个条带计算哈希值（加入条带索引）
        uint64_t stripe_key = (object_id << 16) | i;
        uint32_t stripe_hash = murmurhash3(&stripe_key, sizeof(stripe_key));
        
        // 根据故障域要求选择节点
        if (xsan_select_stripe_nodes(ctx, stripe_hash, 
                                    ctx->failure_domain, stripe) != 0) {
            pthread_rwlock_unlock(&ctx->ctx_lock);
            return -1;
        }
    }
    
    pthread_rwlock_unlock(&ctx->ctx_lock);
    return 0;
}

// 快速寻址查找（无锁优化）
static inline node_id_t xsan_fast_locate_primary(uint64_t object_id,
                                                 hash_ring_t *ring) {
    uint32_t hash = murmurhash3(&object_id, sizeof(object_id));
    
    // 使用二分查找快速定位
    uint32_t left = 0, right = ring->node_count - 1;
    while (left <= right) {
        uint32_t mid = (left + right) / 2;
        hash_ring_node_t *node = &ring->nodes[mid];
        
        if (node->hash >= hash) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }
    
    return (left < ring->node_count) ? ring->nodes[left].node_id : 
                                      ring->nodes[0].node_id;
}
```

### 4.3 与MDS模式的对比分析

```c
// MDS模式示例（对比参考）
typedef struct xsan_mds_client {
    uint32_t mds_server_count;
    struct sockaddr_in *mds_servers;
    uint32_t current_mds;
    uint32_t timeout_ms;
    pthread_mutex_t client_lock;
} xsan_mds_client_t;

// MDS查询式寻址（示例对比）
int xsan_mds_lookup_placement(xsan_mds_client_t *client,
                             volume_id_t volume_id,
                             uint64_t offset,
                             uint32_t length,
                             xsan_placement_info_t *placement) {
    // 1. 构造查询请求
    xsan_mds_request_t request = {
        .type = MDS_REQUEST_LOOKUP,
        .volume_id = volume_id,
        .offset = offset,
        .length = length
    };
    
    // 2. 发送到MDS服务器
    xsan_mds_response_t response;
    if (xsan_mds_send_request(client, &request, &response) != 0) {
        return -1;
    }
    
    // 3. 解析响应
    if (response.status != MDS_STATUS_SUCCESS) {
        return -1;
    }
    
    // 4. 构造placement信息
    placement->object_count = response.object_count;
    placement->objects = malloc(response.object_count * sizeof(xsan_object_placement_t));
    memcpy(placement->objects, response.objects, 
           response.object_count * sizeof(xsan_object_placement_t));
    
    return 0;
}

// 性能对比测试
void xsan_addressing_performance_test() {
    struct timespec start, end;
    uint64_t iterations = 1000000;
    
    // 测试计算式寻址
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint64_t i = 0; i < iterations; i++) {
        xsan_placement_info_t placement;
        xsan_calculate_data_placement(addressing_ctx, 1, i * 4096, 4096, &placement);
        // 清理...
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    uint64_t calc_time = (end.tv_sec - start.tv_sec) * 1000000000 + 
                         (end.tv_nsec - start.tv_nsec);
    
    printf("计算式寻址: %lu 次查找, 平均 %lu ns/次\n", 
           iterations, calc_time / iterations);
    
    // 测试MDS查询式寻址
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint64_t i = 0; i < iterations; i++) {
        xsan_placement_info_t placement;
        xsan_mds_lookup_placement(mds_client, 1, i * 4096, 4096, &placement);
        // 清理...
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    uint64_t mds_time = (end.tv_sec - start.tv_sec) * 1000000000 + 
                        (end.tv_nsec - start.tv_nsec);
    
    printf("MDS查询式寻址: %lu 次查找, 平均 %lu ns/次\n", 
           iterations, mds_time / iterations);
}
```

### 4.4 混合模式支持

虽然XSAN主要使用计算式寻址，但为了支持特殊场景，也可以实现混合模式：

```c
// 混合寻址模式
typedef enum {
    ADDRESSING_MODE_CALCULATED,  // 纯计算式
    ADDRESSING_MODE_MDS,         // MDS查询式
    ADDRESSING_MODE_HYBRID       // 混合模式
} addressing_mode_t;

// 混合寻址上下文
typedef struct xsan_hybrid_addressing {
    addressing_mode_t mode;
    xsan_addressing_ctx_t *calc_ctx;
    xsan_mds_client_t *mds_client;
    
    // 缓存最近的MDS查询结果
    struct {
        uint32_t cache_size;
        uint32_t cache_ttl;
        xsan_placement_cache_t *cache;
        pthread_rwlock_t cache_lock;
    } placement_cache;
    
    // 模式切换策略
    struct {
        uint32_t calc_error_threshold;
        uint32_t mds_latency_threshold;
        uint64_t last_switch_time;
    } switch_policy;
} xsan_hybrid_addressing_t;

// 智能寻址选择
int xsan_hybrid_locate_data(xsan_hybrid_addressing_t *hybrid,
                           volume_id_t volume_id,
                           uint64_t offset,
                           uint32_t length,
                           xsan_placement_info_t *placement) {
    // 1. 检查缓存
    if (xsan_check_placement_cache(&hybrid->placement_cache, 
                                  volume_id, offset, length, placement) == 0) {
        return 0;
    }
    
    // 2. 根据模式选择寻址方式
    int result = -1;
    
    switch (hybrid->mode) {
        case ADDRESSING_MODE_CALCULATED:
            result = xsan_calculate_data_placement(hybrid->calc_ctx, 
                                                  volume_id, offset, length, placement);
            break;
            
        case ADDRESSING_MODE_MDS:
            result = xsan_mds_lookup_placement(hybrid->mds_client,
                                              volume_id, offset, length, placement);
            break;
            
        case ADDRESSING_MODE_HYBRID:
            // 先尝试计算式，失败则回退到MDS
            result = xsan_calculate_data_placement(hybrid->calc_ctx,
                                                  volume_id, offset, length, placement);
            if (result != 0) {
                result = xsan_mds_lookup_placement(hybrid->mds_client,
                                                  volume_id, offset, length, placement);
            }
            break;
    }
    
    // 3. 缓存成功的结果
    if (result == 0) {
        xsan_cache_placement_result(&hybrid->placement_cache,
                                   volume_id, offset, length, placement);
    }
    
    return result;
}
```

### 4.5 一致性哈希环

```c
// 哈希环节点
typedef struct hash_ring_node {
    uint32_t hash;
    node_id_t node_id;
    uint32_t weight;
    struct hash_ring_node *next;
} hash_ring_node_t;

// 哈希环
typedef struct hash_ring {
    uint32_t node_count;
    uint32_t virtual_node_count;
    hash_ring_node_t *nodes;
    pthread_rwlock_t ring_lock;
} hash_ring_t;

// 创建哈希环
hash_ring_t *xsan_create_hash_ring(uint32_t virtual_nodes_per_node) {
    hash_ring_t *ring = malloc(sizeof(hash_ring_t));
    ring->node_count = 0;
    ring->virtual_node_count = virtual_nodes_per_node;
    ring->nodes = NULL;
    pthread_rwlock_init(&ring->ring_lock, NULL);
    return ring;
}

// 添加节点到哈希环
int xsan_add_node_to_ring(hash_ring_t *ring, node_id_t node_id, uint32_t weight) {
    pthread_rwlock_wrlock(&ring->ring_lock);
    
    // 为每个物理节点创建多个虚拟节点
    for (uint32_t i = 0; i < ring->virtual_node_count; i++) {
        hash_ring_node_t *vnode = malloc(sizeof(hash_ring_node_t));
        
        // 计算虚拟节点哈希值
        char key[64];
        snprintf(key, sizeof(key), "%u:%u", node_id, i);
        vnode->hash = murmurhash3(key, strlen(key));
        vnode->node_id = node_id;
        vnode->weight = weight;
        
        // 插入到有序链表
        if (!ring->nodes || vnode->hash < ring->nodes->hash) {
            vnode->next = ring->nodes;
            ring->nodes = vnode;
        } else {
            hash_ring_node_t *current = ring->nodes;
            while (current->next && current->next->hash < vnode->hash) {
                current = current->next;
            }
            vnode->next = current->next;
            current->next = vnode;
        }
    }
    
    ring->node_count++;
    pthread_rwlock_unlock(&ring->ring_lock);
    return 0;
}

// 查找对象存储节点
int xsan_find_object_nodes(hash_ring_t *ring, object_id_t object_id, 
                          uint32_t replica_count, node_id_t *nodes) {
    uint32_t hash = murmurhash3(&object_id, sizeof(object_id));
    
    pthread_rwlock_rdlock(&ring->ring_lock);
    
    hash_ring_node_t *current = ring->nodes;
    node_id_t selected_nodes[MAX_REPLICAS];
    uint32_t selected_count = 0;
    
    // 从哈希值开始顺时针查找
    while (current && selected_count < replica_count) {
        if (current->hash >= hash) {
            // 检查是否已选中该物理节点
            bool already_selected = false;
            for (uint32_t i = 0; i < selected_count; i++) {
                if (selected_nodes[i] == current->node_id) {
                    already_selected = true;
                    break;
                }
            }
            
            if (!already_selected) {
                selected_nodes[selected_count++] = current->node_id;
            }
        }
        current = current->next;
    }
    
    // 如果没有找到足够的节点，从头开始环形查找
    if (selected_count < replica_count) {
        current = ring->nodes;
        while (current && selected_count < replica_count) {
            bool already_selected = false;
            for (uint32_t i = 0; i < selected_count; i++) {
                if (selected_nodes[i] == current->node_id) {
                    already_selected = true;
                    break;
                }
            }
            
            if (!already_selected) {
                selected_nodes[selected_count++] = current->node_id;
            }
            current = current->next;
        }
    }
    
    memcpy(nodes, selected_nodes, selected_count * sizeof(node_id_t));
    pthread_rwlock_unlock(&ring->ring_lock);
    
    return selected_count;
}
```

### 4.6 灵活副本放置与业务迁移

传统计算式寻址的局限性：
- 副本位置固定，难以优化本地访问
- 业务迁移时无法动态调整副本位置
- 缺乏基于访问模式的优化能力

XSAN采用**混合寻址+副本亲和性**策略解决这些问题：

```c
// 副本亲和性策略
typedef enum {
    REPLICA_AFFINITY_NONE,        // 无亲和性
    REPLICA_AFFINITY_CLIENT,      // 客户端亲和性
    REPLICA_AFFINITY_DATACENTER,  // 数据中心亲和性
    REPLICA_AFFINITY_RACK,        // 机架亲和性
    REPLICA_AFFINITY_CUSTOM       // 自定义亲和性
} replica_affinity_t;

// 副本放置策略
typedef struct xsan_replica_placement_policy {
    replica_affinity_t affinity_type;
    uint32_t preferred_node_count;
    node_id_t *preferred_nodes;
    uint32_t excluded_node_count;
    node_id_t *excluded_nodes;
    
    // 动态调整参数
    uint32_t migration_threshold;    // 迁移阈值
    uint32_t access_weight;          // 访问权重
    uint64_t last_migration_time;    // 上次迁移时间
} xsan_replica_placement_policy_t;

// 增强的寻址上下文
typedef struct xsan_enhanced_addressing_ctx {
    // 基础计算式寻址
    hash_ring_t *hash_ring;
    
    // 副本亲和性管理
    struct {
        uint32_t policy_count;
        xsan_replica_placement_policy_t **policies;
        pthread_rwlock_t policy_lock;
    } placement_policies;
    
    // 访问模式统计
    struct {
        uint32_t stats_window_size;
        xsan_access_stats_t *access_stats;
        pthread_rwlock_t stats_lock;
    } access_tracking;
    
    // 迁移管理
    struct {
        uint32_t max_concurrent_migrations;
        uint32_t current_migrations;
        xsan_migration_task_t *migration_queue;
        pthread_mutex_t migration_lock;
    } migration_manager;
} xsan_enhanced_addressing_ctx_t;

// 访问统计
typedef struct xsan_access_stats {
    volume_id_t volume_id;
    uint64_t object_id;
    uint32_t access_count;
    uint64_t total_bytes;
    uint32_t unique_clients;
    node_id_t *client_nodes;
    uint64_t last_access_time;
    float access_frequency;
} xsan_access_stats_t;

// 智能副本选择
int xsan_select_replica_nodes(xsan_enhanced_addressing_ctx_t *ctx,
                             uint64_t object_id,
                             client_id_t client_id,
                             uint32_t replica_count,
                             node_id_t *selected_nodes) {
    // 1. 获取客户端位置信息
    node_id_t client_node = xsan_get_client_node(client_id);
    datacenter_id_t client_dc = xsan_get_node_datacenter(client_node);
    rack_id_t client_rack = xsan_get_node_rack(client_node);
    
    // 2. 计算默认副本位置（作为备选）
    node_id_t default_replicas[MAX_REPLICAS];
    xsan_calculate_default_replicas(ctx->hash_ring, object_id, 
                                   replica_count, default_replicas);
    
    // 3. 检查是否有定制化策略
    xsan_replica_placement_policy_t *policy = 
        xsan_find_placement_policy(ctx, object_id, client_id);
    
    uint32_t selected_count = 0;
    
    // 4. 优先选择本地副本
    if (policy && policy->affinity_type != REPLICA_AFFINITY_NONE) {
        selected_count = xsan_select_nodes_by_affinity(ctx, policy, 
                                                     client_node, client_dc, client_rack,
                                                     replica_count, selected_nodes);
    }
    
    // 5. 补充默认副本
    for (uint32_t i = 0; i < replica_count && selected_count < replica_count; i++) {
        bool already_selected = false;
        for (uint32_t j = 0; j < selected_count; j++) {
            if (selected_nodes[j] == default_replicas[i]) {
                already_selected = true;
                break;
            }
        }
        
        if (!already_selected) {
            selected_nodes[selected_count++] = default_replicas[i];
        }
    }
    
    return selected_count;
}

// 基于亲和性的副本选择
uint32_t xsan_select_nodes_by_affinity(xsan_enhanced_addressing_ctx_t *ctx,
                                      xsan_replica_placement_policy_t *policy,
                                      node_id_t client_node,
                                      datacenter_id_t client_dc,
                                      rack_id_t client_rack,
                                      uint32_t replica_count,
                                      node_id_t *selected_nodes) {
    uint32_t selected_count = 0;
    
    switch (policy->affinity_type) {
        case REPLICA_AFFINITY_CLIENT:
            // 优先选择客户端所在节点
            if (xsan_node_has_capacity(client_node) && 
                !xsan_node_is_excluded(policy, client_node)) {
                selected_nodes[selected_count++] = client_node;
            }
            break;
            
        case REPLICA_AFFINITY_RACK:
            // 优先选择同机架节点
            node_id_t *rack_nodes = xsan_get_rack_nodes(client_rack);
            uint32_t rack_node_count = xsan_get_rack_node_count(client_rack);
            
            for (uint32_t i = 0; i < rack_node_count && selected_count < replica_count; i++) {
                if (xsan_node_has_capacity(rack_nodes[i]) &&
                    !xsan_node_is_excluded(policy, rack_nodes[i])) {
                    selected_nodes[selected_count++] = rack_nodes[i];
                }
            }
            break;
            
        case REPLICA_AFFINITY_DATACENTER:
            // 优先选择同数据中心节点
            node_id_t *dc_nodes = xsan_get_datacenter_nodes(client_dc);
            uint32_t dc_node_count = xsan_get_datacenter_node_count(client_dc);
            
            for (uint32_t i = 0; i < dc_node_count && selected_count < replica_count; i++) {
                if (xsan_node_has_capacity(dc_nodes[i]) &&
                    !xsan_node_is_excluded(policy, dc_nodes[i])) {
                    selected_nodes[selected_count++] = dc_nodes[i];
                }
            }
            break;
            
        case REPLICA_AFFINITY_CUSTOM:
            // 使用预定义的首选节点列表
            for (uint32_t i = 0; i < policy->preferred_node_count && 
                 selected_count < replica_count; i++) {
                if (xsan_node_has_capacity(policy->preferred_nodes[i]) &&
                    !xsan_node_is_excluded(policy, policy->preferred_nodes[i])) {
                    selected_nodes[selected_count++] = policy->preferred_nodes[i];
                }
            }
            break;
    }
    
    return selected_count;
}
```

### 4.7 动态副本迁移

```c
// 迁移触发条件
typedef enum {
    MIGRATION_TRIGGER_MANUAL,        // 手动触发
    MIGRATION_TRIGGER_ACCESS_PATTERN, // 访问模式变化
    MIGRATION_TRIGGER_LOAD_BALANCE,  // 负载均衡
    MIGRATION_TRIGGER_NODE_FAILURE,  // 节点故障
    MIGRATION_TRIGGER_BUSINESS_MOVE  // 业务迁移
} migration_trigger_t;

// 迁移任务
typedef struct xsan_migration_task {
    uint64_t task_id;
    migration_trigger_t trigger;
    uint64_t object_id;
    volume_id_t volume_id;
    
    // 迁移源和目标
    node_id_t source_node;
    node_id_t target_node;
    disk_id_t source_disk;
    disk_id_t target_disk;
    
    // 迁移状态
    migration_state_t state;
    uint32_t progress;
    uint64_t start_time;
    uint64_t estimated_completion;
    
    // 迁移策略
    struct {
        uint32_t chunk_size;
        uint32_t concurrent_chunks;
        uint32_t throttle_rate;
        bool online_migration;
    } migration_params;
    
    struct xsan_migration_task *next;
} xsan_migration_task_t;

// 业务迁移接口
int xsan_migrate_business_data(volume_id_t volume_id,
                              client_id_t client_id,
                              node_id_t new_primary_node,
                              migration_policy_t *policy) {
    // 1. 获取业务相关的所有对象
    uint32_t object_count = 0;
    uint64_t *object_ids = xsan_get_volume_objects(volume_id, &object_count);
    
    // 2. 分析当前访问模式
    xsan_access_stats_t *stats = xsan_analyze_access_pattern(volume_id, client_id);
    
    // 3. 选择需要迁移的热点对象
    uint32_t hot_object_count = 0;
    uint64_t *hot_objects = xsan_select_hot_objects(stats, object_count, 
                                                   &hot_object_count);
    
    // 4. 创建迁移任务
    for (uint32_t i = 0; i < hot_object_count; i++) {
        xsan_migration_task_t *task = xsan_create_migration_task(
            MIGRATION_TRIGGER_BUSINESS_MOVE,
            hot_objects[i],
            volume_id,
            new_primary_node,
            policy
        );
        
        if (task) {
            xsan_submit_migration_task(task);
        }
    }
    
    // 5. 更新副本放置策略
    xsan_replica_placement_policy_t *new_policy = 
        xsan_create_placement_policy(REPLICA_AFFINITY_CUSTOM);
    new_policy->preferred_nodes[0] = new_primary_node;
    new_policy->preferred_node_count = 1;
    
    xsan_update_placement_policy(volume_id, client_id, new_policy);
    
    return 0;
}

// 在线迁移实现
int xsan_migrate_online(xsan_migration_task_t *task) {
    // 1. 准备迁移环境
    task->state = MIGRATION_STATE_PREPARING;
    
    // 在目标节点分配空间
    if (xsan_allocate_target_space(task->target_node, task->object_id, 
                                  &task->target_disk) != 0) {
        return -1;
    }
    
    // 2. 开始增量迁移
    task->state = MIGRATION_STATE_COPYING;
    
    uint64_t object_size = xsan_get_object_size(task->object_id);
    uint32_t chunk_count = (object_size + task->migration_params.chunk_size - 1) / 
                          task->migration_params.chunk_size;
    
    // 创建COW（Copy-On-Write）映射
    xsan_cow_mapping_t *cow_mapping = xsan_create_cow_mapping(task->object_id);
    
    // 3. 分块迁移数据
    for (uint32_t chunk = 0; chunk < chunk_count; chunk++) {
        uint64_t chunk_offset = chunk * task->migration_params.chunk_size;
        uint32_t chunk_size = MIN(task->migration_params.chunk_size, 
                                 object_size - chunk_offset);
        
        // 读取源数据
        void *buffer = malloc(chunk_size);
        if (xsan_read_object_chunk(task->object_id, chunk_offset, 
                                  chunk_size, buffer) != 0) {
            free(buffer);
            return -1;
        }
        
        // 写入目标位置
        if (xsan_write_object_chunk(task->target_node, task->target_disk,
                                   chunk_offset, chunk_size, buffer) != 0) {
            free(buffer);
            return -1;
        }
        
        free(buffer);
        
        // 更新进度
        task->progress = (chunk + 1) * 100 / chunk_count;
        
        // 处理迁移期间的写入
        xsan_handle_cow_writes(cow_mapping, chunk_offset, chunk_size);
        
        // 流量控制
        if (task->migration_params.throttle_rate > 0) {
            usleep(1000000 / task->migration_params.throttle_rate);
        }
    }
    
    // 4. 切换副本
    task->state = MIGRATION_STATE_SWITCHING;
    
    // 原子性地更新元数据
    if (xsan_atomic_switch_replica(task->object_id, task->source_node, 
                                  task->target_node) != 0) {
        return -1;
    }
    
    // 5. 清理源数据
    task->state = MIGRATION_STATE_CLEANUP;
    xsan_cleanup_source_replica(task->source_node, task->source_disk, 
                               task->object_id);
    
    // 清理COW映射
    xsan_destroy_cow_mapping(cow_mapping);
    
    task->state = MIGRATION_STATE_COMPLETED;
    return 0;
}

// 访问模式学习
void xsan_learn_access_patterns(xsan_enhanced_addressing_ctx_t *ctx,
                               volume_id_t volume_id,
                               uint64_t object_id,
                               client_id_t client_id,
                               access_type_t access_type) {
    pthread_rwlock_wrlock(&ctx->access_tracking.stats_lock);
    
    // 查找或创建访问统计
    xsan_access_stats_t *stats = xsan_find_or_create_access_stats(
        ctx, volume_id, object_id);
    
    // 更新统计信息
    stats->access_count++;
    stats->last_access_time = xsan_get_time_us();
    
    // 更新客户端信息
    bool client_exists = false;
    for (uint32_t i = 0; i < stats->unique_clients; i++) {
        if (stats->client_nodes[i] == xsan_get_client_node(client_id)) {
            client_exists = true;
            break;
        }
    }
    
    if (!client_exists && stats->unique_clients < MAX_CLIENTS_PER_OBJECT) {
        stats->client_nodes[stats->unique_clients++] = 
            xsan_get_client_node(client_id);
    }
    
    // 计算访问频率
    uint64_t time_window = 3600 * 1000000; // 1小时窗口
    uint64_t current_time = xsan_get_time_us();
    if (current_time - stats->last_access_time < time_window) {
        stats->access_frequency = (float)stats->access_count / 
                                 (current_time - stats->last_access_time + 1);
    }
    
    pthread_rwlock_unlock(&ctx->access_tracking.stats_lock);
    
    // 检查是否需要触发迁移
    if (stats->access_frequency > ctx->migration_manager.migration_threshold) {
        xsan_consider_migration(ctx, volume_id, object_id, client_id);
    }
}
```

## 11. 本地磁盘组管理

### 11.1 磁盘组架构设计

XSAN采用分层的磁盘管理架构，支持异构存储设备的统一管理：

```c
// 磁盘类型定义
typedef enum {
    DISK_TYPE_UNKNOWN = 0,
    DISK_TYPE_HDD,              // 机械硬盘
    DISK_TYPE_SSD_SATA,         // SATA SSD
    DISK_TYPE_SSD_NVME,         // NVMe SSD
    DISK_TYPE_OPTANE,           // Intel Optane
    DISK_TYPE_PMEM              // 持久化内存
} disk_type_t;

// 磁盘状态
typedef enum {
    DISK_STATE_UNKNOWN = 0,
    DISK_STATE_HEALTHY,         // 健康状态
    DISK_STATE_WARNING,         // 警告状态
    DISK_STATE_DEGRADED,        // 降级状态
    DISK_STATE_FAILED,          // 故障状态
    DISK_STATE_OFFLINE,         // 离线状态
    DISK_STATE_MAINTENANCE      // 维护状态
} disk_state_t;

// 磁盘设备信息
typedef struct xsan_disk {
    disk_id_t disk_id;          // 磁盘唯一标识
    char device_path[256];      // 设备路径 (/dev/nvme0n1)
    char serial_number[64];     // 序列号
    char model[64];             // 型号
    char firmware[32];          // 固件版本
    
    // 基本属性
    disk_type_t type;           // 磁盘类型
    disk_state_t state;         // 磁盘状态
    uint64_t capacity;          // 总容量 (字节)
    uint64_t used_space;        // 已用空间
    uint64_t free_space;        // 可用空间
    uint32_t block_size;        // 块大小
    
    // 性能参数
    uint32_t max_iops_read;     // 最大读IOPS
    uint32_t max_iops_write;    // 最大写IOPS
    uint64_t max_bw_read;       // 最大读带宽 (MB/s)
    uint64_t max_bw_write;      // 最大写带宽 (MB/s)
    uint32_t latency_avg_us;    // 平均延迟 (微秒)
    
    // SMART信息
    struct {
        uint32_t temperature;        // 温度 (摄氏度)
        uint32_t power_cycles;       // 通电次数
        uint64_t power_on_hours;     // 通电小时数
        uint32_t reallocated_sectors; // 重新分配扇区数
        uint32_t pending_sectors;     // 待处理扇区数
        uint32_t uncorrectable_errors; // 不可纠正错误数
        uint8_t health_percentage;    // 健康百分比
    } smart_data;
    
    // 运行时状态
    uint64_t last_health_check;  // 上次健康检查时间
    uint64_t total_reads;        // 总读次数
    uint64_t total_writes;       // 总写次数
    uint64_t total_bytes_read;   // 总读字节数
    uint64_t total_bytes_written; // 总写字节数
    uint32_t current_queue_depth; // 当前队列深度
    
    // SPDK相关
    struct spdk_bdev *bdev;      // SPDK块设备
    struct spdk_bdev_desc *desc; // SPDK设备描述符
    struct spdk_io_channel *io_channel; // I/O通道
    
    // 链表节点
    struct xsan_disk *next;
} xsan_disk_t;

// 磁盘组类型
typedef enum {
    DISK_GROUP_TYPE_UNKNOWN = 0,
    DISK_GROUP_TYPE_SINGLE,     // 单盘
    DISK_GROUP_TYPE_MIRROR,     // 镜像 (RAID1)
    DISK_GROUP_TYPE_STRIPE,     // 条带 (RAID0)
    DISK_GROUP_TYPE_RAID5,      // RAID5
    DISK_GROUP_TYPE_RAID6,      // RAID6
    DISK_GROUP_TYPE_RAID10,     // RAID10
    DISK_GROUP_TYPE_JBOD        // Just a Bunch of Disks
} disk_group_type_t;

// 磁盘组配置
typedef struct xsan_disk_group {
    group_id_t group_id;        // 磁盘组ID
    char name[64];              // 磁盘组名称
    disk_group_type_t type;     // 磁盘组类型
    disk_type_t disk_type;      // 磁盘类型要求
    
    // 磁盘成员
    uint32_t disk_count;        // 磁盘数量
    uint32_t max_disks;         // 最大磁盘数
    xsan_disk_t **disks;        // 磁盘数组
    
    // 容量信息
    uint64_t total_capacity;    // 总容量
    uint64_t usable_capacity;   // 可用容量
    uint64_t used_capacity;     // 已用容量
    uint64_t reserved_capacity; // 预留容量
    
    // 性能参数
    uint32_t stripe_size;       // 条带大小
    uint32_t chunk_size;        // 块大小
    uint32_t parity_disks;      // 校验盘数量
    
    // 状态信息
    disk_state_t state;         // 磁盘组状态
    uint32_t degraded_disks;    // 降级磁盘数
    uint32_t failed_disks;      // 故障磁盘数
    uint64_t last_scrub_time;   // 上次数据校验时间
    
    // 运行时统计
    uint64_t total_ios;         // 总I/O次数
    uint64_t total_bytes;       // 总字节数
    float utilization;          // 利用率
    uint32_t avg_latency_us;    // 平均延迟
    
    // 分配信息
    struct {
        uint32_t chunk_count;       // 总块数
        uint32_t free_chunks;       // 空闲块数
        uint64_t chunk_bitmap_size; // 位图大小
        uint8_t *chunk_bitmap;      // 空闲块位图
        pthread_mutex_t alloc_lock; // 分配锁
    } allocation;
    
    // 链表节点
    struct xsan_disk_group *next;
} xsan_disk_group_t;

// 磁盘管理器
typedef struct xsan_disk_manager {
    uint32_t disk_count;        // 磁盘总数
    uint32_t group_count;       // 磁盘组总数
    xsan_disk_t *disks;         // 磁盘链表
    xsan_disk_group_t *groups;  // 磁盘组链表
    
    // 发现和监控
    pthread_t discovery_thread; // 磁盘发现线程
    pthread_t monitor_thread;   // 监控线程
    bool running;               // 运行状态
    
    // 配置参数
    uint32_t discovery_interval; // 发现间隔(秒)
    uint32_t health_check_interval; // 健康检查间隔(秒)
    uint32_t smart_check_interval;  // SMART检查间隔(秒)
    
    // 同步原语
    pthread_rwlock_t disk_lock;   // 磁盘读写锁
    pthread_rwlock_t group_lock;  // 磁盘组读写锁
    
    // 统计信息
    struct {
        uint64_t total_capacity;    // 总容量
        uint64_t usable_capacity;   // 可用容量
        uint64_t used_capacity;     // 已用容量
        uint32_t healthy_disks;     // 健康磁盘数
        uint32_t degraded_disks;    // 降级磁盘数
        uint32_t failed_disks;      // 故障磁盘数
    } stats;
} xsan_disk_manager_t;
```

### 11.2 磁盘发现与初始化

```c
// 磁盘发现配置
typedef struct xsan_disk_discovery_config {
    char device_filter[256];    // 设备过滤规则
    bool auto_add_disks;        // 自动添加新磁盘
    bool exclude_system_disks;  // 排除系统磁盘
    uint32_t min_disk_size_gb;  // 最小磁盘大小
    char blacklist_patterns[10][64]; // 黑名单模式
    uint32_t blacklist_count;   // 黑名单数量
} xsan_disk_discovery_config_t;

// 磁盘发现器
int xsan_init_disk_manager(xsan_disk_discovery_config_t *config) {
    xsan_disk_manager_t *mgr = &g_disk_manager;
    
    // 初始化管理器
    memset(mgr, 0, sizeof(xsan_disk_manager_t));
    mgr->discovery_interval = 30;      // 30秒发现一次
    mgr->health_check_interval = 60;   // 60秒健康检查
    mgr->smart_check_interval = 300;   // 5分钟SMART检查
    
    // 初始化锁
    pthread_rwlock_init(&mgr->disk_lock, NULL);
    pthread_rwlock_init(&mgr->group_lock, NULL);
    
    // 初始化SPDK
    if (spdk_bdev_initialize(NULL, NULL) != 0) {
        XSAN_LOG_ERROR("Failed to initialize SPDK bdev");
        return -1;
    }
    
    // 启动发现线程
    mgr->running = true;
    if (pthread_create(&mgr->discovery_thread, NULL, 
                      disk_discovery_thread, config) != 0) {
        XSAN_LOG_ERROR("Failed to create discovery thread");
        return -1;
    }
    
    // 启动监控线程
    if (pthread_create(&mgr->monitor_thread, NULL,
                      disk_monitor_thread, mgr) != 0) {
        XSAN_LOG_ERROR("Failed to create monitor thread");
        return -1;
    }
    
    return 0;
}

// 磁盘发现线程
void *disk_discovery_thread(void *arg) {
    xsan_disk_discovery_config_t *config = (xsan_disk_discovery_config_t *)arg;
    xsan_disk_manager_t *mgr = &g_disk_manager;
    
    while (mgr->running) {
        // 扫描系统中的块设备
        scan_system_block_devices(config);
        
        // 扫描SPDK设备
        scan_spdk_devices(config);
        
        // 检查新增设备
        check_new_devices(config);
        
        // 检查移除设备
        check_removed_devices();
        
        sleep(mgr->discovery_interval);
    }
    
    return NULL;
}

// 扫描系统块设备
int scan_system_block_devices(xsan_disk_discovery_config_t *config) {
    DIR *dir = opendir("/sys/block");
    if (!dir) {
        XSAN_LOG_ERROR("Failed to open /sys/block");
        return -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char device_path[256];
        snprintf(device_path, sizeof(device_path), "/dev/%s", entry->d_name);
        
        // 检查是否符合过滤条件
        if (!is_device_acceptable(device_path, config)) {
            continue;
        }
        
        // 检查是否已经存在
        if (find_disk_by_path(device_path)) {
            continue;
        }
        
        // 创建新磁盘对象
        xsan_disk_t *disk = create_disk_from_device(device_path);
        if (disk) {
            add_disk_to_manager(disk);
            XSAN_LOG_INFO("Discovered new disk: %s", device_path);
        }
    }
    
    closedir(dir);
    return 0;
}

// 创建磁盘对象
xsan_disk_t *create_disk_from_device(const char *device_path) {
    xsan_disk_t *disk = malloc(sizeof(xsan_disk_t));
    if (!disk) {
        return NULL;
    }
    
    memset(disk, 0, sizeof(xsan_disk_t));
    
    // 基本信息
    disk->disk_id = generate_disk_id();
    strncpy(disk->device_path, device_path, sizeof(disk->device_path) - 1);
    disk->state = DISK_STATE_UNKNOWN;
    
    // 获取设备信息
    if (get_device_info(device_path, disk) != 0) {
        free(disk);
        return NULL;
    }
    
    // 获取SMART信息
    get_smart_info(device_path, &disk->smart_data);
    
    // 创建SPDK bdev
    if (create_spdk_bdev_for_disk(disk) != 0) {
        XSAN_LOG_WARN("Failed to create SPDK bdev for %s", device_path);
        // 继续，可能是非NVMe设备
    }
    
    // 设置初始状态
    disk->state = DISK_STATE_HEALTHY;
    disk->last_health_check = get_current_time_us();
    
    return disk;
}

// 获取设备信息
int get_device_info(const char *device_path, xsan_disk_t *disk) {
    int fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        XSAN_LOG_ERROR("Failed to open device %s: %s", 
                      device_path, strerror(errno));
        return -1;
    }
    
    // 获取容量
    uint64_t size_bytes;
    if (ioctl(fd, BLKGETSIZE64, &size_bytes) == 0) {
        disk->capacity = size_bytes;
        disk->free_space = size_bytes;
    }
    
    // 获取块大小
    int block_size;
    if (ioctl(fd, BLKBSZGET, &block_size) == 0) {
        disk->block_size = block_size;
    } else {
        disk->block_size = 4096; // 默认4K
    }
    
    // 检测磁盘类型
    disk->type = detect_disk_type(device_path);
    
    // 获取设备标识信息
    get_device_identity(fd, disk);
    
    close(fd);
    return 0;
}

// 检测磁盘类型
disk_type_t detect_disk_type(const char *device_path) {
    // NVMe设备
    if (strstr(device_path, "nvme")) {
        return DISK_TYPE_SSD_NVME;
    }
    
    // 通过rotational属性判断
    char sys_path[512];
    snprintf(sys_path, sizeof(sys_path), 
            "/sys/block/%s/queue/rotational", 
            basename((char*)device_path));
    
    FILE *file = fopen(sys_path, "r");
    if (file) {
        int rotational;
        if (fscanf(file, "%d", &rotational) == 1) {
            fclose(file);
            return rotational ? DISK_TYPE_HDD : DISK_TYPE_SSD_SATA;
        }
        fclose(file);
    }
    
    return DISK_TYPE_UNKNOWN;
}

// 获取SMART信息
int get_smart_info(const char *device_path, struct smart_data *smart) {
    // 使用libatasmart或直接发送SMART命令
    memset(smart, 0, sizeof(*smart));
    
    // 示例：使用smartctl命令获取信息
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "smartctl -A %s 2>/dev/null", device_path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // 解析SMART属性
        parse_smart_attribute(line, smart);
    }
    
    pclose(fp);
    return 0;
}

// 解析SMART属性
void parse_smart_attribute(const char *line, struct smart_data *smart) {
    int id, value;
    char attr_name[64];
    
    if (sscanf(line, "%d %s %*s %*s %*s %*s %*s %*s %*s %d", 
               &id, attr_name, &value) == 3) {
        
        switch (id) {
            case 194: // Temperature
                smart->temperature = value;
                break;
            case 12:  // Power Cycle Count
                smart->power_cycles = value;
                break;
            case 9:   // Power On Hours
                smart->power_on_hours = value;
                break;
            case 5:   // Reallocated Sector Count
                smart->reallocated_sectors = value;
                break;
            case 197: // Current Pending Sector Count
                smart->pending_sectors = value;
                break;
            case 198: // Uncorrectable Error Count
                smart->uncorrectable_errors = value;
                break;
        }
    }
}

// 创建SPDK块设备
int create_spdk_bdev_for_disk(xsan_disk_t *disk) {
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(
        basename(disk->device_path));
    
    if (!bdev) {
        // 尝试创建AIO bdev
        struct spdk_bdev_aio_opts opts = {};
        opts.filename = disk->device_path;
        opts.block_size = disk->block_size;
        
        if (spdk_bdev_aio_create(&opts, basename(disk->device_path)) != 0) {
            return -1;
        }
        
        bdev = spdk_bdev_get_by_name(basename(disk->device_path));
    }
    
    if (bdev) {
        disk->bdev = bdev;
        
        // 打开设备描述符
        if (spdk_bdev_open(bdev, true, NULL, NULL, &disk->desc) != 0) {
            XSAN_LOG_ERROR("Failed to open bdev descriptor");
            return -1;
        }
        
        // 获取I/O通道
        disk->io_channel = spdk_bdev_get_io_channel(disk->desc);
        if (!disk->io_channel) {
            XSAN_LOG_ERROR("Failed to get I/O channel");
            spdk_bdev_close(disk->desc);
            return -1;
        }
    }
    
    return 0;
}
```

### 11.3 磁盘组创建与管理

```c
// 磁盘组创建参数
typedef struct xsan_disk_group_config {
    char name[64];              // 磁盘组名称
    disk_group_type_t type;     // 磁盘组类型
    disk_type_t disk_type;      // 磁盘类型要求
    uint32_t stripe_size;       // 条带大小
    uint32_t chunk_size;        // 块大小
    uint32_t min_disks;         // 最小磁盘数
    uint32_t max_disks;         // 最大磁盘数
    float reserved_ratio;       // 预留空间比例
    bool auto_select_disks;     // 自动选择磁盘
    disk_id_t *disk_ids;        // 指定磁盘ID数组
    uint32_t disk_id_count;     // 指定磁盘数量
} xsan_disk_group_config_t;

// 创建磁盘组
group_id_t xsan_create_disk_group(xsan_disk_group_config_t *config) {
    xsan_disk_manager_t *mgr = &g_disk_manager;
    
    // 验证配置
    if (validate_disk_group_config(config) != 0) {
        return INVALID_GROUP_ID;
    }
    
    // 创建磁盘组对象
    xsan_disk_group_t *group = malloc(sizeof(xsan_disk_group_t));
    if (!group) {
        return INVALID_GROUP_ID;
    }
    
    memset(group, 0, sizeof(xsan_disk_group_t));
    
    // 基本信息
    group->group_id = generate_group_id();
    strncpy(group->name, config->name, sizeof(group->name) - 1);
    group->type = config->type;
    group->disk_type = config->disk_type;
    group->stripe_size = config->stripe_size;
    group->chunk_size = config->chunk_size;
    group->max_disks = config->max_disks;
    
    // 分配磁盘数组
    group->disks = malloc(config->max_disks * sizeof(xsan_disk_t*));
    if (!group->disks) {
        free(group);
        return INVALID_GROUP_ID;
    }
    
    // 选择磁盘
    if (config->auto_select_disks) {
        if (auto_select_disks_for_group(group, config) != 0) {
            cleanup_disk_group(group);
            return INVALID_GROUP_ID;
        }
    } else {
        if (assign_disks_to_group(group, config->disk_ids, 
                                 config->disk_id_count) != 0) {
            cleanup_disk_group(group);
            return INVALID_GROUP_ID;
        }
    }
    
    // 初始化磁盘组
    if (initialize_disk_group(group) != 0) {
        cleanup_disk_group(group);
        return INVALID_GROUP_ID;
    }
    
    // 添加到管理器
    pthread_rwlock_wrlock(&mgr->group_lock);
    group->next = mgr->groups;
    mgr->groups = group;
    mgr->group_count++;
    pthread_rwlock_unlock(&mgr->group_lock);
    
    XSAN_LOG_INFO("Created disk group %s (ID: %u, Type: %d, Disks: %u)",
                 group->name, group->group_id, group->type, group->disk_count);
    
    return group->group_id;
}

// 自动选择磁盘
int auto_select_disks_for_group(xsan_disk_group_t *group, 
                               xsan_disk_group_config_t *config) {
    xsan_disk_manager_t *mgr = &g_disk_manager;
    
    // 创建候选磁盘列表
    xsan_disk_t **candidates = malloc(mgr->disk_count * sizeof(xsan_disk_t*));
    uint32_t candidate_count = 0;
    
    pthread_rwlock_rdlock(&mgr->disk_lock);
    
    xsan_disk_t *disk = mgr->disks;
    while (disk) {
        // 检查磁盘是否符合要求
        if (is_disk_suitable_for_group(disk, config)) {
            candidates[candidate_count++] = disk;
        }
        disk = disk->next;
    }
    
    pthread_rwlock_unlock(&mgr->disk_lock);
    
    // 检查是否有足够的磁盘
    if (candidate_count < config->min_disks) {
        XSAN_LOG_ERROR("Not enough suitable disks: %u < %u", 
                      candidate_count, config->min_disks);
        free(candidates);
        return -1;
    }
    
    // 根据磁盘组类型选择磁盘
    uint32_t required_disks = calculate_required_disks(config->type, config->min_disks);
    if (candidate_count < required_disks) {
        XSAN_LOG_ERROR("Not enough disks for RAID type: %u < %u",
                      candidate_count, required_disks);
        free(candidates);
        return -1;
    }
    
    // 排序候选磁盘（按容量、性能等）
    sort_candidate_disks(candidates, candidate_count, config);
    
    // 选择最佳磁盘
    for (uint32_t i = 0; i < required_disks && i < candidate_count; i++) {
        group->disks[group->disk_count++] = candidates[i];
        candidates[i]->state = DISK_STATE_HEALTHY; // 标记为使用中
    }
    
    free(candidates);
    return 0;
}

// 检查磁盘是否适合磁盘组
bool is_disk_suitable_for_group(xsan_disk_t *disk, 
                               xsan_disk_group_config_t *config) {
    // 检查磁盘类型
    if (config->disk_type != DISK_TYPE_UNKNOWN && 
        disk->type != config->disk_type) {
        return false;
    }
    
    // 检查磁盘状态
    if (disk->state != DISK_STATE_HEALTHY) {
        return false;
    }
    
    // 检查是否已被使用
    if (is_disk_in_any_group(disk)) {
        return false;
    }
    
    // 检查容量要求
    if (disk->capacity < (1ULL << 30)) { // 至少1GB
        return false;
    }
    
    // 检查SMART健康状态
    if (disk->smart_data.health_percentage < 80) {
        return false;
    }
    
    return true;
}

// 排序候选磁盘
void sort_candidate_disks(xsan_disk_t **candidates, uint32_t count,
                         xsan_disk_group_config_t *config) {
    // 根据不同策略排序
    switch (config->type) {
        case DISK_GROUP_TYPE_STRIPE:
        case DISK_GROUP_TYPE_RAID0:
            // 性能优先：按IOPS和带宽排序
            qsort(candidates, count, sizeof(xsan_disk_t*), compare_disk_performance);
            break;
            
        case DISK_GROUP_TYPE_MIRROR:
        case DISK_GROUP_TYPE_RAID1:
            // 可靠性优先：按健康状态和使用时间排序
            qsort(candidates, count, sizeof(xsan_disk_t*), compare_disk_reliability);
            break;
            
        default:
            // 容量优先：按容量大小排序
            qsort(candidates, count, sizeof(xsan_disk_t*), compare_disk_capacity);
            break;
    }
}

// 性能比较函数
int compare_disk_performance(const void *a, const void *b) {
    xsan_disk_t *disk_a = *(xsan_disk_t**)a;
    xsan_disk_t *disk_b = *(xsan_disk_t**)b;
    
    // 首先按类型排序（NVMe > SSD > HDD）
    if (disk_a->type != disk_b->type) {
        return disk_b->type - disk_a->type;
    }
    
    // 然后按IOPS排序
    uint32_t iops_a = disk_a->max_iops_read + disk_a->max_iops_write;
    uint32_t iops_b = disk_b->max_iops_read + disk_b->max_iops_write;
    
    if (iops_a != iops_b) {
        return iops_b - iops_a;
    }
    
    // 最后按带宽排序
    uint64_t bw_a = disk_a->max_bw_read + disk_a->max_bw_write;
    uint64_t bw_b = disk_b->max_bw_read + disk_b->max_bw_write;
    
    return (bw_b > bw_a) ? 1 : (bw_b < bw_a) ? -1 : 0;
}

// 初始化磁盘组
int initialize_disk_group(xsan_disk_group_t *group) {
    // 计算总容量和可用容量
    calculate_group_capacity(group);
    
    // 初始化分配位图
    if (initialize_allocation_bitmap(group) != 0) {
        return -1;
    }
    
    // 根据RAID类型初始化特定设置
    switch (group->type) {
        case DISK_GROUP_TYPE_RAID5:
            group->parity_disks = 1;
            break;
        case DISK_GROUP_TYPE_RAID6:
            group->parity_disks = 2;
            break;
        case DISK_GROUP_TYPE_RAID10:
            if (group->disk_count % 2 != 0) {
                XSAN_LOG_ERROR("RAID10 requires even number of disks");
                return -1;
            }
            break;
        default:
            group->parity_disks = 0;
            break;
    }
    
    // 设置初始状态
    group->state = DISK_STATE_HEALTHY;
    group->degraded_disks = 0;
    group->failed_disks = 0;
    
    return 0;
}

// 计算磁盘组容量
void calculate_group_capacity(xsan_disk_group_t *group) {
    uint64_t min_disk_capacity = UINT64_MAX;
    uint64_t total_raw_capacity = 0;
    
    // 找到最小磁盘容量
    for (uint32_t i = 0; i < group->disk_count; i++) {
        uint64_t capacity = group->disks[i]->capacity;
        if (capacity < min_disk_capacity) {
            min_disk_capacity = capacity;
        }
        total_raw_capacity += capacity;
    }
    
    // 根据RAID类型计算可用容量
    switch (group->type) {
        case DISK_GROUP_TYPE_SINGLE:
        case DISK_GROUP_TYPE_JBOD:
            group->usable_capacity = total_raw_capacity;
            break;
            
        case DISK_GROUP_TYPE_MIRROR:
            group->usable_capacity = min_disk_capacity;
            break;
            
        case DISK_GROUP_TYPE_STRIPE:
            group->usable_capacity = min_disk_capacity * group->disk_count;
            break;
            
        case DISK_GROUP_TYPE_RAID5:
            group->usable_capacity = min_disk_capacity * (group->disk_count - 1);
            break;
            
        case DISK_GROUP_TYPE_RAID6:
            group->usable_capacity = min_disk_capacity * (group->disk_count - 2);
            break;
            
        case DISK_GROUP_TYPE_RAID10:
            group->usable_capacity = min_disk_capacity * (group->disk_count / 2);
            break;
    }
    
    // 预留空间
    group->reserved_capacity = group->usable_capacity * 0.05; // 5%预留
    group->total_capacity = group->usable_capacity;
    group->usable_capacity -= group->reserved_capacity;
}

// 初始化分配位图
int initialize_allocation_bitmap(xsan_disk_group_t *group) {
    // 计算块数量
    group->allocation.chunk_count = group->usable_capacity / group->chunk_size;
    
    // 分配位图
    group->allocation.chunk_bitmap_size = 
        (group->allocation.chunk_count + 7) / 8; // 按字节对齐
    
    group->allocation.chunk_bitmap = 
        calloc(1, group->allocation.chunk_bitmap_size);
    
    if (!group->allocation.chunk_bitmap) {
        XSAN_LOG_ERROR("Failed to allocate chunk bitmap");
        return -1;
    }
    
    // 初始化为全部空闲
    group->allocation.free_chunks = group->allocation.chunk_count;
    
    // 初始化分配锁
    pthread_mutex_init(&group->allocation.alloc_lock, NULL);
    
    return 0;
}
```