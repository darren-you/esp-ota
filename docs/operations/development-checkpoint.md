# ESP OTA 开发检查点

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

## P5-04 单调时钟期限收敛（2026-09-24）

复查上节的候选实现后确认：定时回调仅设置 `expired`，不能中断正在执行的 SDK 调用；DNS 等待、非阻塞 socket 的 `select` 和 HTTP 调用前后都已通过 `esp_timer_get_time` 检查同一组绝对期限。该定时器不能提供额外的返回上界，却仍需创建、反复重臂，并在清理时通过 `esp_timer_stop_blocking(portMAX_DELAY)` 等待共用 timer TASK。因此删除定时器与其假件，改由一份期限状态记录 DNS 前的起点和最后一次有效网络进展时刻；进展先检查旧无进展期限，再更新时间。DNS、连接、TLS、HTTP 各阶段和准备阶段都消费该状态，保留有限 `select` 等待及调用方同步清理。

AppleClang ASan/UBSan host CTest 5/5 通过，包括可控时钟对迟到字节、持续慢滴流总期限和时钟异常的检查、DNS 迟到回调与排队取消、TCP/TLS/收发故障、SDK 内慢滴流、固定 SDK mbedTLS 4.1 的真实 HTTPS 回环。固定公开 SDK `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 下普通 ESP32-C3 样例编译通过，镜像 `0x287d0` 字节，SHA-256 `6024a3b0e2bf1cadc4ad6334161dff243b502a97bd73be006714fcfc4e7aa398`；仓外临时 RSA-3072 测试键的签名样例镜像为 `0x31000` 字节，SHA-256 `1fda3da65d4212ecbc677e4e6a331af1a3adfaf431fbde1e97cd0b9180b3db7f`，本机 `espsecure verify-signature --version 2 --keyfile` 验证 RSA 签名有效。没有设备写入、eFuse 操作或生产密钥使用。

这一收敛减少了共用 timer TASK 与清理等待的依赖，但不改变固定 SDK `close`/HTTP/TLS、Flash 与任务调度不可抢占的事实；仍不能给整个 `eota_prepare` 承诺严格的 30 秒无进展或 5 分钟墙钟返回上界。P5-04 保持未验收，实板与完整 HTTPS/Flash/bootloader 链路仍按上节条件验证。

## P5-04 单次 TLS 读写与 Flash 阶段返回检查（2026-09-24）

固定 SDK 的 `esp_http_client_fetch_headers/read` 能在一次方法调用内多次向传输层读取。此前 custom transport 对每次 `select` 都重新使用 SDK 传入的 `read_timeout_ms`：同一 TLS 记录每隔一小段时间收到密文字节时，单次 `mbedtls_ssl_read` 可持续超过这项单次期限。先用可续读的假 TLS 记录和 18 毫秒一字节的本地 socket 复现：65 毫秒读期限下，旧实现的首轮读取未返回 timeout，测试按预期失败。修正后从 transport 单次读写入口建立单调时钟绝对截止，TLS BIO 回调和 `select` 消费同一截止；到期返回可重试读取 timeout，下次调用保留 TLS 记录状态并可继续读完。全局总期限和无进展期限仍优先拒绝逾期字节，已解密但刚超过单次截止的数据不被丢弃。

准备阶段在 `esp_ota_begin/write/end`、HTTP 清理和分区读回返回后检查同一下载期限；固定假件逐项把操作推进到 30 秒无进展期限，均拒绝产出 `prepared`，不执行 `select`。这只能限制 SDK 调用返回后继续推进的行为，不能中断正在运行的 Flash/签名/HTTP/TLS 单步，也不能保证 `esp_ota_abort` 或 `close` 的严格墙钟时长。

| 验证 | 结果 | 边界 |
| --- | --- | --- |
| AppleClang ASan/UBSan + 固定 SDK mbedTLS 4.1 host CTest | 5/5 通过 | 包含慢 TLS 记录的单次读取期限、可续读、Flash 阶段逾期拒绝以及已有 CA、SNI、证书名和响应慢滴流回归 |
| 固定公开 ESP-IDF fork 普通 C3 样例 | `0x28730` 字节，SHA-256 `410d6758f3039c40cb6ee18a0a8462601015dad7628071c64bf642879d5ef6de` | 默认未武装构建；不具备签名升级能力 |
| 同一 SDK 临时 RSA-3072 测试键签名 C3 样例 | 使用仓外无效网络/镜像占位输入令实验分支参与链接；`0x111000` 字节，SHA-256 `788e5bf3c93cfeedda681f06dfef0487b218d51dc1e0e8281b4421ad4d050baf`，`espsecure verify-signature --version 2` 验签通过；ELF 确认 `eota_prepare`、custom transport 与 HTTP 读取均已链接 | 密钥与占位输入仅在仓外临时目录，未执行该镜像、刷板、改分区、eFuse 或使用生产凭据 |

固定 lwIP `socket/close` 等待 TCP/IP 核心线程、mBed TLS 单步和 Flash SDK 调用仍不可抢占，故不把 30 秒或 5 分钟表述为整个 `eota_prepare` 的严格墙钟上界。P5-04 的受控实板 HTTPS、真实 DNS/lwIP、完整镜像下载/Flash/bootloader 与墙钟测量仍待执行；阶段未验收。

## P5-04 槽预检期限起点收紧（2026-09-24）

审计发现 `eota_prepare` 原来先通过 `inspect_slots` 读取运行/目标槽及 otadata，再初始化下载期限。回归将无进展期限设为与总期限相等，用固定假件在首次 `esp_ota_get_state_partition` 读取中推进单调时钟 `300000001` 微秒，旧实现仍开始下载并准备镜像；新增回归因此先按预期失败。现将期限初始化移到本次槽预检之前，预检返回后若期限耗尽即报 `EOTA_UPDATE_DOWNLOAD_FAILED`，不创建 HTTP 客户端、不擦写 Flash，也不产出 `prepared`。调用方单独调用的 `eota_preflight` 仍是另一项操作，不并入本次 `prepare` 期限。

AppleClang ASan/UBSan 与固定 SDK mbedTLS 4.1 主机 CTest 5/5 通过。固定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 C3 样例构建通过；仓外临时 RSA-3072 测试键与无效网络占位输入构建的签名 C3 镜像为 `0x111000` 字节、SHA-256 `2a1a09e45e750c3e868e934df8d7d77a750e898140c8d45f7101f6fc2232053d`，本机 RSA 验签通过，ELF 包含 `eota_prepare`、custom transport 与 `esp_ota_begin`。没有执行镜像或写设备。

这次修改只阻止预检耗时被排除在期限统计外。固定 SDK 的 otadata/Flash 读写与擦除、镜像校验、HTTP/TLS 单步及 lwIP `socket/close` 仍是同步且不可由组件抢占的调用；进入其中任一步后，期限只能在返回时检查，不能强制 `eota_prepare` 在 5 分钟内返回。P5-04 继续未验收。

## P6-10 固件镜像身份只读切片（2026-09-24）

`eota_sha256_verified_image` 只对受控 policy 中精确地址、大小和类型的 OTA app 分区操作。固定 IDF `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 的 `esp_image_verify(ESP_IMAGE_VERIFY)` 在签名更新配置中核对镜像格式、芯片与 RSA 签名，并把含签名块的完整镜像长度写入 `metadata.image_len`；库再按这一长度从真实分区流式计算 SHA-256。`esp_partition_get_sha256` 对 app 返回的是镜像内容附加摘要，不覆盖签名块，不能代替本接口。失败输出清零，不根据分区存在、boot selector 或 `NEW` 状态宣称可启动。

