ABCMINT-CKPOOL + CKPROXY + libckpool 由 Erwin Schrodbit 开发，适配abcmint区块链网络

超低开销、高度可扩展的多进程、多线程模块化abcmint挖矿池、代理、透传和C语言库，支持Windows和Linux系统。
ABCMINT-CKPOOL是根据GPLv3许可证免费提供的代码，但其开发主要由委托资金支付，默认情况下，在池模式下，已解决区块的0.5%会贡献给开发团队。
如果你在池上运行它，请考虑在代码中保留这一贡献，或者如果你使用此代码，请向AUTHORS中列出的作者捐款，以资助进一步的开发。

---
许可证：

GNU公共许可证V3。详见附带的COPYING文件。

---
快速入门：

## ABCMINT矿池服务器快速部署

### 1. 克隆代码库
```bash
git clone https://github.com/ErwinSchrodbit/abcmint-ckpool-beta0.0.1.git
cd abcmint-ckpool-beta0.0.1
```

### 2. 安装依赖

#### Linux系统：
```bash
sudo apt-get update
sudo apt-get install -y build-essential yasm autoconf automake libtool libzmq3-dev pkgconf
```

#### Windows系统：
- 安装MinGW或MSVC开发环境
- 确保已安装必要的编译工具链

### 2.1 Rainbow18算法依赖
ABCMINT-CKPOOL需要Rainbow18算法库支持。请确保：
1. 将abcmint-master中的librainbowpro库文件复制到正确位置
2. 或将librainbowpro路径添加到编译环境变量中

### 2.2 Windows平台特定说明
- 代码已移除对Windows.h的直接依赖，确保跨平台兼容性
- 实现了跨平台的内存管理和日志功能
- 支持在Windows下使用MinGW或MSVC进行编译

### 3. 构建项目

#### Linux系统：
```bash
./autogen.sh
./configure --with-rainbow18=/path/to/librainbowpro
make -j$(nproc)
```

#### Windows系统（使用MinGW）：
```bash
./autogen.sh
./configure --with-rainbow18=/path/to/librainbowpro
make
```

或者使用提供的构建测试脚本：
```bash
./build_test.bat
```

### 4. 配置ABCMINT节点
确保您的abcmintd节点已正确配置并正在运行。编辑abcmint.conf文件，添加以下内容：
```
server=1
daemon=1
rpcuser=abcmint
rpcpassword=yoursecurepassword
rpcport=8882
rpcallowip=127.0.0.1
maxconnections=200
# 可选：启用ZMQ通知
zmqpubhashblock=tcp://127.0.0.1:28882
```

### 5. 配置矿池
复制并编辑ckpool.conf文件：
```bash
cp ckpool.conf ckpool.conf.backup
nano ckpool.conf
```

确保配置以下关键参数：
```json
{
  "btcd": [
    {
      "url": "localhost:8882",
      "auth": "abcmint",
      "pass": "yoursecurepassword",
      "notify": true
    }
  ],
  "btcaddress": "8YOURABCMINTADDRESSHERE",
  "btcsig": "YourMiningPool",
  "donation": 0.5,
  "serverurl": [
    "0.0.0.0:3333"
  ],
  "startdiff": 100,
  "version_mask": "FFFFFF00",
  "zmqblock": "tcp://127.0.0.1:28882",
  "logdir": "logs",
  "rainbow18_level": 3,  // Rainbow18算法安全级别，可选值：1, 2, 3
  "rainbow18_timeout": 5000  // Rainbow18签名验证超时时间（毫秒）
}
```

### 6. 创建日志目录
```bash
mkdir -p logs
```

