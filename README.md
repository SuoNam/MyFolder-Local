# MyFolder Local

面向个人与小型实验室服务器的内网文件服务。后端使用 C++17、Drogon 和 SQLite，Web 界面使用 Vue 3。安装包包含后端服务与浏览器界面，无需单独安装客户端。

![MyFolder Local 功能演示](docs/demo.gif)

演示使用临时示例账号与文件，不包含真实服务器数据。

## 功能

当前源码与 rk3528 部署已包含注册审批和可信浏览器免密；现有 v1.5.0-1 Release 安装包尚不包含这些功能。使用说明和测试清单见 [注册与免密登录](docs/registration-trusted-login.md)，本次上线及备份信息见 [部署记录](docs/deployment-2026-10-25-auth.md)。

2026-10-26 已改为正确密码登录默认记住浏览器，并支持超级管理员设置有效天数或长期有效（初始默认 90 天）；无需额外设备审批或原注册收据，公共电脑可取消勾选，主动退出会撤销。最新上线及复测步骤见 [免密期限配置记录](docs/deployment-2026-10-26-login-policy.md)。

- 文件与文件夹的上传、下载、创建、移动、改名和删除；文件夹可打包下载。
- 超级管理员创建管理员，管理员创建普通用户；按路径分别授予上传、下载、修改、删除权限。普通用户默认可下载根目录内容。
- 忘记密码申请由上级管理员审核。审核通过后临时密码为 `123456`，须在 30 分钟内登录并设置新密码。
- 超级管理员可申请连接另一台 MyFolder Local 服务器；对方超级管理员审核，并按路径授予访问权限。Web 文件区可切换本机与已授权的外连服务器。
- 显示服务器名称、存储用量、文件数和外连状态；超级管理员可调整名称及存储上限，默认上限为 5 GiB。
- 移动端侧栏导航、触屏文件操作、长文件名换行、权限及外连列表卡片布局；支持窄屏与横屏。
- 用户自行申请注册，由管理员审核；新注册账号默认没有文件权限，需另行授权。
- 正确密码登录默认记住当前浏览器，初始 90 天；超级管理员可在“用户与权限 → 浏览器免密设置”改为 1–3650 天或长期有效。“登录设备”支持查看、命名和撤销，无需再审批浏览器。
- 默认通过内网 HTTP 使用，注册审批与浏览器免密无需配置证书；HTTPS 为可选增强。

移动端回归方法及部署记录见 [移动端适配记录](docs/mobile-2026-10-25.md)。此更新不修改数据库或存储目录，现有 v1.5.0-1 安装包未重新发布。

## 下载与安装