AppleClang ASan/UBSan host CTest 4/4 通过，假件覆盖完整 signed bin 末尾字节、坏镜像、Flash 失败、错误分区几何和失败输出清零。固定 SDK 普通 C3 样例构建通过，镜像 `0x28730` 字节；仓外临时 RSA-3072 测试键的签名 C3 样例构建通过，镜像 `0x31000` 字节，SHA-256 `8ae3a2828272853ac31eaf9f53bf0670e63fe11523e9615673ddd1f040d28167`，本机 `espsecure verify-signature --version 2 --keyfile` 验签通过。未刷设备、改分区或用生产凭据。

本切片只给出**已验签镜像字节身份**。固定 bootloader 在下一次启动会把 `PENDING_VERIFY` 标成 `ABORTED`，而 `NEW` 是尚未经历启动/自检的候选；`esp_ota_get_boot_partition` 自身也不保证镜像有效。Base/Container 尚缺在同一串行所有权下对真实 otadata、boot selector、回退资格及业务包绑定的联合状态转换；当前 Base 也没有独立包分区。因此不能把镜像摘要直接作为 `econtainer_slot_firmware_set_t` 的已证实可启动集合，P6-10/P7-04 仍未验收。

## 选槽阶段重验当前产品目标（2026-09-24）

审计发现 `eota_select` 原来只重验槽几何、完整镜像 SHA-256 与 SDK 签名；同一份已准备的合法签名镜像若在两阶段之间改用另一项目名或芯片 ID 的可信 policy，仍会进入 boot selector。新增故障回归先在旧实现复现选槽，随后将准备阶段的镜像头核对抽为共用函数：选槽前从目标 Flash 读回头与应用描述，按本次 policy 重验项目、芯片、magic 与 SDK 有效性，失败时不写 otadata。短于镜像头的准备记录直接拒绝。

