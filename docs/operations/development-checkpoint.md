# ESP OTA 开发检查点（2026-09-23）

本轮建立 `components/esp_ota` 独立组件与 C3 样例，迁入 Base 通用 OTA 机制并拆为 `preflight → prepare → select`。`prepare` 不切启动槽；`select` 重验实际槽、完整 signed bin 摘要与 SDK 签名。`eota_observe_slots` 向 Base 收据层提供只读实际槽及镜像状态。Base 业务收据、自检与授权没有搬入本仓。来源文件已对应 Base 公开提交 `10cb8514e8f7a3a55b8ec4622cce4f98a0f90eea`，见[来源记录](../design/source-provenance.md)。

| 验证 | 本轮结果 | 范围 |
| --- | --- | --- |
| `cmake` + `ctest`，AppleClang ASan/UBSan | 3/3 通过 | 编译真实 OTA 与定时器源码；SDK/HTTP/Flash/PSA 假件验证一次调用内的 header/body 慢滴流；本地 socketpair 每 20 毫秒发 1 字节，约 250 毫秒中断并核对清理后文件描述符复用 |
| IDF 普通 C3 样例 | `0x28190` 字节，SHA-256 `f1f0d2d6e2965cf6927b7f1d5a4f0471a17c19669da0654e9f0678ae74421955` | 锁定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`；普通构建不可执行签名 OTA |
| IDF 签名 C3 样例 | `0x31000` 字节，SHA-256 `6c542978d408217e54da848e155e32f50961dc659a7855f72464a542276fe034`；`espsecure verify-signature --version 2` 报 RSA 签名有效 | 同一锁定 SDK；仅使用临时 RSA-3072 测试键，只编译与本机验签，没有设备写入、生产签名或密钥替换 |
| SDK 来源守卫 | 锁定组合通过；原生未修正 lwIP 的 SDK 被拒绝 | 守卫只验证源码组合，不代表网络/设备运行 |

该检查点记录上一版连接后定时器的构建结果；其后已在 `http_transport.c` 改为从 DNS 前启动的单一路径，后续验证结果应在本文件另列。原版 `esp_tls_conn_new_async` 首次调用仍同步 `getaddrinfo`；即使先解析并传数值 IP，`getaddrinfo` 仍可能无期限等待 lwIP 核心线程，因此不能证明全阶段截止。新路径以异步 DNS 与自有非阻塞 socket 避开该等待，直接使用 mBed TLS 公共 API 保持 CA、SNI 和证书名检查。P5-01 的独立公开来源已完成；P5-02 及后续完整验收、真实 HTTPS、签名运行基线、正常/失败升级、回滚、恢复与 Base 硬切仍未完成。人工断电按主计划暂缓。

## P5-04 全阶段传输代码复验（2026-09-23）

| 验证 | 结果 | 实际边界 |
| --- | --- | --- |
| AppleClang ASan/UBSan host CTest | 4/4 通过 | `http_transport.c` 与 `http_deadline.c` 真实源码配本地 socket 和 DNS/TLS 假件；覆盖 DNS 超时后的迟到回调及排队取消、TCP 拒绝、TLS 握手超时、请求写入和响应读取慢滴流、CA/主机名/校验调用、坏证书标志、未执行定时回调前已跨空闲期限及文件描述符复用 |
| 固定 IDF C3 临时 RSA 测试键签名构建 | `0x31000` 字节；SHA-256 `778d2ed2bd8d08ce3bc51876b47f278842c7d2d4a6c07056f6f243261375d0a9`；`espsecure verify-signature --version 2 --keyfile` 报 RSA 签名有效 | `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y`；实际 `eota_prepare` 与 `http_transport` 编入；没有刷板或改生产密钥 |

公开 API 无法安全接管 ESP-TLS 内部已连接 socket：绕过其 `INIT` 状态会漏掉私有 `is_tls` 初始化。官方 SSL transport 即使用数值 IP，`getaddrinfo` 仍通过无期限的 lwIP 核心线程信号量等待。新路径不使用这些入口，但 mBed TLS 单步密码学、缓存记录和 HTTP 解析不能由 OTA 库抢占；host 假 TLS 不能证明真实 CA、SNI 或证书链行为。P5-04 仍待真实 HTTPS 与实板长滴流证据，实板升级、回滚、恢复和 Base 硬切亦未验收。