### 7. 启动矿池服务器
```bash
# 直接运行
./src/ckpool

# 或使用配置文件运行
./src/ckpool -c ckpool.conf

# 或使用screen在后台运行
screen -S abcmint-pool ./src/ckpool -c ckpool.conf

---
## 最新挖矿算法适配说明

ABCMINT-CKPOOL已完成与abcmint-master最新版本的挖矿算法开发适配，主要更新包括：

### 核心函数适配
- **exfes函数**：完全重写以匹配abcmint-master的实现，包括：
  - 修复方程掩码处理逻辑
  - 简化线程分区算法
  - 修正方程复制和收缩逻辑
  - 更新区块检查机制

- **Merge_Solution回调函数**：
  - 修复解的存储格式
  - 正确处理掩码应用
  - 确保与abcmint-master的解组合逻辑一致

- **exhaustive_search_wrapper函数**：
  - 更新区块检查逻辑
  - 确保回调机制正确传递解数据

### 线程管理优化
- 使用平衡分配策略进行线程分区
- 优化线程间的负载均衡
- 确保高并发下的稳定性能

### 与abcmint-master完全兼容
- 修复了所有与abcmint-master实现不一致的地方
- 确保正确处理高64位解的存储和验证
- 优化了方程系统的三维结构处理

这些更新确保了ABCMINT-CKPOOL可以与abcmint-master无缝集成，正确执行Rainbow18算法的挖矿和验证功能。
```

### 8. 矿工连接指南
矿工可以使用以下设置连接到您的矿池：
- URL: stratum+tcp://您的服务器IP:3333
- 用户名: 矿工的ABCMINT地址
- 密码: 任意（可选，用于标识矿工）

---
设计：

架构：

- 底层手写架构，仅依赖基本的标准C库函数之外的最小外部库，以实现最大灵活性和最小开销，可以在Windows和Linux系统上构建和部署。

- 多进程+多线程设计，可扩展到大规模部署，并充分利用现代多核/多线程CPU设计。

- 内存开销最小。

- 利用超可靠的unix套接字与依赖进程通信。

- 模块化代码设计，简化进一步开发。

- 独立的库代码，可以独立于ckpool使用。

- 相同的代码可以以多种不同模式部署，设计为在同一台机器、本地局域网或远程互联网位置上相互通信。


---
部署模式：

- 简单矿池。

- 简单代理，没有其他代理解决方案与ckpool通信时固有的哈希率限制。

- 透传节点，将连接组合到单个套接字，可用于扩展到数百万客户端，并允许主池与客户端的直接通信隔离。

- 供其他软件使用的库。


---
特点：

- 与未修改的abcmintd通信，可多地点故障转移到本地或远程位置。

- 本地池实例工作器仅受操作系统资源限制，通过使用多个下游透传节点可以使其几乎无限制。

- 代理和透传模式可以设置多个上游矿池故障转移。

- 可选的份额日志记录。

- 通过从退出实例到新启动实例的套接字交接，实现几乎无缝的升级重启。

- 可配置的自定义coinbase签名。

- 可配置的即时启动和最小难度。

- 快速的 vardiff 调整，具有稳定的无限制最大难度处理。

- 区块变化时的新工作生成包含完整的abcmintd交易集，无需延迟或向矿工发送无交易工作，从而提供最佳的abcmint网络支持，并以最多的交易费用奖励矿工。

- 基于通信就绪的事件驱动通信，防止通信缓慢的客户端延迟低延迟客户端。

- 运行中客户端的Stratum消息系统。

- 准确的矿池和每客户端统计信息。

- 多个命名实例可以在同一台机器上并发运行。


---
构建：

在Windows或Linux系统上，构建abcmint-ckpool不需要基本构建工具和yasm之外的依赖项。推荐的zmq通知支持（仅限ckpool）需要安装zmq开发库。此外，还需要Rainbow18算法库支持。


使用zmq构建（首选构建，但ckproxy不需要）：

#### Linux系统：
```bash
sudo apt-get install build-essential yasm libzmq3-dev

# 确保Rainbow18算法库可用
./configure --with-rainbow18=/path/to/librainbowpro

make
```

#### Windows系统：
```bash
# 在MinGW环境中
pacman -S mingw-w64-x86_64-gcc yasm mingw-w64-x86_64-libzmq

# 确保Rainbow18算法库可用
./configure --with-rainbow18=/path/to/librainbowpro

make
```

基本构建：

#### Linux系统：
```bash
sudo apt-get install build-essential yasm

# 确保Rainbow18算法库可用
./configure --with-rainbow18=/path/to/librainbowpro

make
```

#### Windows系统：
```bash
# 在MinGW环境中
pacman -S mingw-w64-x86_64-gcc yasm

# 确保Rainbow18算法库可用
./configure --with-rainbow18=/path/to/librainbowpro

make
```

从git构建还需要autoconf、automake和pkgconf：

