# 测试入口

## 架构拓扑

```mermaid
flowchart LR
    cmake["仓根 CMake / CTest"] --> update["update_test.c：SDK/HTTP/Flash 假件"]
    cmake --> confirm["ota_test.c：槽状态假件"]
    update --> real_update["真实 components/esp_ota/src/update.c"]
    confirm --> real_ota["真实 components/esp_ota/src/ota.c"]
    sdk["固定 ESP-IDF C3 构建"] --> real_update
    sdk --> real_ota
```

从仓根运行 `cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build --output-on-failure`。AppleClang 可增加 `-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` 与相同 linker flags。假件只模拟调用层结果；真实 TLS、Flash、bootloader 和慢速滴流必须由独立网络/实板测试确认，当前未完成。
