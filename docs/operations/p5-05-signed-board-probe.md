# P5-05 双板签名启动与回退窄探针（2026-09-26）

本轮在两块 4 MiB 实板各自原有双 OTA 分区中，验证临时测试键签名应用的软件验签、选槽、确认和拒绝回退。两板共用同一探针主源码，SHA-256 为 `13d1b38e670b95b69450b121cba316253eabd451136401a619d16d446a4b7313`；分别使用锁定的 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 和 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 构建。RSA-3072 与 ECDSA v1 测试键留在仓外，未使用生产密钥；Secure Boot 与 Flash Encryption 均未启用，没有烧写 eFuse。实验完成后两板已恢复原完整 Flash，因此本记录是机制证据，**P5-05 持续签名运行/回退基线及 P6-03 均未验收**。

| 目标 | 原有应用槽 | 测试签名镜像 | 实板机制结果 |
| --- | --- | --- | --- |
| ESP32-C3 | `ota_0@0x20000/0x1e0000`，`ota_1@0x200000/0x1e0000` | RSA-3072；A/B 各 266,240 B | 两槽应用侧 SDK 验签通过；A 选 B、B pending 确认 valid；B 再选 A、A pending 拒绝后回到 B valid |
| ESP32-D0WD-V3 | `ota_0@0x100000/0x180000`，`ota_1@0x280000/0x180000` | ECDSA v1；A/B 各 262,132 B | 两槽应用侧 `esp_image_verify=ESP_OK`，SDK v1 验签返回 0；完成相同确认与回退链，otadata 最终为 A invalid、B valid |

ESP32 原 AT 分区表没有 MD5。探针 bootloader 使用匹配配置且长度 23,504 B，位于旧 `0x1000..0x8000` 空间内；两块板均保留原分区表。ESP32 构建启用无硬件 Secure Boot 的软件签名启动配置，bootloader 日志级别为 NONE；实板可观察证据是已签名应用启动、应用侧 SDK 验签和 otadata 回退。不能从静默 bootloader 日志单独推断它执行过哪一次验签。

## 写入与恢复证据

- C3 写前两份完整 4 MiB 备份逐字节一致，并与该板 P1-04 冻结件一致；重新枚举的芯片、端点、MAC、容量及原 Base UUID、revision 5、配置状态均已核对。只写探针 bootloader 与两个应用槽，三段写入校验及独立读回通过。两轮串口日志记录两个槽的验签、pending 确认和拒绝回退，无 panic/WDT。
- C3 实验后全片读回在预期镜像区与 otadata 之外，NVS 另有 **5,922 字节**变化，分布于三个扇区。探针应用未初始化 NVS；现有中间读回不足以区分原 Base 的异步写入与试验副作用，原因保持未知。立即全片恢复，独立 4 MiB 读回与写前两份及 P1-04 冻结件逐字节一致；原 Base UUID、revision 5、Wi-Fi=disconnected、config=ready 和管理状态均与写前稳定值一致。不能将 C3 的实际变更范围记为仅三个镜像区和 otadata。
- ESP32 写前重新核对该板 CH340 端点、芯片、MAC、4 MiB、eFuse 只读状态及旧 AT 版本/配置。首对全片备份一致后，旧 AT 重启在 NVS 中又改写 117 字节；于是保持下载模式重新取得最新两份完整 4 MiB 备份，逐字节一致，作为恢复基线。仅写匹配旧表的 bootloader 与两个签名应用；三个区域独立读回一致，初始 otadata 仍全擦除。
- ESP32 第一轮 UART 记录 A valid 选择 B、B pending 确认 valid；otadata 两扇区分别为序号 1/valid、2/valid。第二轮记录 B valid 选择 A、A pending 拒绝并回到 B valid；otadata 分别为序号 3/invalid、2/valid。实验后完整 4 MiB 比较只在 bootloader、otadata、两个应用区发现差异，旧分区表、NVS 与 `at_customize` 均未变化。原 AT 全片写回并独立全片读回，与最新两份备份逐字节一致；重启后 `AT+GMR`、`AT+CWMODE?`、`AT+CWJAP?` 均返回 OK，三项响应与实验前逐字节一致。

原始 Flash、串口记录、设备身份与配置查询只保存在 mac-pro-1 的权限受限私有收据 `tooling/esp-tool/provisioning/receipts/private/p5-05-dual-board-20260926/`，不进入公开仓。临时签名键只保存在 mac-work-1 的仓外私有目录。本记录不包含原始设备身份、凭据或 Flash 字节。

## ESP32 旧数据只读归档试算

ESP32 原表的 `0x9000..0xf000` 全为 FF；旧 `nvs@0x12000/0xe000` 和 `at_customize@0x20000/0xe0000` 各只有相对前两个 4 KiB 扇区非 FF。对实验前最新双备份和 P1-04 冻结双备份四份镜像，均在内存中按候选 `at_old_raw@0x3e6000/0x4000` 的 16 KiB 几何，将旧 NVS 两页置前、`at_customize` 两页置后，再以 FF 补齐原分区长度；两个完整旧分区均逐字节重建、全区 SHA-256 与各自输入一致。最新备份中 NVS/`at_customize` 分别有 5,195/1,017 个非 FF 字节；P1-04 冻结件分别为 4,160/1,017 个。该计算没有写板或改产品分区 CSV，也不证明新布局迁移已完成。

尚需同一产品精确源码、生产签名信任配置、新三包槽布局、真实 HTTPS 下载/Flash、跨两槽安装与回退、断电路径及持续运行状态复核，才能按主计划裁定 P5-05、P6-03。C3 NVS 差异在后续限域试验前仍需定位。
