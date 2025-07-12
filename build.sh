#!/bin/bash
# XSAN 构建脚本
# 用法: ./build.sh [选项]

set -e

# 默认配置
BUILD_TYPE="Release"
BUILD_DIR="build"
JOBS=$(nproc)
VERBOSE=0
CLEAN=0
QUICK=0
DEV=0
TESTS=0
INSTALL=0

# 模块构建选项
BUILD_UTILS=ON
BUILD_COMMON=ON
BUILD_NETWORK=ON
BUILD_STORAGE=ON
BUILD_CLUSTER=ON
BUILD_REPLICATION=ON
BUILD_POLICY=ON
BUILD_VIRTUALIZATION=ON
BUILD_TESTS_OPT=ON
BUILD_TOOLS=ON

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    if [ $VERBOSE -eq 1 ]; then
        echo -e "${BLUE}[DEBUG]${NC} $1"
    fi
}

# 显示帮助信息
show_help() {
    cat << EOF
XSAN 构建脚本

用法: $0 [选项]

通用选项:
    -h, --help              显示此帮助信息
    -v, --verbose           详细输出
    -j, --jobs N            并行编译作业数 (默认: $(nproc))
    -d, --build-dir DIR     构建目录 (默认: build)
    -c, --clean             清理构建目录
    
构建模式:
    --debug                 Debug 模式构建
    --release               Release 模式构建 (默认)
    --quick                 快速构建 (仅核心组件)
    --dev                   开发模式构建 (包含测试和调试工具)
    
构建选项:
    --tests                 构建并运行测试
    --install               安装到系统
    --no-cluster            禁用集群模块
    --no-replication        禁用复制模块
    --no-policy             禁用策略模块
    --no-virtualization     禁用虚拟化模块
    --no-tools              禁用工具模块
    
示例:
    $0                      # 标准构建
    $0 --debug --dev        # 开发模式构建
    $0 --quick              # 快速构建
    $0 --tests              # 构建并运行测试
    $0 --no-virtualization  # 不构建虚拟化模块
    $0 --clean --release    # 清理并重新构建
EOF
}

# 检查依赖
check_dependencies() {
    log_info "检查构建依赖..."
    
    # 检查 CMake
    if ! command -v cmake &> /dev/null; then
        log_error "CMake 未安装，请先安装 CMake"
        exit 1
    fi
    
    # 检查编译器
    if ! command -v gcc &> /dev/null; then
        log_error "GCC 编译器未安装，请先安装 GCC"
        exit 1
    fi
    
    # 检查必要的库
    log_debug "检查 pkg-config..."
    if ! command -v pkg-config &> /dev/null; then
        log_warn "pkg-config 未安装，可能影响库的检测"
    fi
    
    log_info "依赖检查完成"
}

# 清理构建目录
clean_build() {
    if [ -d "$BUILD_DIR" ]; then
        log_info "清理构建目录: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
}

# 创建构建目录
create_build_dir() {
    if [ ! -d "$BUILD_DIR" ]; then
        log_info "创建构建目录: $BUILD_DIR"
        mkdir -p "$BUILD_DIR"
    fi
}

# 配置构建
configure_build() {
    log_info "配置构建 (模式: $BUILD_TYPE)"
    
    cd "$BUILD_DIR"
    
    CMAKE_ARGS=(
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DBUILD_UTILS="$BUILD_UTILS"
        -DBUILD_COMMON="$BUILD_COMMON"
        -DBUILD_NETWORK="$BUILD_NETWORK"
        -DBUILD_STORAGE="$BUILD_STORAGE"
        -DBUILD_CLUSTER="$BUILD_CLUSTER"
        -DBUILD_REPLICATION="$BUILD_REPLICATION"
        -DBUILD_POLICY="$BUILD_POLICY"
        -DBUILD_VIRTUALIZATION="$BUILD_VIRTUALIZATION"
        -DBUILD_TESTS="$BUILD_TESTS_OPT"
        -DBUILD_TOOLS="$BUILD_TOOLS"
    )
    
    if [ $VERBOSE -eq 1 ]; then
        CMAKE_ARGS+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
    fi
    
    log_debug "CMake 参数: ${CMAKE_ARGS[*]}"
    
    cmake .. "${CMAKE_ARGS[@]}"
    
    cd - > /dev/null
}

