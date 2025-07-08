# XSAN 模块化构建指南

## 模块架构

XSAN 采用分层模块化架构，每个模块都有明确的职责和依赖关系：

```
┌─────────────────────────────────────────────────────────────┐
│                        应用层                                │
├─────────────────────────────────────────────────────────────┤
│  xsan-node  │  xsan-cli  │  xsan-debug  │  tools/           │
├─────────────────────────────────────────────────────────────┤
│                        业务逻辑层                            │
├─────────────┬─────────────┬─────────────┬─────────────────────┤
│Virtualization│ Replication │   Policy    │    Cluster         │
├─────────────┴─────────────┴─────────────┴─────────────────────┤
│                        核心引擎层                            │
├─────────────────────────────────────────────────────────────┤
│              Storage Engine       │      Network             │
├─────────────────────────────────────────────────────────────┤
│                        基础设施层                            │
├─────────────────────────────────────────────────────────────┤
│              Common Types         │      Utils               │
└─────────────────────────────────────────────────────────────┘
```

## 模块依赖关系

### 1. 基础设施层 (无依赖)
- **utils**: 日志、内存管理、数据结构等基础工具
- **common**: 通用类型定义、错误处理、常量定义

### 2. 核心引擎层 (依赖基础设施层)
- **network**: 网络通信、RPC、协议处理
  - 依赖: `utils`, `common`
- **storage**: 存储引擎、块管理、设备管理
  - 依赖: `utils`, `common`, `network`

### 3. 业务逻辑层 (依赖核心引擎层)
- **cluster**: 集群管理、节点发现、故障转移
  - 依赖: `utils`, `common`, `network`, `storage`
- **replication**: 数据复制、一致性保证
  - 依赖: `utils`, `common`, `network`, `storage`, `cluster`
- **policy**: 存储策略、QoS 管理
  - 依赖: `utils`, `common`, `storage`
- **virtualization**: KVM/libvirt 集成
  - 依赖: `utils`, `common`, `storage`, `policy`

### 4. 应用层 (依赖所有模块)
- **main**: 主程序 (xsan-node, xsan-cli, xsan-debug)
- **tools**: 管理工具和实用程序

## 构建选项

### 完整构建
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 模块化构建
```bash
# 只构建核心存储功能
cmake .. -DBUILD_CLUSTER=OFF -DBUILD_REPLICATION=OFF -DBUILD_VIRTUALIZATION=OFF

# 只构建网络模块和存储模块
cmake .. -DBUILD_CLUSTER=OFF -DBUILD_REPLICATION=OFF -DBUILD_POLICY=OFF -DBUILD_VIRTUALIZATION=OFF

# 开发模式构建 (包含测试和调试工具)
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
make dev
```

### 快速构建
```bash
# 只构建必要组件
make quick

# 构建并运行测试
make test

# 代码格式化
make format

# 静态分析
make analysis

# 内存检查
make memcheck

# 生成文档
make docs
```

## 编译时优化

### 并行编译
```bash
# 使用所有 CPU 核心
make -j$(nproc)

# 指定核心数
make -j8
```

### 增量编译
- 修改单个模块时，只会重新编译该模块及其依赖的上层模块
- 例如：修改 `storage` 模块时，只会重新编译 `storage`, `cluster`, `replication`, `main`

### 条件编译
```bash
# 禁用特定功能以减少编译时间
cmake .. -DBUILD_REPLICATION=OFF -DBUILD_VIRTUALIZATION=OFF

# 只编译核心功能
cmake .. -DBUILD_CLUSTER=OFF -DBUILD_REPLICATION=OFF -DBUILD_POLICY=OFF -DBUILD_VIRTUALIZATION=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF
```

## 开发工作流

### 1. 新功能开发
1. 确定功能属于哪个模块
2. 修改对应模块的源文件
3. 更新模块的头文件 (如果需要)
4. 运行模块测试: `make test`
5. 运行完整构建: `make all`

### 2. 调试工作流
1. 使用 Debug 模式构建: `cmake .. -DCMAKE_BUILD_TYPE=Debug`
2. 运行内存检查: `make memcheck`
3. 使用调试工具: `./build/src/main/xsan-debug`

### 3. 性能优化
1. 使用 Release 模式构建: `cmake .. -DCMAKE_BUILD_TYPE=Release`
2. 运行性能测试: `./build/tests/xsan_perf_tests`
3. 使用 Profile 模式: `cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo`

## 模块接口设计原则

### 1. 单一职责
- 每个模块只负责一个特定的功能领域
- 模块内部保持高内聚，模块间保持低耦合

### 2. 清晰的接口
- 所有公共接口都在 `include/` 目录下
- 私有接口在 `src/include/` 目录下
- 使用前缀避免命名冲突 (如 `xsan_storage_*`)

### 3. 错误处理
- 统一的错误码定义在 `common` 模块
- 所有函数都有明确的错误返回值
- 使用 `goto` 模式进行资源清理

### 4. 内存管理
- 使用 `utils` 模块的内存管理接口
- 实现引用计数和内存池
- 确保异常安全的资源管理

## 测试策略

### 1. 单元测试
- 每个模块都有对应的单元测试
- 测试覆盖率要求 > 80%
- 使用 CUnit 测试框架

### 2. 集成测试
- 测试模块间的交互
- 模拟真实的使用场景
- 包含故障注入测试

### 3. 性能测试
- 各模块的性能基准测试
- 压力测试和负载测试
- 内存使用和泄漏检测

## 部署和打包

### 1. 静态链接
```bash
cmake .. -DBUILD_SHARED_LIBS=OFF
```

### 2. 动态链接
```bash
cmake .. -DBUILD_SHARED_LIBS=ON
```

### 3. 最小化部署
```bash
# 只构建核心组件
cmake .. -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_VIRTUALIZATION=OFF
```

这种模块化设计的优势：
- **开发效率高**: 开发者可以专注于特定模块
- **编译速度快**: 增量编译，只编译修改的部分
- **维护性好**: 模块间依赖清晰，易于调试
- **可扩展性强**: 新功能可以作为独立模块添加
- **测试友好**: 每个模块可以独立测试
