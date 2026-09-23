# eota API 合同

`eota_` 是第一方机制 API；`esp_ota_*` 仍属于 ESP-IDF。库不消费网络命令中的 target 作为信任来源，产品约束必须由受控应用装配。`eota_policy_t` 指定非空项目名、芯片 ID、两个 OTA 槽的地址与共同大小、下载期限及已建立可信时间的事实。应用先用 `eota_available` 拒绝普通未签名构建。

1. 应用核对请求授权、当前业务状态、网络与可信时间；用 `eota_preflight(policy, size, slots)` 核对可写安全条件并取得真实运行/目标槽、大小和状态。`eota_observe_slots` 是只读事实接口，即使 boot selector 与运行槽不一致也报告实际槽和镜像状态，供跨启动结果查询。应用在首次目标槽写入前持久提交操作收据并精确读回。
2. 应用在唯一专用 worker 调用 `eota_prepare`。URL 只接受 HTTPS；HTTP 必须是 200、确定且一致的 Content-Length、非 chunked、不重定向。镜像头项目、芯片和 SDK 有效性核对后才调用 `esp_ota_begin`。完整 signed bin 写入后从目标槽读回，核对 SHA-256，再由 `esp_ota_end` 验签。返回 `eota_prepared_t` 时 boot selector 仍指向原槽。
3. 应用持久提交与业务包的兼容绑定，再调用 `eota_select`。它重新核对运行/目标槽、目标整镜像摘要，并调用 SDK 的 `esp_ota_set_boot_partition` 再验签。选择读回与可回退事实不成立时尝试恢复旧槽；恢复读回不明明确返回 `EOTA_UPDATE_BOOT_STATE_UNKNOWN`，应用不得自动重启。
4. 应用决定重启。新启动的本地自检及稳定窗口通过后才调用 `eota_confirm_pending`；明确失败时调用 `eota_reject_pending`，该 SDK 调用可能直接重启。`eota_inspect` 和 `eota_sha256_running` 让应用把持久收据与当前实际运行镜像关联，最终业务结果由应用裁决。

调用方在同步操作期间持有 URL、policy、进度上下文与唯一 worker，不并发释放或重复写槽。进度回调只在 `eota_prepare` 调用栈内使用，不能重入 `eota_` 或阻塞业务。产品操作 ID、同 ID 去重、NVS 命名空间、配置事务、USB/MQTT/FRP 协议、Container 包槽均不进入本库。

下载总期限与无进展期限以 ESP 定时器在 SDK 调用**返回后**检查。IDF v6.1 的 `esp_http_client_read` 会为填满一次请求长度而循环底层读取，header fetch 也可能在持续滴流下长期不返回；因此当前接口不承诺严格墙钟结束。独立网络测试须先证实或修正这一行为，才能关闭主计划 P5-04。
