# eota API 合同

`eota_` 是第一方机制 API；`esp_ota_*` 仍属于 ESP-IDF。库不消费网络命令中的 target 作为信任来源，产品约束必须由受控应用装配。`eota_policy_t` 指定非空项目名、芯片 ID、两个 OTA 槽的地址与共同大小、下载期限及已建立可信时间的事实。应用先用 `eota_available` 拒绝普通未签名构建。

1. 应用核对请求授权、当前业务状态、网络与可信时间；用 `eota_preflight(policy, size, slots)` 核对可写安全条件并取得真实运行/目标槽、大小和状态。`eota_observe_slots` 是只读事实接口，即使 boot selector 与运行槽不一致也报告实际槽和镜像状态，供跨启动结果查询。应用在首次目标槽写入前持久提交操作收据并精确读回。
2. 应用在唯一专用 worker 调用 `eota_prepare`。URL 只接受 HTTPS；HTTP 必须是 200、确定且一致的 Content-Length、非 chunked、不重定向。镜像头项目、芯片和 SDK 有效性核对后才调用 `esp_ota_begin`。完整 signed bin 写入后从目标槽读回，核对 SHA-256，再由 `esp_ota_end` 验签。返回 `eota_prepared_t` 时 boot selector 仍指向原槽。
3. 应用持久提交与业务包的兼容绑定，再调用 `eota_select`。它重新核对运行/目标槽、目标整镜像摘要，并调用 SDK 的 `esp_ota_set_boot_partition` 再验签。选择读回与可回退事实不成立时尝试恢复旧槽；恢复读回不明明确返回 `EOTA_UPDATE_BOOT_STATE_UNKNOWN`，应用不得自动重启。
4. 应用决定重启。新启动的本地自检及稳定窗口通过后才调用 `eota_confirm_pending`；明确失败时调用 `eota_reject_pending`，该 SDK 调用可能直接重启。`eota_inspect` 和 `eota_sha256_running` 让应用把持久收据与当前实际运行镜像关联，最终业务结果由应用裁决。

调用方在同步操作期间持有 URL、policy、进度上下文与唯一 worker，不并发释放或重复写槽。进度回调只在 `eota_prepare` 调用栈内使用，不能重入 `eota_` 或阻塞业务。产品操作 ID、同 ID 去重、NVS 命名空间、配置事务、USB/MQTT/FRP 协议、Container 包槽均不进入本库。

IDF v6.1 的 `esp_http_client_read` 为填满一次请求长度会循环底层读取，header fetch 也可在持续滴流下长期不返回。连接完成并取得当前 socket 后，库在 header/body 阶段将最早的无进展/总期限交给一次性 ESP 定时器；到期回调对该 socket 调用 `shutdown`，SDK 返回后先由 `esp_timer_stop_blocking` 等待回调退出，再清理 HTTP 句柄。host 本地 socket 慢滴流测试证实了此阶段的中断和清理顺序。DNS 解析、首次 `open`、TLS 握手与请求发送仍可能在取得 socket 前阻塞；证书主机名的真实 HTTPS 链路也没有实测。因此接口目前不承诺整个 `eota_prepare` 的严格墙钟上界，主计划 P5-04 未验收。
