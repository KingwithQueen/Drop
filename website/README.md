# Drop 网站

dist/index.html 为产品首页，dist/privacy.html 为隐私说明，dist/terms.html 为服务条款，dist/styles.css 和 dist/assets 为本机静态资源。三页互相链接，无 JavaScript、分析脚本、登录表单或第三方字体，网页不处理 Drop 消息或 Google OAuth。

页面使用 Drop 1.0.3 的真实图标和已实现功能，说明服务器/备份加密的范围与客户端本机副本。隐私说明披露 drive.appdata、DPAPI、Google 数据用途、保留及删除；维护者联系途径当前为 GitHub 项目反馈页。公开邮箱待所有者提供后添加，Google 控制台的支持邮箱仍由所有者设置。

自定义域名：drop.jsnowly.win。Google 域名验证对应其可注册域名 jsnowly.win。网站托管和 DNS 状态以 docs/WEBSITE.md 及 build/tests 中的发布记录为准，不能仅凭本文件认为域名已生效或 Google 已审核。

可以把 dist 的内容放在任何提供 HTTPS 的静态网站根目录，无需运行 Drop 服务器或编译网站。当前 Sites 身份保存在 .openai/hosting.json；发布工作副本位于 build/sites/drop-homepage，与主程序 Git 仓库隔离。更新公开页面时同步工作副本，并按 Sites 流程提交、归档和发布，保持同一项目 ID。

只把网页静态文件上传到网站。OAuth JSON、运行目录、消息、密钥、恢复包、证书及令牌不属于网站内容。

本次将本地页面版本标识同步为 1.0.3，没有重新发布站点；线上页面仍以既有部署为准。
