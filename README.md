# QtChat

基于 C++17、Qt6 Widgets 和 Linux TCP/epoll 的即时通信项目，支持注册登录、好友申请与删除、一对一文字聊天、SQLite 历史与未读、在线状态、心跳超时以及断线重连。

**完整客户端与服务端运行环境：Linux。** 服务端使用 Linux 专属的 epoll/eventfd，不能直接在 Windows 原生运行。推荐复用原开发环境：Ubuntu 24.04 x86_64、GCC 13.3、CMake 3.28、Qt 6.12.0（gcc_64）。下载的是源码，需要在目标电脑重新编译。

## 下载与依赖

```bash
git clone https://github.com/ZHONGM666/Qtchat.git
cd Qtchat
```

也可以在 GitHub 点击 **Code → Download ZIP**，解压后在项目根目录打开终端。路径不依赖原电脑的用户名、共享目录或盘符。

系统需要 C++17 编译器、CMake >= 3.16、Qt6 Widgets/Network、SQLite3、OpenSSL Crypto 和 Boost 开发头文件。启用测试时另需 Qt Test，Python3 用于集成测试与压测。

Ubuntu 上缺少依赖时安装：

```bash
sudo apt update
sudo apt install build-essential cmake git pkg-config libsqlite3-dev libssl-dev libboost-dev python3
```

Qt 有两种配置方式：已有 Qt6 的电脑直接使用现有安装；没有 Qt6 时，可使用 Ubuntu 提供的开发包：

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools
```

原环境使用 Qt 6.12.0；Ubuntu 软件源中的 Qt 版本由系统版本决定。采用软件源版本时，须在该电脑完成下述构建和测试，不能直接将原环境的验证结果视为该版本的验证结果。

第三方源码已随仓库提供，不需要另外下载 JSON 库、线程池或 muduo。上游版本、SHA256 和许可证见 [third_party/README.md](third_party/README.md) 和 [manifest.json](third_party/manifest.json)。Boost、Qt、SQLite、OpenSSL 仍需由系统提供。

## 构建

在项目根目录执行。使用系统 Qt6 或已经配置好 CMAKE_PREFIX_PATH 时：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DQTCHAT_BUILD_CLIENT=ON -DBUILD_TESTING=OFF
cmake --build build/release -j2
```

如果 Qt6 由 Qt 安装器安装在自定义位置，配置时加上实际 Qt 前缀。下面沿用原环境的 Qt 版本，安装位置不同时替换路径：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64" \
  -DQTCHAT_BUILD_CLIENT=ON -DBUILD_TESTING=OFF
cmake --build build/release -j2
```

也可以用 **Linux Qt Creator 打开根目录 CMakeLists.txt**，选择与目标电脑对应的 Qt6 gcc_64 套件，构建 `qtchat_server` 和 `qtchat_client`。本仓库统一使用 CMake，不提供旧的、不完整的 Chat.pro/qmake 构建入口。

只构建服务端（不需要 Qt）：

```bash
cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release -DQTCHAT_BUILD_CLIENT=OFF -DBUILD_TESTING=OFF
cmake --build build/server -j2
```

## 运行

第一个终端启动服务端：

```bash
./build/release/qtchat_server
```

第二个终端启动客户端：

```bash
./build/release/qtchat_client
```

默认连接 `127.0.0.1:9527`。没有预置账号，请分别注册两个账号，再登录、查找对方用户名、发送好友申请并接受，即可聊天。用户名为 3–32 位 ASCII 字母、数字或下划线，密码为 8–128 UTF-8 字节。可多次启动客户端验证通信。

数据库首次启动时自动创建，无需拷贝原电脑的数据库或手工建表。默认位置为 `$XDG_DATA_HOME/qtchat/qtchat.db`，未设置 XDG_DATA_HOME 时为 `~/.local/share/qtchat/qtchat.db`。可以显式指定：

```bash
./build/release/qtchat_server --host 127.0.0.1 --port 9527 \
  --db "$HOME/.local/share/qtchat/qtchat.db" --workers 2 --heartbeat-timeout 60
```

源码可放共享目录，数据库建议放 Linux 本地文件系统。默认本地数据库使用 WAL/FULL，显式指定 /mnt/hgfs/ 路径时使用 rollback journal。备份时先停止服务端，再复制数据库。

两台电脑在同一局域网使用时，服务端可通过 `--host 0.0.0.0 --port 9527` 监听网络；客户端登录页的“服务器设置”填写服务端电脑的局域网 IPv4 地址和端口，服务端防火墙须允许该端口。默认回环地址只供同一电脑使用。当前使用普通 TCP，不含 TLS 加密，适用于学习和受控网络演示。

Enter 发送，Shift+Enter 换行，单条文字最多 8 KiB。消息确认表示服务器已保存，已读状态单独维护。此版本不包含群聊、文件传输、音视频。

## 验证

建议在新电脑上单独创建测试构建目录；自定义 Qt 安装同样需要传入 CMAKE_PREFIX_PATH：

```bash
cmake -S . -B build/test -DCMAKE_BUILD_TYPE=Debug -DQTCHAT_BUILD_CLIENT=ON -DBUILD_TESTING=ON
cmake --build build/test -j2
ctest --test-dir build/test --output-on-failure -E 'client_visual_smoke|benchmark_suite'
```

这组命令验证协议分帧、心跳与线程池、数据库、业务、Qt 网络和界面状态、真实服务端集成等。Qt 界面测试使用 offscreen；有 Linux X11 桌面时可额外运行 `ctest --test-dir build/test -R '^client_visual_smoke$' --output-on-failure`。性能测试不在日常测试中自动执行。

只验证服务端可将 QTCHAT_BUILD_CLIENT 设为 OFF。Python 工具仅使用标准库。测试会创建独立临时数据库，结果写入 tests/results/，不携带原电脑的测试结果。

## 目录与第三方组件

| 路径 | 用途 |
| --- | --- |
| main.cpp、mainwindow.h/.cpp | 客户端入口和主窗口 |
| client/ | 登录、聊天界面、网络请求、Qt 资源与样式 |
| common/ | 长度前缀分帧和心跳超时管理 |
| server/ | Linux epoll Reactor、SQLite、密码处理和聊天业务 |
| third_party/ | 固定版本的第三方头文件、移植参考与许可证 |
| tests/ | 单元测试、集成测试、构建助手和压测工具 |
| docs/protocol.md | 通信协议说明 |

线程池直接使用 **BS::thread_pool v5.1.0**（MIT），服务端 JSON 使用 **nlohmann/json v3.12.0**（MIT）；心跳超时管理移植自 **muduo v2.0.3 idleconnection 示例**（BSD），使用系统 Boost circular_buffer。第三方文件和移植代码保留原有版权归属。

仓库只包含编译、测试和理解项目所需的源码、资源及说明，不包含 build/、本地数据库、个人 Qt Creator 配置、测试截图或日志。新电脑应使用自己的编译器和 Qt 套件，重新生成构建目录。
