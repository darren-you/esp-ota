# P5-03 签名 C3 构建复验（2026-09-23）

针对 `esp-ota@51de81313ab90f9f2609fae2a0178a8ca8cb717c`，在独立仓外 checkout 使用锁定的 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，对 `examples/c3` 的默认空输入样例执行签名构建。RSA-3072 测试键由 `espsecure v5.4.0 generate-signing-key --version 2 --scheme rsa3072` 在仓外临时目录新生成；`sdkconfig` 和构建目录也位于仓外。

构建启用了 `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`、`CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME`、`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`、`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES`、`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 和 `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT`。构建日志确认当前 `update.c` 与 `http_transport.c` 均参与编译，签名应用镜像大小为 `0x31000`（200704）字节，SHA-256 为 `afb46cd7ed1b72543078806385a558014b809d8e1fa0229659bdc54f3fc5586b`。`espsecure verify-signature --version 2 --keyfile` 对该镜像报告 `Signature block 0 is valid (RSA)` 且核验成功。

本轮只执行仓外编译与本机验签；没有刷板、写 Flash／otadata、改分区、烧 eFuse、使用生产凭据或更换生产密钥。空输入样例与签名校验不构成实板 HTTPS 下载、Flash 写入、bootloader 切槽或回滚验收。

## 公开 ESP-IDF fork 锁定复验

以 `esp-ota@757bf0a0d8e3898e4a990e191172c2c291fcf4da` 的源码和更新后的 SDK 锁，在仓外独立 checkout 使用公开 `esp-space/esp-idf@855937cf9dcee13ee9c423fb0319238cdc8d53fd` 与 `esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d` 再次构建 C3 默认空输入样例。临时 RSA-3072 测试键在仓外生成；签名更新、RSA、构建签名镜像、回滚和 custom transport 均在最终 `sdkconfig` 中启用。构建日志确认 `ota.c`、`update.c`、`http_transport.c` 均参与编译，签名镜像大小为 `0x31000`（200704）字节，SHA-256 为 `32e30e0b1bc74cebaebc5362e226293b35b84f85b7b7ec6a4305e32efa07afc9`。`espsecure v5.4.0 verify-signature --version 2 --keyfile` 报告 `Signature block 0 is valid (RSA)` 且使用该测试键验签成功。

同一 fork SDK 的主机测试使用真实 mbedTLS 4.1 和 ASan/UBSan，CTest 5/5 通过；独立 C3 普通构建也通过。以上只证明锁定来源下的编译、主机 HTTPS 合同和测试键签名；未连接设备或执行升级、切槽、回滚。

## 完整签名镜像长度绑定（2026-09-24）

基于 `3c3f72b` 的未提交通用修复，在独立 checkout 使用 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d` 验证。被测 `components/esp_ota/src/update.c` 的 SHA-256 为 `7f99fba09e4958eef48a34cb4fdcb65bb852f19fc80818fdd5e9a9c92da54fce`。SDK 父仓的 dirty 标记仅来自按独立锁切到指定 lwIP gitlink，lwIP 工作树本身干净。

固定 SDK 的 `esp_ota_begin` 按下载长度向上取整擦除；`esp_ota_end` 和 `esp_ota_set_boot_partition` 按整个分区验签，未要求下载长度等于实际 signed image。新增短／长长度回归在旧实现的准备成功路径确定性失败。修复在准备产出、切槽写入和运行镜像收据查询处，将请求长度与 `esp_image_verify` 成功返回的完整 `image_len` 精确比较，保留 SDK 原有验签。更新后的主机回归 4/4、ASan/UBSan 加真实 mbedTLS HTTPS 回环 5/5 通过；覆盖长度不同、签名／资源错误、额外验签逾期、句柄释放及 boot selector 不变。

