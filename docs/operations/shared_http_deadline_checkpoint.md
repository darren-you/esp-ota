# 共享 HTTPS 期限机制检查点

2026-10-02，本轮从公开 OTA `99afc7b249c57e438f5b5afff3c34efb3decd374` 的完整源码导出，在仓外候选中把既有期限对象、初始化／剩余时间查询及 transport 工厂整理为 `include/eota_http_transport.h`。旧 `src/http_transport.h` 删除，网络进展与指定时刻查询保持私有；不存在第二份 DNS／TCP／TLS 实现。原期限算法和 DNS、非阻塞 socket、TLS、读写算法均未改，工厂增加可信时间、默认 CA bundle 与证书日期配置的准入核对。

调用方保有唯一期限对象。工厂接收连接预算和本次启动可信时间事实，只提供 HTTP transport 机制；固件策略仍在 `eota.h`，产品包授权、长度、Flash 与持久化仍由 Base 负责。HTTP client 先 cleanup，随后 transport destroy，最后释放期限 owner；固定 SDK 对有效非空 client 不存在 cleanup 失败分支，不用假件伪造该分支。单次使用的 close 不销毁 TLS／socket，超时 DNS 的迟到回调不接触期限 owner。

## 软件验证

- 固定 SDK 为公开 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`，独立 lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`；源守卫核对这两层，未以父 SDK 的旧 lwIP gitlink 抹平独立锁。
- ASan／UBSan 7 项通过：两目标更新、期限、槽确认、真实 transport 配假 DNS／TLS，以及分别缺少证书日期／默认 bundle 的工厂拒绝。
- 真实固定 SDK mbedTLS 回环的完整 8 项通过，覆盖 TLS 1.2／1.3、正确 CA、错误 CA、错误主机名、原样 SNI、慢握手与 HTTP 慢滴流总期限。回环仅使用临时测试 CA；不运行设备默认 bundle、SDK HTTP 解析器或 Flash。
- C3 和 ESP32 独立 OTA 样例完整固定 SDK 构建通过。Base 最终停止／启动与产品包 HTTPS 组合另外在相同 provider 候选下验证，其结果记录在 Base 的共享期限消费检查点。

## 仍开放的边界

公开接口没有新增外部取消请求、线程、定时器或状态机；业务取消仍由原产品 owner 负责。期限约束网络等待和调用返回后的成功资格，不能抢占密码学单步、HTTP 解析、SDK cleanup／socket close 或 Flash。上述软件结果不构成设备网络、长期联合峰值、断电、bootloader、回滚或恢复验收。本轮没有设备访问、生产变更、SDK 补丁或秘密变更。
