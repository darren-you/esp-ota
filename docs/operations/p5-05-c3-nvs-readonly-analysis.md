# P5-05 C3 NVS 差异只读勘误（2026-09-26）

本勘误补充[双板签名启动窄探针](p5-05-signed-board-probe.md)中的 C3 NVS 差异。只读取 mac-pro-1 的私有写前双份全片、试验后全片、恢复读回、串口与复位日志，以及 Base 和锁定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 源码；未打开设备端点、写板或修改备份。下文只给分区、条目类型和变化计数，不发布设备身份、PHY 数据或配置原文。

## 实测可确定的变化

原表中默认 `nvs@0x9000/0x6000` 与 `base_store@0x3e0000/0x20000` 是两个不同分区。写前两份完整 4 MiB 与 P1-04 冻结件逐字节一致；实验后的完整读回，额外差异仅在默认 NVS 的以下三页，合计 **5,922 字节**。`base_store` 全区、NVS 中的设备身份及其余配置条目均逐字节未变。

| 默认 NVS 物理页 | 字节变化 | 页状态与可解释的动作 |
| --- | ---: | --- |
| `0xb000` | 18 | 序号 11 从 Active 变 Full；原 `phy/cal_data` 的记录状态由 Written 变 Erased，记录数据字节未变 |
| `0xc000` | 1,951 | 原空白页变为序号 12 Active；写入新的 `phy/cal_data` 数据条目与索引 |
| `0xe000` | 3,953 | 序号 9 的旧 Full 页擦为空白；该页原条目均为已失效的 PHY 校准记录 |

锁定 SDK 的 NVS 解析器对变化前后的非空页页头及有效条目 CRC 均核对通过。逻辑记录集合仍包含同一批 namespace/key；其中只有 `phy/cal_data` 的 **1,904 字节 blob** 内容变化了 190 字节，`phy/cal_mac`、`phy/cal_version` 和 `base_identity/device_uuid` 未变。NVS 源码在 blob 值相同的情况下跳过重写；页状态、索引与新旧值共同证明发生了一次校准缓存更新，并伴随 NVS 页轮转/回收，而非 5,922 字节业务配置被改写。

## 源码路径与阶段边界

Base 当前启动路径在 `device_protocol/esp_base_protocol.c` 调用 `wifi_runtime/esp_base_wifi.c`；后者初始化 Wi-Fi、使用 RAM 存储 Wi-Fi 配置并启动 station。锁定 SDK 的 `esp_phy/src/phy_init.c` 在 RF PHY 首次启用时读取默认 NVS 的 `phy/cal_data`；旧校准数据读取失败或 PHY 返回校验失败时，会以 `nvs_set_blob` 写回该键。`nvs_flash/src/nvs_storage.cpp` 与 `nvs_pagemanager.cpp` 解释了旧条目失效、新页激活和旧页擦除。现有材料不能区分这两个 SDK 写回条件中的哪一个实际发生。

按私有收据时间线，P1-04、fresh-a、fresh-b 均没有该变化；**首次能观察到**变化的是 `c3-test-full.bin`。fresh-b 读取结束以及写前 `read_mac` 结束都执行了 hard reset；fresh-b 后的 Base status 已显示新的 boot ID。随后刷入的两份探针应用及探针 bootloader 均不含 `phy`/`cal_data` 键，探针 ELF 没有 `esp_phy_enable`、`esp_phy_load_cal_and_init` 或 `esp_wifi_init/start` 符号，试验串口只记录探针启动与切槽。结合写入的条目类型，证据强烈支持原 Base 的 Wi-Fi/PHY 初始化在写前更新了 SDK 校准缓存，探针正常代码路径不具备该写入入口。

**不能从现有收据独立证明写入发生于两次 Base reset 中的哪一次，也不能证明 SDK 是旧数据读取失败还是校准校验失败。**fresh-b 与镜像写入之间没有 NVS 中间读回，也没有对应 Base boot 的 PHY 日志。因此保留“首次在试验后观察到”和“Base 写前更新的强证据推断”两个不同结论，不把推断改写成已测得的时间戳。若以后需要闭合精确时点，须在各次 Base 启动后、写探针前分别取得只读 NVS 快照与 PHY 日志；本轮没有为此再次操作设备。

异常路径的完整 Flash 恢复及独立 4 MiB 读回已逐字节匹配写前两份和 P1-04 原件；原 Base 身份、revision、Wi-Fi 与配置/管理状态也已复核。本勘误不改变 P5-05/P6-03 尚未验收的判定。
