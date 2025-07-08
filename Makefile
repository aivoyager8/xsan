# XSAN 项目 Makefile 包装器
# 这个 Makefile 是一个便捷的包装器，用于简化常见的构建操作

# 默认配置
BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)

# 检查操作系统
ifeq ($(OS),Windows_NT)
    SHELL := cmd
    BUILD_CMD := build.bat
    RM := rmdir /s /q
    MKDIR := mkdir
else
    BUILD_CMD := ./build.sh
    RM := rm -rf
    MKDIR := mkdir -p
endif

# 默认目标
.PHONY: all
all: release

# 构建目标
.PHONY: release debug quick dev
release:
	@echo "构建 Release 版本..."
	$(BUILD_CMD) --release

debug:
	@echo "构建 Debug 版本..."
	$(BUILD_CMD) --debug

quick:
	@echo "快速构建..."
	$(BUILD_CMD) --quick

dev:
	@echo "开发模式构建..."
	$(BUILD_CMD) --dev

# 测试目标
.PHONY: test test-unit test-integration test-perf
test:
	@echo "运行所有测试..."
	$(BUILD_CMD) --tests

test-unit:
	@echo "运行单元测试..."
	@cd $(BUILD_DIR) && ./tests/xsan_unit_tests

test-integration:
	@echo "运行集成测试..."
	@cd $(BUILD_DIR) && ./tests/xsan_integration_tests

test-perf:
	@echo "运行性能测试..."
	@cd $(BUILD_DIR) && ./tests/xsan_perf_tests

# 清理目标
.PHONY: clean distclean
clean:
	@echo "清理构建目录..."
	$(BUILD_CMD) --clean

distclean:
	@echo "完全清理..."
	$(RM) $(BUILD_DIR)

# 代码质量目标
.PHONY: format lint analyze
format:
	@echo "格式化代码..."
	@cd $(BUILD_DIR) && make format

lint:
	@echo "运行代码检查..."
	@cd $(BUILD_DIR) && make analysis

analyze: lint

# 内存检查
.PHONY: memcheck valgrind
memcheck:
	@echo "运行内存检查..."
	@cd $(BUILD_DIR) && make memcheck

valgrind: memcheck

# 覆盖率检查
.PHONY: coverage
coverage:
	@echo "生成覆盖率报告..."
	@cd $(BUILD_DIR) && make coverage

# 文档生成
.PHONY: docs
docs:
	@echo "生成文档..."
	@cd $(BUILD_DIR) && make docs

# 安装目标
.PHONY: install uninstall
install:
	@echo "安装到系统..."
	$(BUILD_CMD) --install

uninstall:
	@echo "从系统卸载..."
	@echo "手动删除已安装的文件"

# 模块化构建目标
.PHONY: utils common network storage cluster replication policy virtualization
utils:
	@echo "构建 utils 模块..."
	$(BUILD_CMD) --no-common --no-network --no-storage --no-cluster --no-replication --no-policy --no-virtualization --no-tools

common:
	@echo "构建 common 模块..."
	$(BUILD_CMD) --no-network --no-storage --no-cluster --no-replication --no-policy --no-virtualization --no-tools

network:
	@echo "构建 network 模块..."
	$(BUILD_CMD) --no-storage --no-cluster --no-replication --no-policy --no-virtualization --no-tools

storage:
	@echo "构建 storage 模块..."
	$(BUILD_CMD) --no-cluster --no-replication --no-policy --no-virtualization --no-tools

cluster:
	@echo "构建 cluster 模块..."
	$(BUILD_CMD) --no-replication --no-policy --no-virtualization --no-tools

replication:
	@echo "构建 replication 模块..."
	$(BUILD_CMD) --no-policy --no-virtualization --no-tools

policy:
	@echo "构建 policy 模块..."
	$(BUILD_CMD) --no-cluster --no-replication --no-virtualization --no-tools

virtualization:
	@echo "构建 virtualization 模块..."
	$(BUILD_CMD) --no-cluster --no-replication --no-tools

# 开发环境设置
.PHONY: setup-dev setup-env
setup-dev:
	@echo "设置开发环境..."
	@echo "安装 Git hooks..."
	@cp scripts/pre-commit .git/hooks/
	@chmod +x .git/hooks/pre-commit
	@echo "创建开发配置..."
	@$(MKDIR) $(BUILD_DIR)
	@echo "开发环境设置完成"

setup-env: setup-dev

# 构建信息
.PHONY: info
info:
	@echo "=== XSAN 构建信息 ==="
	@echo "构建目录: $(BUILD_DIR)"
	@echo "构建类型: $(BUILD_TYPE)"
	@echo "并行作业: $(JOBS)"
	@echo "操作系统: $(if $(OS),Windows,Linux/Unix)"
	@echo "======================"

# 帮助信息
.PHONY: help
help:
	@echo "XSAN 构建系统 - 可用目标:"
	@echo ""
	@echo "构建目标:"
	@echo "  all         - 构建所有组件 (默认: release)"
	@echo "  release     - Release 模式构建"
	@echo "  debug       - Debug 模式构建"
	@echo "  quick       - 快速构建 (仅核心组件)"
	@echo "  dev         - 开发模式构建"
	@echo ""
	@echo "测试目标:"
	@echo "  test        - 运行所有测试"
	@echo "  test-unit   - 运行单元测试"
	@echo "  test-integration - 运行集成测试"
	@echo "  test-perf   - 运行性能测试"
	@echo ""
	@echo "清理目标:"
	@echo "  clean       - 清理构建目录"
	@echo "  distclean   - 完全清理"
	@echo ""
	@echo "代码质量:"
	@echo "  format      - 格式化代码"
	@echo "  lint        - 代码检查"
	@echo "  analyze     - 静态分析"
	@echo "  memcheck    - 内存检查"
	@echo "  coverage    - 覆盖率报告"
	@echo ""
	@echo "文档和安装:"
	@echo "  docs        - 生成文档"
	@echo "  install     - 安装到系统"
	@echo "  uninstall   - 从系统卸载"
	@echo ""
	@echo "模块化构建:"
	@echo "  utils       - 仅构建 utils 模块"
	@echo "  common      - 构建 common 模块"
	@echo "  network     - 构建 network 模块"
	@echo "  storage     - 构建 storage 模块"
	@echo "  cluster     - 构建 cluster 模块"
	@echo "  replication - 构建 replication 模块"
	@echo "  policy      - 构建 policy 模块"
	@echo "  virtualization - 构建 virtualization 模块"
	@echo ""
	@echo "开发环境:"
	@echo "  setup-dev   - 设置开发环境"
	@echo "  info        - 显示构建信息"
	@echo "  help        - 显示此帮助信息"
	@echo ""
	@echo "示例:"
	@echo "  make debug        # Debug 模式构建"
	@echo "  make test         # 构建并运行测试"
	@echo "  make clean all    # 清理后重新构建"
	@echo "  make storage      # 只构建存储模块"
