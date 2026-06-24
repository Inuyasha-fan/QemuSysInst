# QEMU 系统级覆盖率收集说明
本项目用于在 QEMU（mips/mips64）上运行系统镜像并收集系统级覆盖率。当前链路以 `coverage.c` 插件 + `test_qemu_client` 控制协议为主。

## 一、依赖安装
Ubuntu 22.04 可先安装：
```shell
sudo apt update
sudo apt install -y \
  build-essential \
  git \
  ninja-build \
  bridge-utils \
  uml-utilities \
  libglib2.0-dev \
  net-tools \
  expect \
  pkg-config \
  python3-pip
pip install pexpect paramiko distlib
```
说明：
- `build-essential`：编译工具链（gcc、make 等）。
- `git`：版本控制工具。
- `ninja-build`：QEMU 编译所需的构建系统。
- `bridge-utils`/`uml-utilities`/`net-tools`：用于网桥和虚拟网卡配置。
- `libglib2.0-dev`、`pkg-config`：用于 `Debug` 工具编译。
- `expect`：自动化交互脚本。
- `python3-pip`：Python 包管理器。
- `pexpect`/`paramiko`：用于 Python 测试脚本。
- `distlib`：Python 包管理工具。

## 二、编译
先在项目根目录准备 QEMU 源码：

```shell
git clone https://github.com/qemu/qemu
cd qemu
git checkout v8.2.10
cd ..
mv qemu Qemu
mkdir -p Qemu/build
```

将覆盖率插件源码复制到 QEMU 插件目录：

```shell
cp Debug/src/coverage/coverage.c Qemu/contrib/plugins/
cp Debug/src/coverage/coverage_comm.c Qemu/contrib/plugins/
cp Debug/src/coverage/coverage_comm.h Qemu/contrib/plugins/
cp Debug/src/socket/socket_comm.c Qemu/contrib/plugins/
cp Debug/src/socket/socket_comm.h Qemu/contrib/plugins/
```

修改 `Qemu/contrib/plugins/Makefile`，在 `contrib_plugins` 列表中添加 `coverage.so`：

```makefile
contrib_plugins += coverage.so
```

`Debug` 目录构建命令：

```shell
make -C Debug
```

QEMU 编译（仅 `mips-softmmu,mips64-softmmu`，开启插件）：

```shell
cd Qemu/build
../configure \
  --target-list=mips-softmmu,mips64-softmmu \
  --enable-plugins \
  --enable-debug \
  --prefix=$PWD/install
make -j"$(nproc)"
make install
```

如仅重编插件：

```shell
cd Qemu/build/contrib/plugins
make -j"$(nproc)"
```

## 三、QEMU 启动
首次使用前，需要先下载内核文件和磁盘文件：

```shell
curl -o Simulation/mirrors/debian_squeeze_mips_standard.qcow2 https://people.debian.org/~aurel32/qemu/mips/debian_squeeze_mips_standard.qcow2
curl -o Simulation/mirrors/vmlinux-2.6.32-5-4kc-malta https://people.debian.org/~aurel32/qemu/mips/vmlinux-2.6.32-5-4kc-malta
```

无插件启动（用于验证镜像、登录、网络）：

```shell
# 退出时先按 Ctrl + A，再按 C 进入 QEMU 控制台
./Simulation/auto_qemu
```

覆盖率模式启动：

```shell
# 正常启动，不加载覆盖率插件
./Simulation/scripts/build.sh
# 启动覆盖率插件（监听 127.0.0.1:3111）
./Simulation/scripts/build.sh coverage
```

如需虚拟机访问外网，在宿主机上配置转发和 NAT（将 `ens3` 替换为宿主机真实网卡）：

```shell
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -j MASQUERADE
sudo iptables -A FORWARD -i ens3 -o Virbr0 -j ACCEPT
sudo iptables -A FORWARD -i Virbr0 -o ens3 -j ACCEPT
```

在虚拟机内配置 DNS 服务器：

```shell
echo "nameserver 223.5.5.5" > /etc/resolv.conf
```

## 四、覆盖率控制协议
`test_qemu_client` 与 `Qemu/contrib/plugins/coverage.c` 已改为“CSV 落盘完成通知”协议，不再实时发送/接收 hash。
每轮流程如下：
1. `client -> plugin` 发送 `1`：开始该轮覆盖率采集。
2. 执行测试动作（例如 SSH 命令、SMTP 发信）。
3. `client -> plugin` 发送 `0`：结束该轮采集。
4. `plugin` 收到 `0` 后写入 `Data/coverage-i.csv`。
5. `plugin -> client` 发送 `1`：通知该轮 CSV 写完。
6. `client` 收到通知后读取 `Data/coverage-i.csv`。

可直接运行示例客户端（当前默认两轮）：

```shell
./Debug/build/test_qemu_client
```

补充：
- 插件首次从 noise 阶段切入 coverage 阶段时会导出 `Data/noise.csv`。
- 如果插件端长时间未接入客户端，socket 会超时退出。

## 五、测试脚本说明
说明：
- SMTP 服务（包括本地邮件）要正常使用，需要先完成与虚拟机访问外网相同的转发和 NAT 配置。
- SMTP 相关测试脚本运行前，必须在 guest 内完成一次 `exim4` 配置；这项配置只需要做一次，后续无需重复设置。

`exim4` 配置命令：
```shell
dpkg-reconfigure exim4-config
```

