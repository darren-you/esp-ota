# ESP OTA 开发检查点（2026-09-23）

本轮建立 `components/esp_ota` 独立组件与 C3 样例，迁入 Base 通用 OTA 机制并拆为 `preflight → prepare → select`。`prepare` 不切启动槽；`select` 重验实际槽、完整 signed bin 摘要与 SDK 签名。`eota_observe_slots` 向 Base 收据层提供只读实际槽及镜像状态。Base 业务收据、自检与授权没有搬入本仓。来源文件已对应 Base 公开提交 `10cb8514e8f7a3a55b8ec4622cce4f98a0f90eea`，见[来源记录](../design/source-provenance.md)。

| 验证 | 本轮结果 | 范围 |
| --- | --- | --- |
| `cmake` + `ctest`，AppleClang ASan/UBSan | 3/3 通过 | 编译真实 OTA 与定时器源码；SDK/HTTP/Flash/PSA 假件验证一次调用内的 header/body 慢滴流；本地 socketpair 每 20 毫秒发 1 字节，约 250 毫秒中断并核对清理后文件描述符复用 |
| IDF 普通 C3 样例 | `0x28190` 字节，SHA-256 `f1f0d2d6e2965cf6927b7f1d5a4f0471a17c19669da0654e9f0678ae74421955` | 锁定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；普通构建不可执行签名 OTA |
| IDF 签名 C3 样例 | `0x31000` 字节，SHA-256 `6c542978d408217e54da848e155e32f50961dc659a7855f72464a542276fe034`；`espsecure verify-signature --version 2` 报 RSA 签名有效 | 同一锁定 SDK；仅使用临时 RSA-3072 测试键，只编译与本机验签，没有设备写入、生产签名或密钥替换 |
| SDK 来源守卫 | 锁定组合通过；原生未修正 lwIP 的 SDK 被拒绝 | 守卫只验证源码组合，不代表网络/设备运行 |

构建在仓外 `/tmp` 目录完成，签名输入与日志均未进入公开仓。锁定 IDF 的 `esp_http_client_get_socket` 和 `esp_timer_stop_blocking` 提供连接后中断与回调同步；本地 socket 测试不代表 HTTPS。IDF 的 `esp_tls_conn_new_async` 首次调用仍同步 `getaddrinfo`；公开的 lwIP `dns_gethostbyname` 可异步取得 IP，但无取消接口，且当前 C3 配置未启用 TCP/IP 核心锁。IDF HTTP 配置中的 `common_name` 和显式 Host header 有独立入口，本轮只做源码核对，没有以数值 IP 完成真实 HTTPS 主机名验证与发送链路。DNS、首次建连/发送、严格全阶段墙钟结束、签名运行基线、正常/失败升级、回滚、恢复与 Base 硬切未验收；主计划 P5 各完整行尚不能写 ✅。人工断电仍按主计划暂缓。
