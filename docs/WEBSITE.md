# Drop 首页、隐私说明、服务条款与域名

首页、隐私说明和服务条款已按用户要求制作，Sites 原生发布状态为 succeeded，访问权限为 public。源文件位于 website/dist，使用用户更新的两个图标，无登录、消息、OAuth 表单或跟踪脚本。网页是产品说明，Drop 客户端和服务器独立运行。托管站点不保存 OAuth JSON、令牌、运行数据或恢复材料。

已验证的自定义域名地址（2026-10-11，中国时间）：

- 首页：[Drop](https://drop.jsnowly.win/)
- 隐私说明：[Drop 隐私说明](https://drop.jsnowly.win/privacy.html)
- 服务条款：[Drop 服务条款](https://drop.jsnowly.win/terms.html)

Google 项目品牌资料填写：

- 首页：https://drop.jsnowly.win/
- 隐私说明：https://drop.jsnowly.win/privacy.html
- 应用服务条款链接：https://drop.jsnowly.win/terms.html
- 授权域名：jsnowly.win

域名所有者已确认添加三条记录。Sites 刷新后的域名、提供商及 HTTPS 状态均为 active；本机经现有网络代理访问首页、privacy.html 均为 HTTP 200，正文包含对应产品/隐私内容，TLS 证书验证开启。公共 Google DNS 查询确认两条 TXT 值一致；当前 CNAME 查询未返回独立 CNAME 答案，不据此否定已生效的域名。完整响应摘要见 build/tests/website-dns-20261011.json，Sites 原生状态和发布证据见 build/tests/website-20261010.json。以下是已添加记录，留作维护参考，主机记录按 jsnowly.win 区域填写。

| 类型 | 主机记录 | 值 |
| --- | --- | --- |
| CNAME | drop | custom-domains.chatgpt.site. |
| TXT | _openai-site-verification.drop | openai-site-verification=0zrxEa5TJ-8KrMqnxPv9OxqTNzGoww0FyaEKdpEisoI |
| TXT | _cf-custom-hostname.drop | 99a368df-5fd5-44f8-aff6-ee58a7533e1e |

这是网站托管及证书验证的记录，不是 Google 的域名所有权验证。网站与 HTTPS 已确认生效，后续维护使用同一 Sites 项目，不重建网站，也不改变主程序仓库的分支。原始托管地址 https://drop-message-sync.owlwithleaves.chatgpt.site/ 仍返回首页，可作辅助检查；Google 品牌资料使用上述自有域名。

Google 控制台的“品牌信息”填入自定义域名的首页、隐私说明，以及用户自己的支持/开发者邮箱。如要求域名所有权，使用与项目关联的 Google 账号在 [Search Console](https://search.google.com/search-console) 验证 jsnowly.win，将 Google 实际生成的 TXT 按要求添加到 DNS。不要把网站托管的 TXT 当作 Google TXT。Google 的 [品牌验证要求](https://developers.google.com/identity/protocols/oauth2/production-readiness/brand-verification)规定首页公开、与应用相关、隐私说明同域且披露 Google 数据用途；完成网页和 DNS 不等于 Google 已批准。

当前隐私内容描述 drive.appdata、使用范围、服务器与本机存储、DPAPI、备份保留、撤销及删除，并明确非端到端加密。维护者联系途径使用 GitHub 项目反馈页；公开邮箱由所有者另行提供。实际服务器部署者如有额外日志或其他数据处理，应补充自己的说明。

用户确认已填写品牌资料，但控制台还需要“应用服务条款链接”。已新增 terms.html，在首页/隐私说明的导航和页脚增加入口，三页互相链接。条款覆盖自行部署的运营者范围、账号与合理使用、Google Drive 可选备份、数据及恢复责任、退出与删除、开源许可及联系，不替代项目许可或实际运营者规则，不新增收费、公共托管或运行保障承诺。已通过三页 HTML/内部链接/资源检查、八文件归档与静态源码一致性检查、Sites 第 2 版公开部署及三页实际 HTTPS 200；记录为 build/tests/website-terms-20261011.json。网页布局仍未人工浏览器视觉确认。

最新发布源码提交：27831d26a607b1b8b9be125e92f40b8a9921905d，第 2 版归档为 build/sites/drop-homepage-terms.tar.gz。首次提交 b0f42e64fe629bcb4fcc7a037094ce7762a3f43a 及 build/sites/drop-homepage.tar.gz 保留。工作副本为 build/sites/drop-homepage，托管项目 ID 保存在 website/.openai/hosting.json。主程序 Git 仓库未提交或推送本轮改动；网站源码由 Sites 自己的仓库承载并已推送。修改网站时保留项目 ID，按 Sites 流程同步工作副本、验证静态资源、提交及发布，保留当前 public 访问范围。

验证边界：已检查 HTML、本机资源、内部链接和归档字节，Sites 返回发布成功，自有域名首页/隐私说明的实际 HTTPS 访问通过；本机没有可用的浏览器控制入口，网站布局尚未人工视觉确认。域名切换期间遇到的 active_redeploying、旧路由 404 已由 active 与最终 200 结果取代；绕开本机代理的访问被 Cloudflare 返回 403，不将该网络路径称为已通过。Windows 发布环境通过本次工作副本的 Git 配置绕过失效代理，Git Bash 的 PATH 与 TAR_OPTIONS=--force-local 只作用于打包进程，不改全局代理或插件文件。2026-10-11 所有者确认 Google 控制台显示正式版，发布状态按所有者反馈记录；Google 品牌审核没有独立确认。随后按所有者选择，在本机默认 LocalAppData\DropServer 目录完成全新浏览器授权及真实空存储密文备份上传/下载校验，证据 google-production-default-20261011.json；部署时仍可手动选目录，实际账号/TLS/独立恢复与长期运行继续验收。步骤见 GOOGLE_DRIVE.md。