#### Linux系统：
```bash
sudo apt-get install build-essential yasm autoconf automake libtool libzmq3-dev pkgconf

./autogen.sh

# 确保Rainbow18算法库可用
./configure --with-rainbow18=/path/to/librainbowpro

make
```

#### Windows系统：
```bash
# 在MinGW环境中
pacman -S mingw-w64-x86_64-gcc yasm autoconf automake libtool mingw-w64-x86_64-libzmq pkgconf

./autogen.sh

# 确保Rainbow18算法库可用
./configure --with-rainbow18=/path/to/librainbowpro

make
```


二进制文件将在src/子目录中构建。生成的二进制文件将是：

ckpool - 主矿池后端，完全适配abcmint的rainbow18后量子加密算法，支持多级安全级别和动态难度调整

ckproxy - 指向ckpool的链接，自动以代理模式启动它

ckpmsg - 用于以libckpool格式向ckpool传递消息的应用程序

notifier - 设计为与abcmintd的-blocknotify一起运行的应用程序，用于通知ckpool区块变化

test_rainbow - 用于验证rainbow18_compat模块是否可以正常编译的测试程序


安装不是必需的，ckpool可以直接从构建它的目录中运行，但可以通过以下方式安装：
sudo make install


---
运行：

ckpool支持以下选项：

-c CONFIG | --config CONFIG

-g GROUP | --group GROUP

-H | --handover

-h | --help

-k | --killold

-L | --log-shares

-l LOGLEVEL | --loglevel LOGLEVEL

-N | --node

-n NAME | --name NAME

-P | --passthrough

-p | --proxy

-R | --redirector

-s SOCKDIR | --sockdir SOCKDIR

-u | --userproxy


-c <CONFIG>告诉ckpool覆盖其默认配置文件名并加载指定的配置文件。如果未指定-c，ckpool会查找ckpool.conf，在代理模式下查找ckproxy.conf，在透传模式下查找ckpassthrough.conf，在重定向器模式下查找ckredirector.conf。

-g <GROUP>将以指定的组ID启动ckpool。

-H将使ckpool尝试从具有相同名称的运行中的ckpool实例接收交接，接管其客户端监听套接字并关闭它。

-h显示上述帮助

-k将使ckpool关闭具有相同名称的现有ckpool实例，必要时终止它。否则，如果具有相同名称的实例已经在运行，ckpool将拒绝启动。

-L将在logs目录中按区块高度和工作库记录每个份额信息。

-l <LOGLEVEL将日志级别更改为指定的级别。默认是5，最大调试级别是7。

-N将以透传节点模式启动ckpool，它的行为类似于透传，但需要本地运行的abcmintd，并且除了将份额传回上游矿池外，还可以自己提交区块。它还监控哈希率，比简单的透传需要更多资源。请注意，上游矿池必须指定接受传入节点请求的专用IP/端口，使用下面描述的nodeserver指令。

-n <NAME>将ckpool进程名称更改为指定的名称，允许多个不同命名的实例运行。默认情况下使用变体名称：ckpool、ckproxy、ckpassthrough、ckredirector、cknode。

-P将以透传代理模式启动ckpool，它将所有传入连接合并，并在单个连接上将所有信息流式传输到ckproxy.conf中指定的上游矿池。下游用户在主矿池上都保留其个人身份。隐含独立模式。

-p将以代理模式启动ckpool，它看起来像一个处理客户端作为单独实体的本地矿池，同时将份额作为单个用户呈现给指定的上游矿池。请注意，上游矿池需要是ckpool才能扩展到大型哈希率。独立模式是可选的。

-R将以透传模式的变体启动ckpool。它被设计为前端，用于过滤掉从不贡献任何份额的用户。一旦检测到来自上游矿池的已接受份额，它将向配置文件中的redirecturl条目中的一个发出重定向。如果存在多个条目，它将循环遍历条目，但尝试保持来自同一IP的所有客户端重定向到同一矿池。

-s <SOCKDIR>告诉ckpool将其自己的通信套接字放在哪个目录（默认为/tmp）

-u用户代理模式将按照上面的-p选项以代理模式启动ckpool，但除此之外，它还将接受来自stratum连接的用户名/密码，并尝试使用这些凭据打开与配置文件中指定的上游矿池的额外连接，然后重新连接矿工，让他们使用自己选择的用户名/密码向上游矿池挖矿。