使用仓外新建的 RSA-3072 测试键，并用非真实网络输入启用样例 OTA 路径，签名 C3 样例构建通过；`update.c`／`http_transport.c` 参与编译，最终链接包含长度检查及准备／选择路径。签名应用为 1,118,208 字节（`0x111000`），SHA-256 `4216975aa5f6dadc7500b7874a1049bf6f2c22a70b02ceb73e46aa1fae3a0b61`；`espsecure v5.4.0 verify-signature` 使用该测试键核验成功。样例没有连接设备或启动网络。

另用官方 QEMU `esp_develop_9.2.2_20260417` 运行仓外 C3 探针，在虚拟 Flash 的两个既有 OTA 分区放入同一份有效签名镜像。通过 SDK 选中／确认运行槽后，只重写另一槽前 262,144 字节，保留末尾 4,096 字节的旧签名扇区。实际 SDK `esp_ota_end` 返回 `ESP_OK`，再次验签返回完整长度 266,240；修复后的 `eota_select` 和 `eota_sha256_running` 对该短长度均返回 `invalid_request`，boot selector 保持原槽。随后使用完整长度及摘要，运行镜像身份和正常选槽均通过。关键输出为：

```text
STALE_TAIL sdk_end=0 written=262144 signed_image_len=266240 retained_signature=4096
BOUNDARY short_select=invalid_request short_running=invalid_request boot_unchanged=1
FULL_IMAGE exact_identity=1 select=ok qemu_only=1
```

探针应用 SHA-256 为 `71f886bc44b9a7c7efc9d87ae027f258fd57f6826817aad60f98823e1fe2e403`；日志位于仓外 `esp-ota-image-size.20260924/qemu-stale-tail.log`，SHA-256 为 `82f5492ae1c82e0522632362736afac262042a2251407d60fe7ba95452ee65a6`。探针含额外复制缓冲及断言，单独使用 8 KiB 主任务栈；不修改组件、样例或 Base 的任务栈合同，也不作为组合资源证据。这证明的是完整镜像摘要身份约束，不是绕过 RSA。没有设备 Flash／otadata 写入、eFuse 操作或生产密钥变更；真实 HTTPS／网络栈／Flash、断电及组合峰值验收仍待完成。


### 运行摘要入口的 6 KiB 栈复核

Base 的 `ota.result → receipt_query → evaluate → eota_sha256_running` 由 6,144 字节控制任务调用；prepare/select 使用独立 12,288 字节 OTA worker。为核对新增完整镜像验证的栈需求，仓外 QEMU 探针先创建独立 6,144 字节任务，在应用尚未调用任何 RSA 验签入口前执行完整运行镜像摘要，再验证短长度拒绝。两次分别返回 `ok`、`invalid_request`，最低栈余量均为 **2,940 字节**；已确认该 IDF 的 `StackType_t` 为 1 字节。随后原有完整镜像／短长度矩阵继续通过。

这仅证明相同 SDK 的直接入口调用余量，没有模拟 Base 上层完整调用帧、网络并发或真实板；不修改产品任务栈，也不把独立探针写作 Base 栈验收。新增探针应用 SHA-256 `106ae72255ae8561faf348de70fb95bf6f34d5f0928435993032aed0fd775b6a`，源码 `main.c` 为 `a00afff702504230059d65ff19516b06474878cbc57c79a08e571ae9917ffb13`，日志 `qemu-stack.log` 为 `ec73d397f32dfd1ece763b74f4936cd3d84f845a20bff4616210e5d4dfeb4ae9`，均在同一仓外 `esp-ota-image-size.20260924` 目录。

首次旧尾部探针使用默认主任务栈，额外持有复制缓冲和 metadata，在初始 SDK `set_boot` 的 RSA 路径发生 Load access fault，尚未执行本次新增长度检查；随后仅把该实验主任务改为 8 KiB，完整矩阵才通过。失败日志 `qemu-stale-tail-first.log` 保留，SHA-256 `050aaa4498e2cf4f968c766a052488b2e3cfd64ab94c4505c5aef08de74a9208`。独立 6 KiB 入口探针与上述 8 KiB 实验主任务分开，不用后者替代前者的栈证据。
