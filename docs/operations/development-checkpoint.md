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

公开 API 无法安全接管 ESP-TLS 内部已连接 socket：绕过其 `INIT` 状态会漏掉私有 `is_tls` 初始化。官方 SSL transport 即使用数值 IP，`getaddrinfo` 仍通过无期限的 lwIP 核心线程信号量等待。新路径不使用这些入口，但 mBed TLS 单步密码学、缓存记录和 HTTP 解析不能由 OTA 库抢占；此检查点的 host 假 TLS 尚不能证明真实 CA、SNI 或证书链行为。后续真实 TLS host 回环见下节；实板升级、回滚、恢复和 Base 硬切仍未验收。

## P5-04 真实 TLS host 回环补证（2026-09-23）

固定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 的 mbedTLS 4.1 原生构建，链接同一份 `http_transport.c`。AppleClang ASan/UBSan CTest 5/5 通过，其中真实 HTTPS 回环含六个场景：TLS 1.2 下正确 CA 与 `localhost` SNI/证书名可读完整响应；错误 CA、`wrong.local` 主机名被拒；握手停顿在约 250 毫秒连接截止；响应每 60 毫秒 1 字节，在约 450 毫秒总期限失败。TLS 1.3 成功场景发现 `mbedtls_ssl_read` 会先返回非致命 `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`；传输现继续读取应用数据。测试 CA 在 host 的 `esp_crt_bundle_attach` 适配函数中注入，证书链、主机名及 TLS 记录由真实 mbedTLS 验证。原生库使用 host 默认配置，当前 C3 样例仅启用 TLS 1.2；未执行设备侧 ESP 证书 bundle、真实 DNS/lwIP、`esp_http_client` 解析或升级写槽。

固定 IDF C3 样例以仓外临时 RSA-3072 测试键重新签名构建：镜像 `0x31000` 字节，SHA-256 `4c52fc53ce83c669b49e528aa5d3c8ab0b51da91b51a604987c2b595aaf28de4`；`espsecure verify-signature --version 2 --keyfile` 核验 RSA 签名有效。没有刷板、改分区、eFuse 或生产密钥。P5-04 仍缺设备上的完整 HTTPS、HTTP 解析、Flash 与 bootloader 路径及实板长滴流证据，因此保持未验收。

## 槽选择失败恢复复验（2026-09-23）

固定 IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 的 `app_update/esp_ota_ops.c` 中，`esp_ota_set_boot_partition` 对任意选中槽写入 `ESP_OTA_IMG_NEW`；即使失败后再选回当前正在运行的旧槽，旧槽也会变成 NEW。原库只读回 boot 指针便报告普通切槽失败，下一次预检因运行槽不再是 VALID 而拒绝升级，重启后也会进入新的 pending 窗口。故障假件先复现该失败，再验证恢复时将旧运行槽标回 VALID、清除未启动目标槽的 NEW 记录并重新预检；状态写入或清除不确定时报告 `EOTA_UPDATE_BOOT_STATE_UNKNOWN`。UNTRACKED 运行状态不再冒充 pending 确认成功。

固定 IDF 的 `esp_ota_begin` 会尝试使 inactive 槽原有的 otadata 记录失效；`prepare` 只承诺不选择新启动槽，不承诺 otadata 完全不变。选择失败时还覆盖 boot 未改变但目标槽已留下 NEW 记录的读回路径。IDF 的 `esp_ota_mark_app_valid_cancel_rollback` 在 `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` 下会继续调用 eFuse 更新，因此组件在 IDF 构建中明确拒绝该配置；带该宏的目标源码编译已确认被拒绝。AppleClang ASan/UBSan host CTest 4/4 通过；普通 C3 构建 `0x28190` 字节；仓外临时 RSA-3072 测试键签名 C3 构建 `0x31000` 字节，SHA-256 `12c343d15412c9ee621cb805c6fd4695e833ab92095e23c15362cc6a8f23877c`，本机 `espsecure verify-signature --version 2 --keyfile` 核验通过。未刷板、写设备 otadata、改分区或使用生产凭据；真实 Flash 错误及 bootloader 跨启动仍待实板验证。

## P5-04 固定 SDK 的墙钟边界审计（2026-09-24）

固定公开 SDK `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的源码显示：`esp_http_client_fetch_headers` 与 `esp_http_client_read` 可在一次调用内连续读取；本仓自定义 transport 的 `recv`/`send` 为非阻塞，`select` 每次使用单调时钟计算的剩余绝对期限。但原定时回调在到期后调用 `shutdown`，固定 lwIP 的 `lwip_shutdown → netconn_shutdown → netconn_apimsg → tcpip_send_msg_wait_sem → sys_arch_sem_wait(sem, 0)` 会同步等待 TCP/IP 线程；此处的 `0` 表示无等待期限。固定 SDK 的 `esp_timer_stop_blocking(..., portMAX_DELAY)` 又等待该回调退出。因此 TCP/IP 线程停顿时，原回调本身无法提供墙钟返回上界，还可能阻塞共用的 ESP timer TASK。

在旧实现上先加入回归：35 毫秒到期后检查 socket 仍可由调用方持有；`http_deadline` 1/1 按预期失败，因定时回调将 socket `shutdown` 成 EOF。随后删除回调与 transport close 中的 `shutdown`，移除定时器对文件描述符的所有权；回调仅设置到期原子标志，非阻塞 I/O 和有限 `select` 等待按原有绝对期限结束，HTTP 清理留给同步调用方。新回归、原有 HTTP 故障测试和固定 SDK mbedTLS 4.1 的真实 HTTPS 回环在 AppleClang ASan/UBSan 下 5/5 通过。固定 SDK 的普通 ESP32-C3 样例编译通过，镜像 `0x287d0` 字节，SHA-256 `6024a3b0e2bf1cadc4ad6334161dff243b502a97bd73be006714fcfc4e7aa398`；仓外临时 RSA-3072 测试键的签名 C3 镜像为 `0x31000` 字节，SHA-256 `335a720d7a6495d80895eb4104f52c21168ed464970c0ce336767b429201e43e`，本机 `espsecure verify-signature --version 2` 验证 RSA 签名有效。没有设备写入、eFuse 操作或生产密钥使用。

这项修正只去除定时回调自身的无界 TCP/IP 等待。固定 lwIP 的 `socket` 创建和 `close` 仍可能等待 TCP/IP 线程，mBed TLS 单步、HTTP 解析、Flash 写入和任务调度也不可由 OTA 库抢占。30 秒无进展与 5 分钟总期限因此是正常调度下网络 I/O 的截止检查，不能声称为整个 `eota_prepare` 的严格墙钟返回保证。P5-04 仍须在受控 C3 与 HTTPS 服务上验证 DNS、握手、响应头/体慢滴流、断流、完整镜像及清理读回，并保留设备恢复基线；该阶段未验收。