ckpmsg和notifier支持-n、-p和-s选项

---
配置

在ckpool模式下，至少需要一个abcmintd，最低要求是设置了server、rpcuser和rpcpassword。

Ckpool默认在ckpool.conf中采用json编码的配置文件，在代理或透传模式下使用ckproxy.conf，除非使用-c指定。源代码中包含了ckpool和ckproxy的示例配置。有效json之后的条目将被忽略，可以在那里使用空格添加注释。识别的选项如下：


"btcd"：这是abcmintd的数组，包含与配置的abcmintd匹配的url、auth和pass选项。可选的布尔字段notify告诉ckpool这个abcmintd正在使用notifier，不需要轮询区块变化。如果未指定btcd，ckpool将在localhost:8882上查找一个，用户名为"user"，密码为"pass"。

"proxy"：这是与上面btcd相同格式的数组，但在代理和透传模式下用于设置上游矿池，并且是必需的。

"btcaddress"：这是尝试生成区块的abcmint地址（以'8'开头）。

"btcsig"：这是放入已挖矿区块coinbase的可选签名。

"blockpoll"：这是检查新网络区块频率的毫秒数，默认值为100。它仅用于notifier未设置时的备份，并且仅在abcmintd上未设置"notify"字段时才轮询。

"donation"：区块奖励的可选百分比捐款，用于资助ckpool开发者进一步开发和维护代码。采用浮点值，如果未设置，默认为零。

"nodeserver"：这采用与serverurl数组相同的格式，并指定要绑定的附加IP/端口，以接受挖矿节点通信的传入请求。建议选择性隔离此地址，以最小化与未授权节点的不必要通信。

"nonce1length"：这是可选的，允许extranonce1长度从2到8中选择。默认4

"nonce2length"：这是可选的，允许extranonce2长度从2到8中选择。默认8

"update_interval"：这是向矿工发送stratum更新的频率，默认设置为30秒，以帮助abcmint网络的交易延续，促进网络健康。

"version_mask"：这是客户端可以更改版本号中哪些位的掩码，表示为十六进制字符串。abcmint使用"FFFFFF00"作为默认值，允许矿工修改低8位。

"serverurl"：这是尝试将ckpool唯一绑定到的IP，如果不指定，它将尝试在池模式下默认绑定到所有接口的3333端口，在代理模式下默认绑定到3334端口。可以通过IP或可解析的域名将多个条目指定为数组，但可执行文件必须能够绑定到所有这些条目，并且1024以下的端口通常需要特权访问。

"redirecturl"：这是ckpool在重定向器模式下将活动矿工重定向到的URL数组。它们必须是有效的可解析URL+端口。

"mindiff"：vardiff允许矿工降至的最小难度。默认1

"startdiff"：新客户端获得的起始难度。abcmint建议使用100作为默认值，以适应rainbow18算法。

"maxdiff"：可选的vardiff将限制的最大难度，其中零表示无最大值。

"logdir"：存储矿池和客户端日志的目录。默认"logs"

"maxclients"：ckpool在拒绝更多客户端之前接受的客户端数量的可选上限。

"zmqblock"：用于zmq区块哈希通知的可选接口 - 仅限ckpool。需要使用匹配的abcmintd -zmqpubhashblock选项。
默认：tcp://127.0.0.1:28882

---
ABCMINT特殊配置说明：

1. 区块版本：abcmint使用区块版本号101（ABCMINT_BLOCK_VERSION），ckpool已配置为强制使用此版本。系统会自动检测区块是否为Rainbow18格式，并进行相应处理。

2. 挖矿算法：abcmint使用rainbow18后量子加密算法，而非比特币的SHA-256算法。ckpool已完全适配此算法，支持签名验证和难度计算。

3. 地址格式：abcmint地址以'8'开头，与比特币地址格式不同。

4. RPC端口：abcmint主网默认RPC端口为8882，测试网为18882。

5. 版本掩码：abcmint使用0xFFFFFF00作为默认版本掩码，允许矿工修改低8位。

6. 起始难度：为适配rainbow18算法，建议将startdiff设置为100。

7. 不支持Segwit：abcmint不使用Segwit，相关功能已在代码中禁用。

