# 独立 OTA 样例 RAM Wi-Fi 检查点

日期：2026-10-04。

原 `bf11916ab904be4ee9bcdfae213c85336363e96a` 的双目标共用样例未选择 Wi-Fi 存储方式；固定 SDK 默认 `WIFI_STORAGE_FLASH`，启用 Wi-Fi NVS。样例现于 Wi-Fi 初始化成功后、配置 STA 及启动前显式调用 `esp_wifi_set_storage(WIFI_STORAGE_RAM)`。失败沿现有 `connect_network()` 的 bool 合同返回 `false`，调用方终止本次实验。保持 NVS 错误返回，不自动擦除既有数据；OTA 库、公开 API、双目标配置和分区没有改变。

## 实际软件验证

SDK 固定为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，lwIP 保持原精确锁。仓外仅使用无效网络占位输入、既有独立软件测试签名键，以及独立 build/sdkconfig。两目标均启用原样例 armed 编译路径；普通构建仍由 `eota_available` 拒绝升级。

| 目标 | 普通镜像 | 完整 signed bin | 原样例 app 槽余量 | 官方签名验证 |
| --- | ---: | ---: | ---: | --- |
| ESP32-C3 | 839,104 B | 1,118,208 B | 847,872 B | RSA v2 通过 |
| ESP32 | 790,160 B | 1,048,564 B | 524,300 B | ECDSA v1 通过 |

四条官方 SDK 构建实际退出 0，两个 signed ELF 均实际编译新 `main.c`（SHA-256 `e2f483e6e74472ed47022d07c16b260ac4cd68ddf15ed45fb737a974b10a6563`）。`connect_network` 反汇编核对参数 1 调用 `esp_wifi_set_storage`，随后错误分支返回失败；两个 ELF 均包含 `eota_prepare`、`eota_select`、`eota_confirm_pending`。ESP32 生成分区表的 ECDSA v1 验签也通过。

ESP32 仓外 CSV 继续使用此前读回的旧 AT 几何，仅供离线编译；没有改变设备表或建立签名运行／回退基线。C3 继续原独立样例 `0x1e0000` 槽，ESP32 继续旧 AT `0x180000` 槽；此余量与 Base 正式布局的容量门不同。

首次 C3 普通编译因本机磁盘 ENOSPC 退出 2，原日志保留；空间恢复后同源重试四构建及验签成功。没有把环境失败记为产品回归，也没有用默认空输入的优化结果证明 RAM 调用。

## 验收边界

没有执行设备、真实 Wi-Fi／HTTPS、Flash／otadata、回滚、断电、eFuse、生产发布或凭据修改；未测动态 RAM。软件构建和验签不能证明实板 NVS／网络行为或 P5-04 至 P5-07 总验收。此变更只影响独立样例，Base 的运行库精确 pin 保持 `bf11916a`。
