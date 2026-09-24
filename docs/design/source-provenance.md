# 源码来源记录

本仓第一方源码采用 Apache-2.0，原仓作者为 DarrenYou。迁移基线是 `esp-space/esp-base` 的公开 `master` 提交 `10cb8514e8f7a3a55b8ec4622cce4f98a0f90eea`。下列四个来源文件已按该提交的原路径与 SHA-256 逐一核对；表中 SHA-256 指文件内容摘要，源码迁入后的修改见右栏。Base 的持久操作收据和业务自检没有迁入。

| 来源路径（esp-base 仓内） | 原始 SHA-256 | 本仓位置与改动 |
| --- | --- | --- |
| `firmware/components/ota_runtime/esp_base_ota.c` | `9ed223a28c318480cffb2b69a107912a871f9f45cedd51a73669e33ce09a4bdc` | `components/esp_ota/src/ota.c`：去掉 Base 稳定窗口策略，仅保留真实槽状态、确认和回滚机制 |
| `firmware/components/ota_runtime/esp_base_ota_update.c` | `d4d3e8e2fbe5f3fb24ccea826334b1b34a1c7fb8fad931eaf0dbb13fe91a131f` | `components/esp_ota/src/update.c`：产品约束参数化，拆成 prepare/select，补运行槽摘要接口和重新读回 |
| `firmware/tests/ota_confirmation_test.c` | `7a8d71474eb61d3a3490e82b76e15e2176095c9927ccadd528fe41ff5051c5a3` | `tests/ota_test.c`：继续编译真实确认机制并注入 SDK 故障 |
| `firmware/tests/ota_update_test.c` | `935cb356ea02be6d6c1b03fe1b93258cc1a63b9c2c42d174037340f8eda5f266` | `tests/update_test.c`：继续编译真实升级机制，验证新双阶段与产品约束 |

底层 HTTP、app_update、分区和密码 API 来自锁定的[公开 ESP-IDF fork](https://github.com/esp-space/esp-idf) `578cf89c343e388db43ba1f4ddcd602fedcb763c`；其官方父提交为 `fff9895c82d744c7237be8847347bdd1b07c6643`，当前两项源码修正分别在 OTA 擦除失败时释放句柄，以及 HTTP 客户端初始化的内建 TCP／TLS transport 注册失败时释放未交给列表的句柄。本仓没有复制这些 SDK 实现。样例中固定的 C3 双槽地址只用于保留实验设备的当前布局，不成为库内默认策略。