在 [Releases](https://github.com/SuoNam/MyFolder-Local/releases) 下载与目标系统、CPU 架构一致的原生 `.deb`：

| 操作系统 | x86-64 | ARM64 |
| --- | --- | --- |
| Ubuntu 22.04 | `ubuntu22.04_amd64.deb` | `ubuntu22.04_arm64.deb` |
| Ubuntu 24.04 | `ubuntu24.04_amd64.deb` | `ubuntu24.04_arm64.deb` |
| Debian 12 | `debian12_amd64.deb` | `debian12_arm64.deb` |
| Debian 13 | `debian13_amd64.deb` | `debian13_arm64.deb` |

文件完整名称以 `myfolder-lan_1.5.0-1_` 开头。`all.deb` 是在目标设备安装时编译 C++ 后端的源码包，适用于发行版提供 `libdrogon-dev` 等构建依赖的环境；优先使用表中的原生包。其他系统或架构可按下文从源码编译。

```bash
sudo apt install ./myfolder-lan_1.5.0-1_ubuntu24.04_arm64.deb
sudo -u myfolder-lan env MYFOLDER_DATA=/var/lib/myfolder-lan \
  /opt/myfolder-lan/bin/myfolder-lan --init-admin
```

第二条命令仅首次初始化时生成 `admin` 超级管理员及随机初始密码。请把输出保存在可信的位置；不要将密码写进代码仓库或聊天记录。服务会随安装启动，默认监听 `0.0.0.0:8080`。从局域网浏览器访问 `http://服务器内网地址:8080/`。建议在首次登录后修改密码，并仅在受信任的网络开放该端口。

服务配置文件为 `/etc/default/myfolder-lan`，数据与上传文件位于 `/var/lib/myfolder-lan`。修改配置后运行 `sudo systemctl restart myfolder-lan`。常用环境变量：

| 变量 | 默认值 | 含义 |
| --- | --- | --- |
| `MYFOLDER_HOST` | `0.0.0.0` | 监听地址 |
| `MYFOLDER_PORT` | `8080` | HTTP 端口 |
| `MYFOLDER_IO_WORKERS` | `4` | I/O 工作线程数 |
| `MYFOLDER_MAX_UPLOADS` | `2` | 同时上传数 |
| `MYFOLDER_MAX_REQUEST_GIB` | `5` | 单个 HTTP 请求的 GiB 上限 |
| `MYFOLDER_REQUIRE_HTTPS_DEVICE_LOGIN` | `0` | 可选：设为 `1` 后仅允许在可信 HTTPS 入口申请、领取和恢复浏览器免密；不限制普通密码登录 |
| `MYFOLDER_TRUSTED_PROXY_IPS` | `127.0.0.1,::1` | 允许声明 `X-Forwarded-Proto: https` 的反向代理源 IP，逗号分隔；须严格配置 |

Web 管理页中的存储容量上限独立于单个 HTTP 请求上限。跨公网连接其他服务器时，应通过受信任的 TLS 反向代理或 VPN 保护通信，并由目标服务器超级管理员审批路径权限。

默认 HTTP 即可完成注册、审批、密码登录和浏览器免密，不需要额外开关或证书。HTTP 不加密密码、登录凭据及文件内容，请仅在可信内网使用，不要直接映射到公网。管理员审批、凭据哈希和 Cookie 属性不能代替传输加密。

HTTPS 为可选增强：配置 HTTPS 反向代理后，向后端覆盖写入 `X-Forwarded-Proto: https`，并将代理的实际源 IP 配入 `MYFOLDER_TRUSTED_PROXY_IPS`；后端会自动给认证 Cookie 增加 `Secure` 标记。代理应覆盖客户端提供的转发头，后端端口应限制为代理可达。如果还希望强制浏览器免密仅走 HTTPS，可设置 `MYFOLDER_REQUIRE_HTTPS_DEVICE_LOGIN=1`；它不会将整个站点自动转换为 HTTPS 或禁止 HTTP 密码登录。

## 源码构建

先构建 Web，再在目标系统构建后端。依赖 C++17 编译器、CMake、Drogon、OpenSSL、SQLite3，以及系统提供的 Drogon 构建依赖。

```bash
cd web
npm ci
npm run build
cd ..
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --parallel 2
MYFOLDER_CPP_BINARY="$PWD/cpp/build/myfolder-lan" \
  python3 -m unittest discover -s tests -p 'test_*.py' -v
```

构建对应发行版的原生包时，`packaging/build_native.sh` 会下载固定版本的 Drogon 1.9.12 和 Trantor 1.5.26，在当前系统上编译静态框架库、测试并生成 `.deb`。例如：

```bash
bash packaging/build_native.sh ubuntu24.04
```

构建脚本要求已有 `web/dist`，并安装 `cmake g++ libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev libsqlite3-dev libc-ares-dev python3 dpkg-dev curl ca-certificates`。源码编译包可通过 `python3 packaging/build_deb.py` 生成。

## 发布包与数据

GitHub Actions 在 Ubuntu 22.04/24.04 和 Debian 12/13 的 amd64、arm64 环境中分别编译、测试并发布原生包。Release 还包含一个 `all.deb` 源码编译包。发布仓库不包含运行中的 SQLite 数据库、上传文件、日志、会话或初始管理员密码。安装或升级软件包前，请自行备份 `/var/lib/myfolder-lan`。

项目中的 `cpp/third_party` 包含带原始许可文件的 cpp-httplib 与 nlohmann/json 头文件。
