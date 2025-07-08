@echo off
:: XSAN 构建脚本 (Windows 版本)
:: 用法: build.bat [选项]

setlocal enabledelayedexpansion

:: 默认配置
set BUILD_TYPE=Release
set BUILD_DIR=build
set JOBS=4
set VERBOSE=0
set CLEAN=0
set QUICK=0
set DEV=0
set TESTS=0
set INSTALL=0

:: 模块构建选项
set BUILD_UTILS=ON
set BUILD_COMMON=ON
set BUILD_NETWORK=ON
set BUILD_STORAGE=ON
set BUILD_CLUSTER=ON
set BUILD_REPLICATION=ON
set BUILD_POLICY=ON
set BUILD_VIRTUALIZATION=ON
set BUILD_TESTS_OPT=ON
set BUILD_TOOLS=ON

:: 日志函数
:log_info
echo [INFO] %~1
goto :eof

:log_warn
echo [WARN] %~1
goto :eof

:log_error
echo [ERROR] %~1
goto :eof

:log_debug
if %VERBOSE%==1 (
    echo [DEBUG] %~1
)
goto :eof

:: 显示帮助信息
:show_help
echo XSAN 构建脚本 (Windows 版本)
echo.
echo 用法: %~nx0 [选项]
echo.
echo 通用选项:
echo     /h, /help              显示此帮助信息
echo     /v, /verbose           详细输出
echo     /j:N                   并行编译作业数 (默认: 4)
echo     /d:DIR                 构建目录 (默认: build)
echo     /c, /clean             清理构建目录
echo.
echo 构建模式:
echo     /debug                 Debug 模式构建
echo     /release               Release 模式构建 (默认)
echo     /quick                 快速构建 (仅核心组件)
echo     /dev                   开发模式构建 (包含测试和调试工具)
echo.
echo 构建选项:
echo     /tests                 构建并运行测试
echo     /install               安装到系统
echo     /no-cluster            禁用集群模块
echo     /no-replication        禁用复制模块
echo     /no-policy             禁用策略模块
echo     /no-virtualization     禁用虚拟化模块
echo     /no-tools              禁用工具模块
echo.
echo 示例:
echo     %~nx0                  # 标准构建
echo     %~nx0 /debug /dev      # 开发模式构建
echo     %~nx0 /quick           # 快速构建
echo     %~nx0 /tests           # 构建并运行测试
goto :eof

:: 检查依赖
:check_dependencies
call :log_info "检查构建依赖..."

:: 检查 CMake
cmake --version >nul 2>&1
if errorlevel 1 (
    call :log_error "CMake 未安装，请先安装 CMake"
    exit /b 1
)

:: 检查编译器 (通过 CMake 检测)
call :log_info "依赖检查完成"
goto :eof

:: 清理构建目录
:clean_build
if exist "%BUILD_DIR%" (
    call :log_info "清理构建目录: %BUILD_DIR%"
    rmdir /s /q "%BUILD_DIR%"
)
goto :eof

:: 创建构建目录
:create_build_dir
if not exist "%BUILD_DIR%" (
    call :log_info "创建构建目录: %BUILD_DIR%"
    mkdir "%BUILD_DIR%"
)
goto :eof

:: 配置构建
:configure_build
call :log_info "配置构建 (模式: %BUILD_TYPE%)"

pushd "%BUILD_DIR%"

set CMAKE_ARGS=-DCMAKE_BUILD_TYPE=%BUILD_TYPE%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_UTILS=%BUILD_UTILS%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_COMMON=%BUILD_COMMON%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_NETWORK=%BUILD_NETWORK%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_STORAGE=%BUILD_STORAGE%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_CLUSTER=%BUILD_CLUSTER%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_REPLICATION=%BUILD_REPLICATION%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_POLICY=%BUILD_POLICY%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_VIRTUALIZATION=%BUILD_VIRTUALIZATION%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_TESTS=%BUILD_TESTS_OPT%
set CMAKE_ARGS=%CMAKE_ARGS% -DBUILD_TOOLS=%BUILD_TOOLS%

if %VERBOSE%==1 (
    set CMAKE_ARGS=%CMAKE_ARGS% -DCMAKE_VERBOSE_MAKEFILE=ON
)

call :log_debug "CMake 参数: %CMAKE_ARGS%"

cmake .. %CMAKE_ARGS%
if errorlevel 1 (
    call :log_error "CMake 配置失败"
    popd
    exit /b 1
)

popd
goto :eof

:: 执行构建
:build_project
call :log_info "开始构建 (使用 %JOBS% 个并行作业)"

pushd "%BUILD_DIR%"

