# 有界入站固件流检查点

日期：2026-10-06。对应 Base 当前方案 R2／R3 中的独立机制子项；本页只登记软件验证，不代表设备 FRP、公网、实体 Flash、bootloader、双目标容量或正式发布通过。

## 实现

新增 `eota_stream_t`、`eota_validate_stream_request` 与 `eota_prepare_stream`。入站 callback 与原 HTTPS 复用同一个目标头、1024 B 顺序写缓冲、短 Flash claim、完整 signed bin 读回 SHA-256、SDK 签名、精确 signed 长度和 A/B 准备核心；没有整镜像 RAM、第三 Flash 副本、worker、持久收据或 socket 新 owner。准备与选槽仍分离，失败清零输出，活跃写句柄 abort。

每次读取最多 64 B，声明长度后再要求 1 B 并取得真实 framing／FIN 的 EOF。callback 返回 `-2` 可重试，其他负数失败；迟到字节、截断、超长、断流、无 EOF 与超时均不产生准备收据。调用方负责 5 秒建连、授权、协议边界、流数和关闭；库按实际 policy 传递至多 1 秒 I/O 预算，并持续检查 30 秒无进展与 300 秒总期限。库不能抢占违约 callback 或 SDK 单步。

README、API 合同与测试说明同步为固件独立流程，保留 HTTPS 来源和公开共享 HTTPS 机制。历史记录仍保留原事实。

## 软件证据

- `build-stream` 使用 AppleClang 21、ASan／UBSan、`-Wall -Wextra -Werror`；CTest **7／7** 通过。两套更新矩阵分别编译 C3 `chip_id=5`／ESP32 `chip_id=0` 的真实 `update.c`，新增完整输入、提前拒绝、四阶段截断／断流、超长、缺 EOF、目标／摘要／签名／完整长度、重试／迟到／总期限、Flash 获取／释放与 SDK begin／write／读回失败；旧 HTTPS 回归保持通过。
- 固定 SDK `/private/tmp/naming_a_esp_base_sdk_20261002/esp-idf`（源码锁 `578cf89c343e388db43ba1f4ddcd602fedcb763c`）和同目录 `tools`，分别全量构建 `build-c3-stream-signed` 与 `build-esp32-stream-signed`。两目标组件归档都含 `eota_prepare_stream`、`eota_validate_stream_request`、共用 `receive_image`，证明签名配置下新核心实际编译；样例只调用 HTTPS，因此未引用入站 API 的 ELF 节会被链接器移除。
- 仓外输入 `/tmp/eota_inbound_stream_20261006` 只使用无效网络占位和已存在的软件测试键，没有生产凭据或设备访问。C3 RSA-3072／ESP32 ECDSA v1 的官方 `espsecure v5.4.0 verify-signature` 均通过。两板槽容量为 `0x1e0000`，均通过 app 尺寸检查；ESP32 CSV从本轮 Base 新布局只读传入，属于离线构建输入，不证明旧 AT 设备已迁入。
- ESP32 首次默认 bootloader 日志构建因 `0x7c30 > 0x7000` 门失败；按 Base 已有 `CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y` 和 size 优化的离线设置重建通过，没有移动 partition offset、关闭 boot 验签或回滚。

| 目标 | signed bin 字节数 | signed bin SHA-256 |
| --- | ---: | --- |
| C3 | 1,118,208（`0x111000`） | `8afd2a7a7b4ea4c620076586aa5d1c821d1463cece7c64913cd3722e1decbc0e` |
| ESP32 | 1,048,564（`0xffff4`） | `69c527bb4fde9d122b69ecf4b0173b11e707438a8e20478d975857954fa87d76` |

上述制品为软件测试签名，不能发布或刷入当前设备。实体 HMAC／FRP framing、TLS、Flash 时延、32 B上传key副本等消费者成本、持久operation与掉电对账、双板16,384／24,576／1,024 B门仍由 Base／Tool 当前 R2–R6 继续验证。
