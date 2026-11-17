# Linux 构建助手

桌面模拟输入无法进入 VMware guest 时，使用此助手执行项目的 Linux 检查。

在 Linux 虚拟机中的项目根目录启动：

```bash
python3 tests/linux_verify.py
```

项目可放在任意目录，不依赖原电脑的共享挂载路径。

看到 `QtChat build helper ready` 后保留终端，随时可用 Ctrl+C 停止。它不监听网络端口、不修改系统设置、不安装软件，也不注册自动启动。

助手在 `tests/results/linux-helper` 写入实际环境、状态、命令日志和退出码。请求动作仅有 env、build、test、build_test、stop；运行 CMake/CTest 时不使用 shell。只处理本项目，构建目录固定为 `build/qtchat-cmake` 或 `build/qtchat-sanitize`。构建和测试会执行当前项目代码，与手动执行这些命令相同，因此仅在这个受信任的共享项目中使用。

本机可执行的请求边界及退出码测试：`python3 tests/linux_helper_test.py`。这不代替在 Linux 中实际构建。
