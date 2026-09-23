# P5-03 签名 C3 构建复验（2026-09-23）

针对 `esp-ota@51de81313ab90f9f2609fae2a0178a8ca8cb717c`，在独立仓外 checkout 使用锁定的 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 与 esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，对 `examples/c3` 的默认空输入样例执行签名构建。RSA-3072 测试键由 `espsecure v5.4.0 generate-signing-key --version 2 --scheme rsa3072` 在仓外临时目录新生成；`sdkconfig` 和构建目录也位于仓外。

构建启用了 `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`、`CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME`、`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`、`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES`、`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 和 `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT`。构建日志确认当前 `update.c` 与 `http_transport.c` 均参与编译，签名应用镜像大小为 `0x31000`（200704）字节，SHA-256 为 `afb46cd7ed1b72543078806385a558014b809d8e1fa0229659bdc54f3fc5586b`。`espsecure verify-signature --version 2 --keyfile` 对该镜像报告 `Signature block 0 is valid (RSA)` 且核验成功。

本轮只执行仓外编译与本机验签；没有刷板、写 Flash／otadata、改分区、烧 eFuse、使用生产凭据或更换生产密钥。空输入样例与签名校验不构成实板 HTTPS 下载、Flash 写入、bootloader 切槽或回滚验收。
