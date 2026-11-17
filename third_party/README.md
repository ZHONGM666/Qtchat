# 第三方来源与适配

固定上游版本、完整 commit、原始文件 SHA256 见 manifest.json。上游文件不作修改，许可证与文件一同交付。

- **BS::thread_pool v5.1.0**，Barak Shoshany，MIT；业务工作池与单线程数据库执行器直接调用它，不自行实现线程池。
- **nlohmann/json v3.12.0**，MIT；仅用于 C++ 服务端 JSON 解析。
- **muduo v2.0.3**，Shuo Chen，BSD；原始 `examples/idleconnection/echo.h` 和 `echo.cc` 保存在 muduo 子目录。

## muduo 超时管理移植

`common/idle_timeout.h/.cpp` 复用原示例的以下代码和机制：

1. Bucket 持有 shared_ptr<Entry>，Entry 持有弱连接引用，最后一个 Bucket 引用释放时通过析构回收连接。
2. onConnection 在最后一个 Bucket 放入新 Entry，连接保存弱 Entry 引用。
3. onMessage 续期时将同一 Entry 放入最后一个 Bucket。
4. onTimer 通过 `boost::circular_buffer` 的 `push_back(Bucket())` 淘汰旧 Bucket。

适配仅涉及连接与事件循环接口：用 opaque Watch 和关闭回调替代 TcpConnection，用外部单调时钟调用 advance 替代 EventLoop::runEvery；取消与 owner 析构时禁用关闭回调。长暂停时最多推进一整轮，因为一整轮后所有旧活动记录均已到期。未使用自写堆、扫描表或新的时间轮算法。

编译保留原例的 Boost circular_buffer，使用系统 Boost 开发头文件。库自己的 TCP Server/EventLoop、日志和示例 echo 回显不引入本项目。

QtChat 的 Linux Socket/epoll Reactor、协议与业务为项目实现；线程池与心跳超时管理必须按以上来源归属描述，不能宣称全部自行编写。
