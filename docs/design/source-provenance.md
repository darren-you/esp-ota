# 源码来源记录

本仓第一方源码采用 Apache-2.0，原仓作者为 DarrenYou。迁移基线来自 `darren-you/esp-base` 当前工作树，读取时 HEAD 为 `49df899e9f9f8fe6bc5eade1ea2814c02a498bfa`。下列 OTA 通用源码和测试在该 HEAD 上**尚未提交**；因此 HEAD 只是工作树基点，不可冒充含有这些字节的来源提交。这里用原路径与原始文件 SHA-256 冻结迁移快照，待 Base 正式保存后再补精确来源提交。Base 的持久操作收据和业务自检没有迁入。

| 来源路径（esp-base 仓内） | 原始 SHA-256 | 本仓位置与改动 |
| --- | --- | --- |
| `firmware/components/ota_runtime/esp_base_ota.c` | `9ed223a28c318480cffb2b69a107912a871f9f45cedd51a73669e33ce09a4bdc` | `components/esp_ota/src/ota.c`：去掉 Base 稳定窗口策略，仅保留真实槽状态、确认和回滚机制 |
| `firmware/components/ota_runtime/esp_base_ota_update.c` | `d4d3e8e2fbe5f3fb24ccea826334b1b34a1c7fb8fad931eaf0dbb13fe91a131f` | `components/esp_ota/src/update.c`：产品约束参数化，拆成 prepare/select，补运行槽摘要接口和重新读回 |
| `firmware/tests/ota_confirmation_test.c` | `7a8d71474eb61d3a3490e82b76e15e2176095c9927ccadd528fe41ff5051c5a3` | `tests/ota_test.c`：继续编译真实确认机制并注入 SDK 故障 |
| `firmware/tests/ota_update_test.c` | `935cb356ea02be6d6c1b03fe1b93258cc1a63b9c2c42d174037340f8eda5f266` | `tests/update_test.c`：继续编译真实升级机制，验证新双阶段与产品约束 |

底层 HTTP、app_update、分区和密码 API 来自锁定的公开 ESP-IDF；本仓没有复制这些 SDK 实现。样例中固定的 C3 双槽地址只用于保留实验设备的当前布局，不成为库内默认策略。
