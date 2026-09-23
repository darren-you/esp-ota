# ESP OTA 开发检查点（2026-09-23）

本轮建立 `components/esp_ota` 独立组件与 C3 样例，迁入 Base 通用 OTA 机制并拆为 `preflight → prepare → select`。`prepare` 不切启动槽；`select` 重验实际槽、完整 signed bin 摘要与 SDK 签名。`eota_observe_slots` 向 Base 收据层提供只读实际槽及镜像状态。Base 业务收据、自检与授权没有搬入本仓。来源中的 Base 文件仍是未提交工作树，见[来源记录](../design/source-provenance.md)。

| 验证 | 本轮结果 | 范围 |
| --- | --- | --- |
| `cmake` + `ctest`，AppleClang ASan/UBSan | 2/2 通过 | 编译真实 OTA 源码，SDK/HTTP/Flash/PSA 为假件；覆盖槽/项目/芯片/长度/摘要/签名、下载中断、selector 恢复、pending 确认 |
| IDF 普通 C3 样例 | `0x28190` 字节，SHA-256 `4bd6470350d31fca285678d15e048bfb38b051c8cdaa4aaa387b7dae5cf5bc56` | 锁定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；普通构建不可执行签名 OTA |
| IDF 签名 C3 样例 | `0x31000` 字节，SHA-256 `e5e4fc2d9b7f6a1e9554c59dbfabd0f46c8197046ad6ffbd7b0f91fecf9eabbb`；`espsecure verify-signature --version 2` 报 RSA 签名有效 | 同一锁定 SDK；仅使用临时 RSA-3072 测试键，只编译与本机验签，没有设备写入、生产签名或密钥替换 |
| SDK 来源守卫 | 锁定组合通过；原生未修正 lwIP 的 SDK 被拒绝 | 守卫只验证源码组合，不代表网络/设备运行 |

构建在仓外 `/tmp` 目录完成，签名输入与日志均未进入公开仓。`esp_http_client_read` 和 header fetch 的单次调用可能在慢滴流下延长，当前期限仅在返回点核对。真实 HTTPS、慢滴流严格墙钟结束、签名运行基线、正常/失败升级、回滚、恢复与 Base 硬切都没有本轮实板证据，主计划 P5 各完整行尚不能写 ✅。人工断电仍按主计划暂缓。