配置项：
- `General type of mail configuration`：选择 `internet site; mail is sent and received directly using SMTP`，把当前虚拟机配置成直接通过 SMTP 收发邮件的独立主机，供测试脚本发信和收信使用。
- `System mail name`：填写 `debian-mips`，设置本机邮件系统名称，影响邮件头、本地投递和收件目标匹配。
- `IP-addresses to listen on for incoming SMTP connections`：填写 `0.0.0.0 ; ::0`，让 `exim4` 监听所有 IPv4 和 IPv6 地址，确保转发到虚拟机的 SMTP 连接可以接入。
- `Other destinations for which mail is accepted`：填写 `debian-mips`，声明发往 `debian-mips` 的邮件由本机接收并投递到本地邮箱。
- `Domains to relay mail for`：保持空，表示不为其他域做邮件中继。
- `Machines to relay mail for`：保持空，表示不额外开放其他主机通过本机中继发信。
- `Keep number of DNS-queries minimal (Dial-on-Demand)?`：选择 `No`，避免拨号场景优化影响正常的 SMTP 主机名解析。
- `Delivery method for local mail`：选择 `mbox format in /var/mail/`，把本地邮件投递到 `/var/mail/`，与测试脚本读取本地邮箱文件的方式一致。
- `Split configuration into small files?`：选择 `No`，使用单文件配置，便于当前测试环境排查和维护。

### test_ssh.py
功能：
- 建立 SSH 连接并执行命令，支持 `none` / `init` / `random` 三种模式。
- `random` 模式会先执行一次短 `sleep` 再执行随机命令。

三种模式区别：
- `none`：只建立 SSH 连接，不执行任何业务命令，用来采集“只有连接行为时”的覆盖率。
- `init`：执行固定命令 `sleep 0.1`，用来采集 SSH 的初始化基线覆盖率，尽量减少随机因素。
- `random`：从 `ls`、`ifconfig`、`ping -c 1 127.0.0.1`、`ip link`、`hostname`、`whoami`、`netstat -antp`、`echo 123` 中随机选一条执行；执行前还会额外跑一次 `sleep 0.1`，使流程与初始化阶段更接近。

运行：
```shell
python3 Debug/src/test/test_ssh.py
```
### test_smtp.py
功能：
- 建立 SMTP 连接并发送邮件。
- 支持 `none` / `init` / `random` 三种正文模式。

三种模式区别：
- `none`：只建立 SMTP 连接，不发送邮件，用来采集“只有连接行为时”的覆盖率。
- `init`：发送一封空正文邮件，用来采集 SMTP 的初始化基线覆盖率。
- `random`：发送一封随机正文邮件，正文长度在 1 到 64 之间，字符集包含字母、数字、标点和空格，用来观察 SMTP 业务输入变化带来的覆盖率变化。

运行：
```shell
python3 Debug/src/test/test_smtp.py
```
### test_coverage.py
功能：
- 自动启动 `build.sh coverage`。
- 自动登录 guest、执行基础网络准备。
- 通过 socket 驱动多轮 SSH/SMTP 覆盖率采集，并在每轮结束后读取对应的 `Data/coverage-i.csv`。

流程：
- 先采集 1 轮 SSH `none` 覆盖率，再采集 1 轮 SMTP `none` 覆盖率，用于得到“只有连接行为时”的覆盖数据。
- 再采集 1 轮 SSH `init` 覆盖率和 1 轮 SMTP `init` 覆盖率，作为两个协议的初始化基线。
- 然后执行 SSH `random` 100 轮和 SMTP `random` 100 轮，每轮结束后等待插件写完 CSV，再读取该轮覆盖数据。

数据：
- 对每一轮 `random` 覆盖率，分别计算它与 SSH `init` 基线、SMTP `init` 基线的重合比例。
- 如果某轮与 SSH `init` 的重合比例更高，则把该轮判定为 SSH；如果与 SMTP `init` 的重合比例更高，则判定为 SMTP；相等则记为 `equal`。

输出数据：
- `none` 覆盖规模：记录 SSH `none` 和 SMTP `none` 各自采集到了多少个 hash。
- `init` 基线覆盖规模：记录 SSH `init` 和 SMTP `init` 各自采集到了多少个 hash。
- `none/init` 重合度：统计 SSH `none` 与 SSH `init` 的重合数量及占 SSH `init` 的比例；SMTP 同理。这个值可以看作“空操作覆盖”和“初始化基线覆盖”的接近程度。
- 每轮阶段判定结果：日志中会按行输出 `actual_phase`、`predicted_phase`、`round`、`current_vs_init_ssh`、`current_vs_init_smtp`，用于查看每一轮被判成了什么阶段，以及两个基线重合比例分别是多少。
- SSH 阶段判定正确率：在实际为 SSH 的所有 `random` 轮次里，被正确判为 SSH 的比例。
- SMTP 阶段判定正确率：在实际为 SMTP 的所有 `random` 轮次里，被正确判为 SMTP 的比例。
- 覆盖率原始结果：插件会输出 `Data/noise.csv` 和 `Data/coverage-*.csv`，便于后续单独分析每轮覆盖数据。

运行：

```shell
python3 Debug/src/test/test_coverage.py
```

输出：
- 覆盖率 CSV：`Data/noise.csv`、`Data/coverage-*.csv`
- 测试日志：`Log/test_coverage.log`

### test_coverage_presentation.py
功能：
- 基于 `test_coverage.py` 的覆盖率采集逻辑，保留 `none`/`init` 固定前置流程。
- `random` 阶段改为 SSH 和 SMTP 混合随机发送（总量仍为 SSH 100 轮、SMTP 100 轮）。
- 日志仅写入 `Log/test_coverage.log`，不输出到终端。
- 终端提供中文测试展示信息，适合演示场景。

终端展示内容：
- 初始化信息：开始测试、等待 QEMU 启动、发送初始包。
- 测试信息：第 `x` 个数据包的协议类型、发送内容、判定结果。
- 测试结果：SSH 判定率、SMTP 判定率。

运行：
```shell
python3 Debug/src/test/test_coverage_presentation.py
```