8. Rainbow18安全级别：
   - 系统支持多级Rainbow18安全级别（1-3）
   - 不同区块高度可能使用不同安全级别，系统会自动根据区块高度调整
   - 可以在配置文件中通过"rainbow18_level"参数手动指定默认安全级别

9. Rainbow18特定配置项：
   - "rainbow18_level"：Rainbow18算法安全级别（1-3）
   - "rainbow18_timeout"：签名验证超时时间（毫秒），默认为5000

10. 量子抗性挖矿：
    - ckpool包含完整的quantum_resistant模块，支持抗量子挖矿验证
    - 实现了基于区块高度的动态难度调整机制
    - 支持系数矩阵生成和解决方案验证
    - 完全适配abcmint-master中的挖矿算法实现

11. 挖矿算法适配更新：
    - 修复了exfes函数实现，与abcmint-master完全兼容
    - 重写了Merge_Solution回调函数，确保解的存储格式和掩码应用一致
    - 更新了线程分区算法，使用平衡分配策略
    - 优化了区块检查逻辑，使用直接指针比较
    - 修复了重复的GetBlockHeight函数定义

12. 错误处理增强：
    - 新增RAINBOW_ERR_系列错误码，提供详细的错误信息
    - 优化了签名验证失败的错误反馈机制
    - 增加了详细的日志记录，便于问题排查
    - 添加了跨平台兼容的日志宏定义（LOGERROR, LOGINFO, LOGWARN, LOGDEBUG）

13. 性能优化：
    - 实现了基于安全级别的动态内存分配
    - 优化了哈希计算和签名验证逻辑
    - 添加了并行验证支持，提高处理效率
    - 简化了方程复制和收缩逻辑，提高计算效率
    - 实现了高效的内存管理，避免内存泄漏
    - 添加了适当的NULL检查，提高代码健壮性

---
故障排除指南：

### Rainbow18相关问题
1. **签名验证失败错误**：
   - 检查区块高度与配置的rainbow18_level是否匹配
   - 验证librainbowpro库版本是否兼容
   - 检查日志中的RAINBOW_ERR_错误码，根据具体错误类型排查
   - 确保sha256和sha512函数正确实现或链接

2. **内存使用过高**：
   - 调整rainbow18_level参数，较低的安全级别使用更少内存
   - 增加系统可用内存或减少并发连接数
   - 检查是否有内存泄漏问题，特别是在高并发情况下

3. **验证超时**：
   - 增加配置文件中的rainbow18_timeout参数值
   - 检查系统负载，确保有足够的CPU资源用于验证
   - 在Windows平台上，考虑调整系统性能设置

4. **构建错误**：
   - 确保正确指定了librainbowpro库路径
   - 检查是否安装了所有必需的依赖项
   - 验证configure命令中的--with-rainbow18参数是否正确
   - 对于Windows平台，确保已正确配置编译环境
   - 检查是否有重复定义的函数或变量

5. **区块提交失败**：
   - 检查abcmintd连接配置是否正确
   - 验证区块版本是否为101
   - 确认钱包地址格式正确（以'8'开头）
   - 检查quantum_resistant.c中的区块检查逻辑是否与最新的abcmint-master兼容
   - 确保hashBlock成员访问正确，必要时使用phashBlock

### Windows平台特定问题
1. **编译错误**：
   - 确保使用兼容的编译器版本
   - 移除代码中的平台特定依赖
   - 使用标准C库函数替代Windows特定函数

2. **运行时问题**：
   - 确保所有DLL文件都在正确位置
   - 检查防火墙设置，允许必要的网络连接
   - 使用管理员权限运行程序（如果需要）

### 日志分析
ABCMINT-CKPOOL提供了详细的日志记录，特别是关于Rainbow18验证的信息：
- 查看logs目录下的日志文件
- 使用-l 7参数启用最大调试级别
- 关注包含"rainbow18"、"verify"、"quantum"等关键词的日志条目

### 性能调优
对于高负载矿池，建议：
1. 增加服务器内存，尤其是使用高安全级别时
2. 调整rainbow18_timeout参数以平衡验证速度和准确性
3. 考虑使用多节点部署分散负载
4. 监控系统资源使用情况，确保CPU和内存充足
5. 在Windows平台上，考虑使用性能模式运行，关闭不必要的服务
6. 根据实际负载调整线程数和连接池大小
7. 定期检查日志，及时发现和解决潜在问题
