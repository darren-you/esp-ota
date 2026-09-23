# AGENTS

本仓尚未纳入根仓 AGENTS 生成 manifest；完成首个仓库提交、公开远端与根 gitlink 纳管后，由工作区 `harness/scripts/sync_agents.sh` 根据 `.agents/esp-ota.md` 生成本文件。当前工作遵守工作区根 `AGENTS.md` 和下列仓内规则。

## 仓内边界

- OTA 组件只负责签名 ESP app 固件的下载、校验、槽选择、确认和回滚；业务授权、持久操作收据、自检窗口及 product 包归调用方。
- 独立构建只依赖本仓与精确 ESP-IDF/esp-lwip 公开源码，不从相邻仓库或私有 Tool 读取实现和凭据。
- 准备镜像不切槽；切槽前重新核对分区、摘要及 IDF 签名。普通未签名构建拒绝升级。
- 编译和 host 假件测试不授权设备写入。刷板、分区、eFuse 和生产密钥操作必须另有精确设备授权与恢复基线；人工断电测试按主计划暂缓。
