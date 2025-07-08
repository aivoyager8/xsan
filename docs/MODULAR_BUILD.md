# XSAN 项目模块化构建系统

## 概述

XSAN 分布式存储系统采用了完全模块化的构建架构，支持增量编译、条件编译和分布式开发。这个文档总结了整个模块化构建系统的设计和使用方法。

## 模块架构图

```
                    ┌─────────────────────────────────────────┐
                    │                应用层                  │
                    │  ┌─────────┬─────────┬─────────┬─────────┐ │
                    │  │xsan-node│xsan-cli │xsan-debug│ tools  │ │
                    │  └─────────┴─────────┴─────────┴─────────┘ │
                    └─────────────────────────────────────────┘
                                        │
                    ┌─────────────────────────────────────────┐
                    │              业务逻辑层                │
                    │ ┌─────────┬─────────┬─────────┬─────────┐ │
                    │ │Virtual. │Replica. │ Policy  │ Cluster │ │
                    │ └─────────┴─────────┴─────────┴─────────┘ │
                    └─────────────────────────────────────────┘
                                        │
                    ┌─────────────────────────────────────────┐
                    │               核心引擎层               │
                    │      ┌─────────┬─────────────────────┐   │
                    │      │ Network │     Storage        │   │
                    │      └─────────┴─────────────────────┘   │
                    └─────────────────────────────────────────┘
                                        │
                    ┌─────────────────────────────────────────┐
                    │              基础设施层                │
                    │      ┌─────────┬─────────────────────┐   │
                    │      │ Common  │       Utils        │   │
                    │      └─────────┴─────────────────────┘   │
                    └─────────────────────────────────────────┘
```

## 模块详细说明

### 1. Utils 模块 (基础工具)
- **位置**: `src/utils/`
- **依赖**: 无
- **功能**: 日志、内存管理、数据结构、线程池等基础工具
- **编译产物**: `libxsan_utils.a`

### 2. Common 模块 (通用类型)
- **位置**: `src/common/`
- **依赖**: Utils
- **功能**: 错误处理、类型定义、常量、版本信息
- **编译产物**: `libxsan_common.a`

### 3. Network 模块 (网络通信)
- **位置**: `src/network/`
- **依赖**: Utils, Common
- **功能**: Socket 通信、事件循环、RPC、协议处理
- **编译产物**: `libxsan_network.a`

### 4. Storage 模块 (存储引擎)
- **位置**: `src/storage/`
- **依赖**: Utils, Common, Network
- **功能**: 块管理、元数据、设备管理、I/O 引擎
- **编译产物**: `libxsan_storage.a`

### 5. Cluster 模块 (集群管理)
- **位置**: `src/cluster/`
- **依赖**: Utils, Common, Network, Storage
- **功能**: 节点发现、心跳、选举、故障转移
- **编译产物**: `libxsan_cluster.a`

### 6. Replication 模块 (数据复制)
- **位置**: `src/replication/`
- **依赖**: Utils, Common, Network, Storage, Cluster
- **功能**: 数据复制、一致性保证、恢复
- **编译产物**: `libxsan_replication.a`

### 7. Policy 模块 (存储策略)
- **位置**: `src/policy/`
- **依赖**: Utils, Common, Storage
- **功能**: 存储策略、QoS 管理、分层存储
- **编译产物**: `libxsan_policy.a`

### 8. Virtualization 模块 (虚拟化集成)
- **位置**: `src/virtualization/`
- **依赖**: Utils, Common, Storage, Policy
- **功能**: libvirt 集成、虚拟机存储管理
- **编译产物**: `libxsan_virtualization.a`

## 构建系统特性

### 1. 模块化编译
- 每个模块独立编译为静态库
- 支持条件编译，可以选择性构建模块
- 增量编译，只编译修改的模块

### 2. 依赖管理
- 清晰的模块依赖关系
- 自动解决依赖顺序
- 循环依赖检测

### 3. 构建选项
```cmake
# 模块构建选项
-DBUILD_UTILS=ON/OFF
-DBUILD_COMMON=ON/OFF
-DBUILD_NETWORK=ON/OFF
-DBUILD_STORAGE=ON/OFF
-DBUILD_CLUSTER=ON/OFF
-DBUILD_REPLICATION=ON/OFF
-DBUILD_POLICY=ON/OFF
-DBUILD_VIRTUALIZATION=ON/OFF
-DBUILD_TESTS=ON/OFF
-DBUILD_TOOLS=ON/OFF
```

### 4. 构建目标
- **quick**: 快速构建，仅构建核心组件
- **dev**: 开发构建，包含测试和调试工具
- **all**: 完整构建，包含所有组件

## 使用方法

### 1. 完整构建
```bash
# Linux/Unix
make all
# 或
./build.sh

# Windows
build.bat
# 或
make all
```

### 2. 模块化构建
```bash
# 只构建存储相关模块
cmake .. -DBUILD_CLUSTER=OFF -DBUILD_REPLICATION=OFF -DBUILD_VIRTUALIZATION=OFF

# 只构建网络和存储模块
cmake .. -DBUILD_CLUSTER=OFF -DBUILD_REPLICATION=OFF -DBUILD_POLICY=OFF -DBUILD_VIRTUALIZATION=OFF
```