# 执行构建
build_project() {
    log_info "开始构建 (使用 $JOBS 个并行作业)"
    
    cd "$BUILD_DIR"
    
    if [ $QUICK -eq 1 ]; then
        log_info "执行快速构建"
        make quick -j"$JOBS"
    elif [ $DEV -eq 1 ]; then
        log_info "执行开发模式构建"
        make dev -j"$JOBS"
    else
        log_info "执行完整构建"
        make all -j"$JOBS"
    fi
    
    cd - > /dev/null
}

# 运行测试
run_tests() {
    log_info "运行测试"
    
    cd "$BUILD_DIR"
    
    if [ ! -f "tests/xsan_unit_tests" ]; then
        log_warn "测试程序不存在，可能未启用测试构建"
        return
    fi
    
    make test
    
    cd - > /dev/null
}

# 安装
install_project() {
    log_info "安装到系统"
    
    cd "$BUILD_DIR"
    
    if [ $EUID -eq 0 ]; then
        make install
    else
        log_warn "需要 root 权限进行安装"
        sudo make install
    fi
    
    cd - > /dev/null
}

# 显示构建信息
show_build_info() {
    log_info "构建完成！"
    echo
    echo "构建信息:"
    echo "  构建类型: $BUILD_TYPE"
    echo "  构建目录: $BUILD_DIR"
    echo "  并行作业: $JOBS"
    echo
    echo "生成的文件:"
    if [ -f "$BUILD_DIR/src/main/xsan-node" ]; then
        echo "  - xsan-node (节点守护进程)"
    fi
    if [ -f "$BUILD_DIR/src/main/xsan-cli" ]; then
        echo "  - xsan-cli (命令行工具)"
    fi
    if [ -f "$BUILD_DIR/src/main/xsan-debug" ]; then
        echo "  - xsan-debug (调试工具)"
    fi
    echo
    echo "使用方法:"
    echo "  cd $BUILD_DIR"
    echo "  ./src/main/xsan-node --help"
    echo "  ./src/main/xsan-cli --help"
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -d|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --quick)
            QUICK=1
            shift
            ;;
        --dev)
            DEV=1
            BUILD_TYPE="Debug"
            shift
            ;;
        --tests)
            TESTS=1
            shift
            ;;
        --install)
            INSTALL=1
            shift
            ;;
        --no-cluster)
            BUILD_CLUSTER=OFF
            shift
            ;;
        --no-replication)
            BUILD_REPLICATION=OFF
            shift
            ;;
        --no-policy)
            BUILD_POLICY=OFF
            shift
            ;;
        --no-virtualization)
            BUILD_VIRTUALIZATION=OFF
            shift
            ;;
        --no-tools)
            BUILD_TOOLS=OFF
            shift
            ;;
        *)
            log_error "未知选项: $1"
            echo "使用 --help 查看帮助信息"
            exit 1
            ;;
    esac
done

# 主执行流程
main() {
    log_info "开始构建 XSAN..."
    
    # 检查依赖
    check_dependencies
    
    # 清理构建目录 (如果需要)
    if [ $CLEAN -eq 1 ]; then
        clean_build
    fi
    
    # 创建构建目录
    create_build_dir
    
    # 配置构建
    configure_build
    
    # 执行构建
    build_project
    
    # 运行测试 (如果需要)
    if [ $TESTS -eq 1 ]; then
        run_tests
    fi
    
    # 安装 (如果需要)
    if [ $INSTALL -eq 1 ]; then
        install_project
    fi
    
    # 显示构建信息
    show_build_info
}

# 执行主函数
main "$@"