AppleClang ASan/UBSan host CTest 4/4 通过。锁定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 C3 样例编译通过；仓外临时 RSA-3072 测试键签名 C3 样例编译并验签通过，镜像 `0x31000` 字节、SHA-256 `901b3fad3c924f42fe70421656ccca6d0d769ff420814db10996e09e342b3c1e`。以上没有设备写入或真实切槽；P5-04 的实板 HTTPS、Flash 与回滚验收仍未完成。

## P5-04 TLS 1.3 会话票据的单次读取期限（2026-09-24）

固定 SDK 的 mbedTLS 4.1 可让 `mbedtls_ssl_read` 返回 `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`，表示收到非致命的 TLS 1.3 握手后会话票据，尚无应用数据。传输层原先直接重试这个返回值，仅在 `WANT_READ/WANT_WRITE` 分支检查单次读取期限。连续票据因而能让一次 `esp_http_client_read` 的传输调用持续到总期限，而非在本次 `read_timeout_ms` 到达时返回可重试 timeout。

新增本地 socket 与 TLS 假件回归，在握手后每 5 毫秒返回一张会话票据，单次读取期限为 25 毫秒、总期限为 800 毫秒。旧源码的 `http_transport` 测试先在预期 timeout 断言失败（退出码 134）；现于每次 TLS read 前检查同一绝对期限，约 25 毫秒返回 timeout，随后同一连接还能读取服务端应用字节。AppleClang ASan/UBSan host CTest 4/4 通过；加入固定 SDK mbedTLS 4.1 的真实 HTTPS 回环后 CTest 5/5 通过。真实 TLS 1.3 回环仍只证明普通会话票据与响应；连续票据的期限故障由直接编译生产 transport 的假件覆盖。

锁定公开 ESP-IDF fork `855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 的普通 ESP32-C3 样例构建通过，镜像 `0x287d0` 字节、SHA-256 `220825e0d07ae870fc38d6a8ba38d97f5ce7e51ac621815f4bf106151977004e`。仓外临时 RSA-3072 测试键与无效网络占位输入的签名 C3 构建通过，镜像 `0x111000` 字节、SHA-256 `a1ac24d0e9fc21bfc5ba043277ea916e4eb25c9b2c2e30f9c9677599ddc7faaa`；本机 RSA 验签成功，ELF map 包含 `eota_prepare`、`transport_read` 与官方 `esp_http_client_read`。构建未执行镜像、写设备或使用生产凭据。

这项修正约束的是 TLS 每次返回后的再次调用。固定 SDK 单次密码学调用、HTTP 解析、Flash 和 `close` 仍不可抢占，且真实设备 HTTPS/Flash/bootloader 链路未测；P5-04 保持未验收。

## 公开 SDK HTTP 初始化失败所有权修正锁定复验（2026-09-24）

当前 `sdk-lock.json` 将 ESP-IDF 精确固定到公开 fork `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP 仍为 `2758df4cd3666b3b2a5b53830148379326425c0d`。新 SDK 修复了 `esp_http_client_init` 在内建 TCP／TLS transport 创建成功、列表登记失败时遗留尚未转交的句柄；OTA 的 custom transport 也会经过 SDK 的内建 transport 初始化。独立 SDK C3 QEMU 回归在修正前定点令 HTTP 列表登记返回 `ESP_ERR_NO_MEM`，连续八次失败使 8-bit 可用堆减少 1792 字节；修正后 HTTP 与 HTTPS 两条失败路径各八次的堆差均为零。该故障注入只验证初始化所有权，不模拟真实 TLS/Flash 故障。