### 3. 开发模式
```bash
# 开发构建
make dev

# 运行测试
make test

# 代码检查
make lint

# 内存检查
make memcheck

# 生成文档
make docs
```

### 4. 快速构建
```bash
# 只构建必要组件
make quick

# 或使用脚本
./build.sh --quick
```

## 编译优化

### 1. 并行编译
```bash
# 使用所有 CPU 核心
make -j$(nproc)

# 指定核心数
make -j8
```

### 2. 增量编译
- 修改单个模块时，CMake 会自动检测依赖关系
- 只重新编译修改的模块及其依赖的上层模块
- 大大减少开发时的编译时间

### 3. 条件编译
```bash
# 最小化构建 (只有存储核心功能)
cmake .. \
    -DBUILD_CLUSTER=OFF \
    -DBUILD_REPLICATION=OFF \
    -DBUILD_POLICY=OFF \
    -DBUILD_VIRTUALIZATION=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_TOOLS=OFF

# 服务器版本 (无虚拟化支持)
cmake .. -DBUILD_VIRTUALIZATION=OFF
```

## 开发工作流

### 1. 新功能开发
1. 确定功能属于哪个模块
2. 修改对应模块的源文件
3. 更新模块的头文件接口
4. 编译测试: `make <module>`
5. 运行单元测试: `make test-unit`
6. 完整构建: `make all`

### 2. 调试工作流
1. Debug 模式构建: `make debug`
2. 运行调试工具: `./build/src/main/xsan-debug`
3. 内存检查: `make memcheck`
4. 性能分析: `make test-perf`

### 3. 代码质量保证
1. 代码格式化: `make format`
2. 静态分析: `make analyze`
3. 单元测试: `make test-unit`
4. 集成测试: `make test-integration`
5. 覆盖率检查: `make coverage`

## 构建脚本

### 1. Linux/Unix 构建脚本 (`build.sh`)
```bash
./build.sh --help                    # 查看帮助
./build.sh --release                 # Release 构建
./build.sh --debug --dev             # 开发构建
./build.sh --quick                   # 快速构建
./build.sh --no-virtualization       # 禁用虚拟化模块
./build.sh --tests                   # 构建并运行测试
./build.sh --clean --release         # 清理后重新构建
```

### 2. Windows 构建脚本 (`build.bat`)
```cmd
build.bat /help                      # 查看帮助
build.bat /release                   # Release 构建
build.bat /debug /dev                # 开发构建
build.bat /quick                     # 快速构建
build.bat /no-virtualization         # 禁用虚拟化模块
build.bat /tests                     # 构建并运行测试
build.bat /clean /release            # 清理后重新构建
```

### 3. Makefile 包装器
```bash
make help                            # 查看所有可用目标
make release                         # Release 构建
make debug                           # Debug 构建
make quick                           # 快速构建
make test                            # 运行测试
make clean                           # 清理构建
make storage                         # 只构建存储模块
make setup-dev                       # 设置开发环境
```

## 性能和优化

### 1. 编译时间优化
- **模块化编译**: 只编译修改的模块
- **并行编译**: 利用多核 CPU
- **预编译头**: 减少头文件解析时间
- **条件编译**: 只编译需要的功能

### 2. 内存使用优化
- **静态链接**: 减少运行时依赖
- **符号剥离**: 减少二进制文件大小
- **链接时优化**: 消除未使用的代码

### 3. 开发体验优化
- **增量编译**: 快速的修改-编译-测试循环
- **单元测试**: 快速验证模块功能
- **自动化工具**: 格式化、分析、检查

## 部署支持

### 1. 最小化部署
```bash
# 只构建运行时必需的组件
cmake .. \
    -DBUILD_TESTS=OFF \
    -DBUILD_TOOLS=OFF \
    -DCMAKE_BUILD_TYPE=Release
```

### 2. 开发部署
```bash
# 包含调试和开发工具
cmake .. \
    -DBUILD_TESTS=ON \
    -DBUILD_TOOLS=ON \
    -DCMAKE_BUILD_TYPE=Debug
```

### 3. 生产部署
```bash
# 优化的生产版本
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF
```

## 扩展和维护

### 1. 添加新模块
1. 创建模块目录 `src/new_module/`
2. 编写模块的 `CMakeLists.txt`
3. 在主 `CMakeLists.txt` 中添加模块
4. 更新模块依赖关系
5. 添加相应的测试

### 2. 修改模块依赖
1. 更新模块的 `CMakeLists.txt`
2. 修改 `target_link_libraries`
3. 确保没有循环依赖
4. 更新文档

### 3. 性能调优
1. 使用 `make test-perf` 进行性能测试
2. 分析编译时间和内存使用
3. 优化模块间的接口
4. 减少不必要的依赖

这个模块化构建系统为 XSAN 项目提供了：
- **快速开发**: 增量编译和模块化开发
- **灵活部署**: 条件编译和最小化构建
- **质量保证**: 自动化测试和代码检查
- **易于维护**: 清晰的模块结构和依赖关系
