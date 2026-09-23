# 测试入口

## 架构拓扑

```mermaid
flowchart LR
    cmake["仓根 CMake / CTest"] --> update["update_test.c：SDK/HTTP/Flash 假件"]
    cmake --> drip["http_deadline_test.c：本地 socket 慢滴流"]
    cmake --> transport["http_transport_test.c：DNS、TCP、TLS 与读写故障"]
    cmake --> confirm["ota_test.c：槽状态假件"]
    update --> real_update["真实 components/esp_ota/src/update.c"]
    drip --> deadline["真实 components/esp_ota/src/http_deadline.c"]
    transport --> real_transport["真实 components/esp_ota/src/http_transport.c"]
    transport --> deadline
    confirm --> real_ota["真实 components/esp_ota/src/ota.c"]
    sdk["固定 ESP-IDF C3 构建"] --> real_update
    sdk --> real_ota
```

从仓根运行 `cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build --output-on-failure`。AppleClang 可增加 `-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` 与相同 linker flags。`update_test` 用假件模拟 SDK 调用内的慢滴流与升级故障；`http_deadline_test` 以本地 socketpair 验证定时中断、晚发布 socket 和回调结束后的文件描述符复用。`http_transport_test` 编译真实 transport 源码，配 DNS/TLS 假件与本地 TCP 服务复现 DNS 迟到回调、连接故障、握手延迟、请求写入和响应读取慢滴流。假 TLS 可以核对配置调用和所有权，不能证明真实 CA/SNI/证书路径；完整 HTTPS、Flash 与 bootloader 仍需单独网络/实板证据。
