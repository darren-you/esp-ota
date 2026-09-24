## ESP OTA 边界

- 本仓只提供签名 ESP app 固件的下载、校验、槽选择、确认与回滚机制；业务授权、持久操作收据、自检窗口及 product 包归调用方。
- 独立开发只依赖本仓和精确 ESP-IDF，不读取相邻工作区仓、私有工具、真实凭据或设备恢复字节。
- 准备镜像不切槽；显式切槽前重新核对完整摘要、真实分区与 SDK 验签。普通未签名构建必须拒绝 OTA。
- 编译与 host 假件测试不代表设备写入授权或实板验收。真实设备操作必须先核对唯一设备、完整 Flash 恢复基线与当前授权；禁止自动整片擦除、eFuse 写入和生产密钥替换。
- 实板人工断电测试按[五仓主计划](https://github.com/darren-you/darren-space/blob/master/harness/docs/design/darren-space/global/esp-base-frp-mqtt-ota-container-development-plan.md)当前决定及对应验收前置执行；具备测试条件不代表已执行或通过，不能跳过设备授权、签名运行/回退基线与恢复前置，也不能用软件重启冒充真实断电。