if %QUICK%==1 (
    call :log_info "执行快速构建"
    cmake --build . --target quick --parallel %JOBS%
) else if %DEV%==1 (
    call :log_info "执行开发模式构建"
    cmake --build . --target dev --parallel %JOBS%
) else (
    call :log_info "执行完整构建"
    cmake --build . --parallel %JOBS%
)

if errorlevel 1 (
    call :log_error "构建失败"
    popd
    exit /b 1
)

popd
goto :eof

:: 运行测试
:run_tests
call :log_info "运行测试"

pushd "%BUILD_DIR%"

if not exist "tests\xsan_unit_tests.exe" (
    call :log_warn "测试程序不存在，可能未启用测试构建"
    popd
    goto :eof
)

cmake --build . --target test

popd
goto :eof

:: 安装
:install_project
call :log_info "安装到系统"

pushd "%BUILD_DIR%"

cmake --build . --target install

popd
goto :eof

:: 显示构建信息
:show_build_info
call :log_info "构建完成！"
echo.
echo 构建信息:
echo   构建类型: %BUILD_TYPE%
echo   构建目录: %BUILD_DIR%
echo   并行作业: %JOBS%
echo.
echo 生成的文件:
if exist "%BUILD_DIR%\src\main\xsan-node.exe" (
    echo   - xsan-node.exe (节点守护进程)
)
if exist "%BUILD_DIR%\src\main\xsan-cli.exe" (
    echo   - xsan-cli.exe (命令行工具)
)
if exist "%BUILD_DIR%\src\main\xsan-debug.exe" (
    echo   - xsan-debug.exe (调试工具)
)
echo.
echo 使用方法:
echo   cd %BUILD_DIR%
echo   src\main\xsan-node.exe --help
echo   src\main\xsan-cli.exe --help
goto :eof

:: 解析命令行参数
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="/h" goto :help_and_exit
if /i "%~1"=="/help" goto :help_and_exit
if /i "%~1"=="/v" (
    set VERBOSE=1
    shift
    goto :parse_args
)
if /i "%~1"=="/verbose" (
    set VERBOSE=1
    shift
    goto :parse_args
)
if /i "%~1"=="/c" (
    set CLEAN=1
    shift
    goto :parse_args
)
if /i "%~1"=="/clean" (
    set CLEAN=1
    shift
    goto :parse_args
)
if /i "%~1"=="/debug" (
    set BUILD_TYPE=Debug
    shift
    goto :parse_args
)
if /i "%~1"=="/release" (
    set BUILD_TYPE=Release
    shift
    goto :parse_args
)
if /i "%~1"=="/quick" (
    set QUICK=1
    shift
    goto :parse_args
)
if /i "%~1"=="/dev" (
    set DEV=1
    set BUILD_TYPE=Debug
    shift
    goto :parse_args
)
if /i "%~1"=="/tests" (
    set TESTS=1
    shift
    goto :parse_args
)
if /i "%~1"=="/install" (
    set INSTALL=1
    shift
    goto :parse_args
)
if /i "%~1"=="/no-cluster" (
    set BUILD_CLUSTER=OFF
    shift
    goto :parse_args
)
if /i "%~1"=="/no-replication" (
    set BUILD_REPLICATION=OFF
    shift
    goto :parse_args
)
if /i "%~1"=="/no-policy" (
    set BUILD_POLICY=OFF
    shift
    goto :parse_args
)
if /i "%~1"=="/no-virtualization" (
    set BUILD_VIRTUALIZATION=OFF
    shift
    goto :parse_args
)
if /i "%~1"=="/no-tools" (
    set BUILD_TOOLS=OFF
    shift
    goto :parse_args
)
if "%~1:~0,3%"=="/j:" (
    set JOBS=%~1:~3%
    shift
    goto :parse_args
)
if "%~1:~0,3%"=="/d:" (
    set BUILD_DIR=%~1:~3%
    shift
    goto :parse_args
)

call :log_error "未知选项: %~1"
echo 使用 /help 查看帮助信息
exit /b 1

:help_and_exit
call :show_help
exit /b 0

:args_done
goto :eof

:: 主执行流程
:main
call :log_info "开始构建 XSAN..."

:: 检查依赖
call :check_dependencies
if errorlevel 1 exit /b 1

:: 清理构建目录 (如果需要)
if %CLEAN%==1 (
    call :clean_build
)

:: 创建构建目录
call :create_build_dir

:: 配置构建
call :configure_build
if errorlevel 1 exit /b 1

:: 执行构建
call :build_project
if errorlevel 1 exit /b 1

:: 运行测试 (如果需要)
if %TESTS%==1 (
    call :run_tests
)

:: 安装 (如果需要)
if %INSTALL%==1 (
    call :install_project
)

:: 显示构建信息
call :show_build_info
goto :eof

:: 解析参数并执行主函数
call :parse_args %*
call :main