| 本仓验证 | 结果 | 边界 |
| --- | --- | --- |
| `check_sdk.py` 与 AppleClang ASan/UBSan host CTest | 锁定源码核对通过；含固定 SDK mbedTLS 4.1 真实 HTTPS 回环的 CTest 5/5 通过 | host 假件与本机 TLS 服务，不代表设备网络栈或 bootloader |
| 固定 SDK 普通 ESP32-C3 样例 | `0x288d0` 字节，SHA-256 `0d56ab726753083deb0ed8e7b5677c791ab54fb63f056421063260c8ee414e53` | 默认未武装构建，不能执行签名升级 |
| 仓外临时 RSA-3072 测试键、无效网络占位输入的签名 C3 样例 | `0x111000` 字节，SHA-256 `9c95b690d0265507cb03f7c4b994f7a54952d5700793ce90af3f4d16a63da0a3`；`espsecure verify-signature --version 2 --keyfile` 验签成功，ELF／map 确认 `eota_prepare`、`transport_read`、官方 HTTP 读取和 `esp_ota_begin` 均已链接 | 只编译与本机验签；没有执行镜像、刷板或使用生产凭据 |

复验还发现样例的实验输入若恰放在构建目录根的 `lab_inputs.h`，多轮 CMake 配置会将它覆盖为默认空输入，导致表面上签名构建成功、实际 OTA 准备代码未进入 ELF。现在顶层 CMake 在配置前拒绝构建目录内输入；故障复现用例验证拒绝时原输入 SHA-256 不变，构建目录外的占位输入仍可完成签名构建。历史检查点的 SHA 均保留其当次验证事实。本次没有设备 HTTPS、Flash、切槽或回滚运行证据，P5-04 继续未验收。

## C3 与 ESP32 双目标离线验证（2026-09-26）

