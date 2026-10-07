# MyFolder Local

面向个人与小型实验室服务器的内网文件服务。后端使用 C++17、Drogon 和 SQLite，Web 界面使用 Vue 3。安装包包含后端服务与浏览器界面，无需单独安装客户端。

## 功能

- 文件与文件夹的上传、下载、创建、移动、改名和删除；文件夹可打包下载。
- 超级管理员创建管理员，管理员创建普通用户；按路径分别授予上传、下载、修改、删除权限。普通用户默认可下载根目录内容。
- 忘记密码申请由上级管理员审核。审核通过后临时密码为 `123456`，须在 30 分钟内登录并设置新密码。
- 超级管理员可申请连接另一台 MyFolder Local 服务器；对方超级管理员审核，并按路径授予访问权限。Web 文件区可切换本机与已授权的外连服务器。
- 显示服务器名称、存储用量、文件数和外连状态；超级管理员可调整名称及存储上限，默认上限为 5 GiB。

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

Web 管理页中的存储容量上限独立于单个 HTTP 请求上限。跨公网连接其他服务器时，应通过受信任的 TLS 反向代理或 VPN 保护通信，并由目标服务器超级管理员审批路径权限。

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