固定 SDK `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 通过 `check_sdk.py`。ESP32 的官方 chip ID 是 `0x0000`，旧 policy 检查会误拒；现在只拒绝 SDK 的 `ESP_CHIP_ID_INVALID` (`0xffff`)。同一真实 `update.c` 故障矩阵分别以 C3 `0x0005`、ESP32 `0x0000` 编译并运行，AppleClang ASan/UBSan host CTest **5/5** 通过。仓外错配配置的独立编译探针也确认 C3/ECDSA、ESP32/RSA 均令 `eota_available=false`。这组 host 测试未启用真实 mbedTLS HTTPS 回环。

首次 ESP32 签名装配暴露旧组件只接受 RSA 宏；固定 SDK 的 ESP32 默认签名应用配置实际选用 ECDSA v1，拿 C3 的 RSA-3072 测试键签 ESP32 分区表时被拒绝。组件现按 C3/RSA-3072 与 ESP32/ECDSA v1 分别检查目标签名配置，保持 `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`、证书 bundle、自定义 HTTPS transport、rollback 及 anti-rollback 禁用条件。固定 SDK 的 `esp_image_verify(ESP_IMAGE_VERIFY)` 在 ECDSA v1 下使用 bootloader 内嵌公钥验证签名，并将签名块计入 `metadata.image_len`；组件继续按此完整长度做摘要身份检查。host 假件不能代替设备端的这个 SDK 验证调用。

| 离线验证 | 镜像字节数 | SHA-256 | 装配检查 |
| --- | ---: | --- | --- |
| C3 默认未武装 | 164240 (`0x28190`) | `c15094bf548c1931fe9da974a4d714a9cb90f0deb61c77c41b2a718a1999f85a` | `CONFIG_IDF_TARGET="esp32c3"`；通用源码在 `examples/common/main.c` |
| ESP32 默认未武装 | 156848 (`0x264b0`) | `b46331e8ac3820b6c3bf7cb1cf654ded2df03e011cbd459e1f95851ff449d8bd` | `CONFIG_IDF_TARGET="esp32"`；默认官方通用双 OTA 表只用于编译 |
| C3 仓外临时 RSA-3072 测试键签名 | 1118208 (`0x111000`) | `e2472beac900288fa325056edec185e84e9e1c7a03d2186d159641ca92de6598` | `espsecure verify-signature --version 2 --keyfile` 通过；RSA、signed update、custom transport 已启用 |
| ESP32 仓外临时 ECDSA P-256 测试键签名 | 1048564 (`0xffff4`) | `2ea7094b3b310f65ff89a56496fd06beaed81ce7a1fe46d88f9ce555b5e297d4` | `espsecure verify-signature --version 1 --keyfile` 通过；ECDSA v1、signed update、custom transport 已启用；ELF 包含 `eota_prepare`、`eota_select` |

签名构建只使用仓外测试键、无效网络占位输入以及独立 build/sdkconfig。C3 使用已确认的现有 C3 样例分区；ESP32 的仓外 CSV 依据两份逐字节一致的 4 MiB Flash 只读备份：分区表位于 `0x8000`，`phy_init@0xf000/0x1000`、`otadata@0x10000/0x2000`、`nvs@0x12000/0xe000`、`at_customize` type `0x40`/subtype `0x00` `@0x20000/0xe0000`、`ota_0@0x100000/0x180000`、`ota_1@0x280000/0x180000`。固定 SDK 的官方分区解析器读回生成表与上述几何一致。该表只作为当前旧板的**离线编译输入**，不代表新 Container 目标布局。

ESP32 当前 `otadata` 两扇区全 `0xff`，`ota_0` 是 2017 年 AT 固件，`ota_1` 全 `0xff`；它没有与本次临时 ECDSA 测试键匹配的签名运行与回退基线。离线签名、编译、镜像摘要和 host 假件不能证明现有 bootloader 可启动本次镜像，也不证明 OTA 下载、Flash 写入、运行时验签、确认或回滚。没有连接或写入两台设备，没有烧 eFuse、改生产密钥或替换分区。P5-04 至 P5-06 及 Base/Container 接入的设备验收仍待按五仓主计划执行。

## 准备失败后旧收据失效（2026-09-26）

审查发现 `eota_prepare` 对无效 URL 或尚未建立可信时间的请求会在清零 `prepared` 之前返回。若调用方复用上一次成功准备的输出对象，这次失败会留下旧收据；旧源码在“先成功准备，再提交 HTTP URL”回归中确定性违反收据清零断言。现于函数入口清零非空输出，包括未启用签名 OTA 的分支；失败请求之后的 `eota_select` 因无有效收据而拒绝切槽。调用方仍必须检查返回值，不能把清零当作授权或并发保护。

本机 AppleClang ASan/UBSan 直接编译同一份 `update.c` 故障矩阵，C3 `chip_id=5` 与 ESP32 `chip_id=0` 均通过；本机独立 `http_transport` ASan/UBSan 测试通过。`mac-work-1` 的固定 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 下，两目标默认未武装样例构建通过，镜像分别为 C3 `0x28190`、ESP32 `0x264b0` 字节；它们不运行签名升级。该机启用真实 mbedTLS 回环的初轮 CTest 共 6 项，其中 `update`、`update_esp32`、`http_deadline`、`confirmation` 通过；`http_transport` 和 `real_https` 的毫秒级时序断言未通过，普通与 ASan/UBSan 构建均复现。后两项只编译传输/期限源码，不包含本次修改的 `update.c`；初轮不能声称完整 host suite 通过，后续调查和修正见下节。没有设备 HTTPS、Flash 或 bootloader 验证；P5 各项验收状态不变。

## 主机时序测试与真实期限对照（2026-09-26）

在相同固定 SDK 下，父提交 `5da4a0d` 和收据修复提交 `2072731` 的普通与 ASan/UBSan 构建均复现上述两项失败。父提交仓外诊断版记录：慢写假件一次计划 15 毫秒的 `nanosleep` 实耗 192 毫秒，超过原 170 毫秒总期限；真实 HTTPS 测试服务计划每 60 毫秒发送一字节，实际相邻发送间隔曾为 110、171、195 毫秒，超过原 180 毫秒空闲期限。另一次真实 HTTPS 客户端在总期限 450 毫秒、空闲期限 180 毫秒时于 497 毫秒失败，读回单调时钟显示两项期限均已到期。原断言把未受控的宿主调度当作固定输入，不能据此判定产品 transport 超时错误。

只修改测试：慢写、分片 TLS 记录及连续会话票据使用受控单调时钟步进，仍编译真实 transport 并检查单次/全阶段绝对截止；本地 socket 的空闲等待和可续读结果另由实际时钟用例覆盖。真实 mbedTLS HTTPS 回环继续使用真实 TLS 与 socket，慢滴流改为总期限与空闲期限同为 450 毫秒，明确只检验总期限；独立 `http_deadline` 和 transport 无进展用例继续检验空闲期限。慢握手另要求客户端在服务端主动关闭前失败，测试进程设 10 秒防挂起上限。没有修改 `http_transport.c`、`http_deadline.c` 或任何产品期限配置。

把这组**仅测试文件**修正分别套在父提交和当前提交后，`mac-work-1` 的完整 CTest 在普通与 AppleClang ASan/UBSan 两种构建均为 **6/6 通过**；本机直接编译的 transport ASan/UBSan 用例也通过。测试修正证明先前两项失败属于测试对宿主调度的错误假设；它不证明整个 `eota_prepare` 的严格墙钟上界，也不替代设备上的 DNS、HTTPS、Flash、签名与 bootloader 验收。P5-04、P5-05、P5-06 继续未验收。
