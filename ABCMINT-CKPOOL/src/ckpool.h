/*
 * 版权所有 2014-2018,2023 Con Kolivas
 * 适配abcmint区块链网络
 *
 * 本程序是自由软件；您可以按照自由软件基金会发布的GNU通用公共许可证（第3版或更高版本）
 * 的条款重新分发和/或修改它。详情请参阅COPYING文件。
 */

#ifndef CKPOOL_H
#define CKPOOL_H

#include "config.h"

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "libckpool.h"
#include "uthash.h"

#define RPC_TIMEOUT 60 // RPC超时时间（秒）

/* abcmint相关配置常量 */
#define ABCMINT_RPC_PORT_MAIN 8882 // abcmint主网默认RPC端口
#define ABCMINT_RPC_PORT_TEST 18882 // abcmint测试网默认RPC端口
#define ABCMINT_BLOCK_VERSION 101 // abcmint当前区块版本号
#define ABCMINT_VERSION_MASK 0xFFFFFF00 // abcmint版本掩码，允许矿工修改低8位

struct ckpool_instance; // ckpool实例结构体
typedef struct ckpool_instance ckpool_t; // ckpool实例类型定义

struct ckmsg { // 消息结构体
	struct ckmsg *next; // 指向下一个消息
	struct ckmsg *prev; // 指向上一个消息
	void *data; // 消息数据
};

typedef struct ckmsg ckmsg_t; // 消息类型定义

typedef struct unix_msg unix_msg_t; // Unix消息类型定义

struct unix_msg { // Unix消息结构体
	unix_msg_t *next; // 指向下一个消息
	unix_msg_t *prev; // 指向上一个消息
	int sockd; // 套接字描述符
	char *buf; // 消息缓冲区
};

struct ckmsgq { // 消息队列结构体
	ckpool_t *ckp; // ckpool实例指针
	char name[16]; // 队列名称
	pthread_t pth; // 处理线程
	mutex_t *lock; // 互斥锁
	pthread_cond_t *cond; // 条件变量
	ckmsg_t *msgs; // 消息列表
	void (*func)(ckpool_t *, void *); // 消息处理函数
	int64_t messages; // 消息计数
	bool active; // 是否活跃
};

typedef struct ckmsgq ckmsgq_t; // 消息队列类型定义

typedef struct proc_instance proc_instance_t; // 进程实例类型定义

struct proc_instance { // 进程实例结构体
	ckpool_t *ckp; // ckpool实例指针
	unixsock_t us; // Unix套接字
	char *processname; // 进程名称
	char *sockname; // 套接字名称
	int pid; // 进程ID
	int oldpid; // 旧进程ID
	pthread_t pth_process; // 进程线程

	/* 接收消息的链表，锁和条件变量 */
	unix_msg_t *unix_msgs; // 接收到的消息链表
	mutex_t rmsg_lock; // 消息锁
	pthread_cond_t rmsg_cond; // 消息条件变量
};

struct connsock { // 连接套接字结构体
	int fd; // 文件描述符
	char *url; // URL地址
	char *port; // 端口
	char *auth; // 认证信息

	char *buf; // 缓冲区
	int bufofs; // 缓冲区偏移量
	int buflen; // 缓冲区长度
	int bufsize; // 缓冲区大小
	int rcvbufsiz; // 接收缓冲区大小
	int sendbufsiz; // 发送缓冲区大小

	ckpool_t *ckp; // ckpool实例指针
	/* 用于序列化请求/响应的信号量 */
	sem_t sem; // 信号量

	bool alive; // 是否活跃
};

typedef struct connsock connsock_t; // 连接套接字类型定义

typedef struct char_entry char_entry_t; // 字符条目类型定义

struct char_entry { // 字符条目结构体
	char_entry_t *next; // 指向下一个条目
	char_entry_t *prev; // 指向上一个条目
	char *buf; // 缓冲区
};

typedef struct log_entry log_entry_t; // 日志条目类型定义

struct log_entry { // 日志条目结构体
	log_entry_t *next; // 指向下一个日志
	log_entry_t *prev; // 指向上一个日志
	char *fname; // 文件名
	char *buf; // 日志内容
};

struct server_instance { // 服务器实例结构体
	/* 哈希表数据 */
	UT_hash_handle hh; // 哈希表句柄
	int id; // 服务器ID

	char *url; // URL地址
	char *auth; // 认证信息
	char *pass; // 密码
	bool notify; // 是否通知
	bool alive; // 是否活跃
	connsock_t cs; // 连接套接字
};

typedef struct server_instance server_instance_t; // 服务器实例类型定义

struct ckpool_instance { // ckpool实例结构体
	/* 启动时间 */
	time_t starttime; // 启动时间
	/* 启动进程ID */
	pid_t startpid; // 启动进程ID
	/* 初始命令行参数 */
	char **initial_args; // 初始命令行参数
	/* 参数数量 */
	int args; // 参数数量
	/* 配置文件名 */
	char *config; // 配置文件名
	/* 是否终止同名旧实例 */
	bool killold; // 终止旧实例标志
	/* 是否记录份额 */
	bool logshares; // 记录份额标志
	/* 日志级别 */
	int loglevel; // 日志级别
	/* 主进程名称 */
	char *name; // 进程名称
	/* 创建套接字的目录 */
	char *socket_dir; // 套接字目录
	/* Unix套接字的组ID */
	char *grpnam; // 组名称
	gid_t gr_gid; // 组ID
	/* 写入日志的目录 */
	char *logdir; // 日志目录
	/* 日志文件 */
	char *logfilename; // 日志文件名
	FILE *logfp; // 日志文件指针
	int logfd; // 日志文件描述符
	time_t lastopen_t; // 最后打开时间
	/* 如果从运行进程继承连接器文件描述符 */
	int *oldconnfd; // 旧连接器文件描述符
	/* 是否继承运行实例的套接字并关闭它 */
	bool handover; // 交接标志
	/* 在拒绝之前接受的最大客户端数量 */
	int maxclients; // 最大客户端数

	/* API消息队列 */
	ckmsgq_t *ckpapi; // API消息队列

	/* 日志消息队列 */
	ckmsgq_t *logger; // 日志消息队列
	ckmsgq_t *console_logger; // 控制台日志消息队列

	/* 父/子进程的进程实例数据 */
	proc_instance_t main; // 主进程实例

	proc_instance_t generator; // 生成器进程实例
	proc_instance_t stratifier; // 分层器进程实例
	proc_instance_t connector; // 连接器进程实例

	bool generator_ready; // 生成器是否就绪
	bool stratifier_ready; // 分层器是否就绪
	bool connector_ready; // 连接器是否就绪

	/* ZMQ区块通知使用的协议名称 */
	char *zmqblock; // ZMQ区块通知协议名称

	/* 主进程的线程 */
	pthread_t pth_listener; // 监听器线程
	pthread_t pth_watchdog; // 看门狗线程

	/* 是否运行在可信远程节点模式 */
	bool remote; // 远程模式标志

	/* 是否运行在节点代理模式 */
	bool node; // 节点模式标志

	/* 是否运行在直通模式 */
	bool passthrough; // 直通模式标志

	/* 是否为重定向直通模式 */
	bool redirector; // 重定向标志

	/* 是否运行作为代理 */
	bool proxy; // 代理模式标志

	// Solo挖矿模式已删除 for 只保留矿池模式操作

	/* 是否运行在用户代理模式 */
	bool userproxy; // 用户代理模式标志

	/* 是否将ckpool进程守护进程化 */
	bool daemon; // 守护进程标志

	/* 是否禁用动画指示器 */
	bool quiet; // 安静模式标志

	/* 是否已发出无法增加缓冲区大小的警告 */
	bool wmem_warn; // 写入内存警告标志
	bool rmem_warn; // 读取内存警告标志

	/* abcmint守护进程数据 */
	int btcds; // abcmint守护进程数量
	char **btcdurl; // abcmint守护进程URL数组
	char **btcdauth; // abcmint守护进程认证信息数组
	char **btcdpass; // abcmint守护进程密码数组
	bool *btcdnotify; // abcmint守护进程通知标志数组
	int blockpoll; // 轮询abcmint守护进程区块更新的频率（毫秒）
	int nonce1length; // 额外nonce1长度
	int nonce2length; // 额外nonce2长度

	/* 难度设置 */
	int64_t mindiff; // 最小难度，默认1
	int64_t startdiff; // 起始难度，默认42
	int64_t highdiff; // 高难度，默认1000000
	int64_t maxdiff; // 最大难度，无默认值

	/* coinbase数据 */
	char *btcaddress; // 挖矿地址
	bool script; // 地址是否为脚本地址
	bool segwit; // 地址是否为隔离见证地址（abcmint不使用segwit）
	char *btcsig; // 可选的添加到coinbase的签名
	bool coinbase_valid; // Coinbase交易确认有效

	/* 捐赠数据 */
	char *donaddress; // 捐赠地址
	char *tndonaddress; // 测试网捐赠地址
	char *rtdonaddress; // 回归测试网捐赠地址
	bool donscript; // 捐赠是否为脚本
	bool donsegwit; // 捐赠是否为隔离见证（abcmint不使用segwit）
	bool donvalid; // 捐赠地址在此网络上是否有效
	double donation; // 开发捐赠百分比

	/* Stratum选项 */
	server_instance_t **servers; // 服务器实例数组
	char **serverurl; // 服务器/代理绑定的URL数组
	int serverurls; // 服务器绑定数量
	bool *server_highdiff; // 此服务器是否为高难度
	bool *nodeserver; // 此服务器URL是否提供节点信息
	int nodeservers; // 此服务器是否有远程节点服务器
	bool *trusted; // 此服务器URL是否接受可信远程节点
	char *upstream; // 可信远程模式中的上游矿池

	int update_interval; // Stratum更新间隔（秒）

	uint32_t version_mask; // 设为true的位表示允许矿工修改这些位，abcmint使用ABCMINT_VERSION_MASK

	/* 代理选项 */
	int proxies; // 代理数量
	char **proxyurl; // 代理URL数组
	char **proxyauth; // 代理认证信息数组
	char **proxypass; // 代理密码数组

	/* 直通重定向选项 */
	int redirecturls; // 重定向URL数量
	char **redirecturl; // 重定向URL数组
	char **redirectport; // 重定向端口数组

	/* 每个进程的私有数据 */
	void *gdata; // 生成器私有数据
	void *sdata; // 分层器私有数据
	void *cdata; // 连接器私有数据
};

enum stratum_msgtype { // Stratum消息类型枚举
	SM_RECONNECT = 0, // 重新连接
	SM_DIFF, // 难度
	SM_MSG, // 消息
	SM_UPDATE, // 更新
	SM_ERROR, // 错误
	SM_SUBSCRIBE, // 订阅
	SM_SUBSCRIBERESULT, // 订阅结果
	SM_SHARE, // 份额
	SM_SHARERESULT, // 份额结果
	SM_AUTH, // 认证
	SM_AUTHRESULT, // 认证结果
	SM_TXNS, // 交易
	SM_TXNSRESULT, // 交易结果
	SM_PING, // Ping
	SM_WORKINFO, // 工作信息
	SM_SUGGESTDIFF, // 建议难度
	SM_BLOCK, // 区块
	SM_PONG, // Pong
	SM_TRANSACTIONS, // 交易
	SM_SHAREERR, // 份额错误
	SM_WORKERSTATS, // 矿工统计
	SM_REQTXNS, // 请求交易
	SM_CONFIGURE, // 配置
	SM_NONE // 无
};

static const char __maybe_unused *stratum_msgs[] = { // Stratum消息名称数组
	"reconnect", // 重新连接
	"diff", // 难度
	"message", // 消息
	"update", // 更新
	"error", // 错误
	"subscribe", // 订阅
	"subscribe.result", // 订阅结果
	"share", // 份额
	"share.result", // 份额结果
	"auth", // 认证
	"auth.result", // 认证结果
	"txns", // 交易
	"txns.result", // 交易结果
	"ping", // Ping
	"workinfo", // 工作信息
	"suggestdiff", // 建议难度
	"block", // 区块
	"pong", // Pong
	"transactions", // 交易
	"shareerr", // 份额错误
	"workerstats", // 矿工统计
	"reqtxns", // 请求交易
	"mining.configure", // 挖矿配置
	"" // 空
};

#define SAFE_HASH_OVERHEAD(HASHLIST) (HASHLIST ? HASH_OVERHEAD(hh, HASHLIST) : 0) // 安全哈希开销宏

void get_timestamp(char *stamp); // 获取时间戳函数声明

ckmsgq_t *create_ckmsgq(ckpool_t *ckp, const char *name, const void *func); // 创建消息队列函数声明
ckmsgq_t *create_ckmsgqs(ckpool_t *ckp, const char *name, const void *func, const int count); // 创建多个消息队列函数声明
bool _ckmsgq_add(ckmsgq_t *ckmsgq, void *data, const char *file, const char *func, const int line); // 向消息队列添加消息（内部函数）
#define ckmsgq_add(ckmsgq, data) _ckmsgq_add(ckmsgq, data, __FILE__, __func__, __LINE__) // 向消息队列添加消息宏
bool ckmsgq_empty(ckmsgq_t *ckmsgq); // 检查消息队列是否为空函数声明
unix_msg_t *get_unix_msg(proc_instance_t *pi); // 获取Unix消息函数声明

bool ping_main(ckpool_t *ckp); // Ping主进程函数声明
void empty_buffer(connsock_t *cs); // 清空缓冲区函数声明
int set_sendbufsize(ckpool_t *ckp, const int fd, const int len); // 设置发送缓冲区大小函数声明
int set_recvbufsize(ckpool_t *ckp, const int fd, const int len); // 设置接收缓冲区大小函数声明
int read_socket_line(connsock_t *cs, float *timeout); // 从套接字读取一行函数声明
void _queue_proc(proc_instance_t *pi, const char *msg, const char *file, const char *func, const int line); // 向进程队列发送消息（内部函数）
#define send_proc(pi, msg) _queue_proc(&(pi), msg, __FILE__, __func__, __LINE__) // 向进程发送消息宏
char *_send_recv_proc(const proc_instance_t *pi, const char *msg, int writetimeout, int readtimedout,
		      const char *file, const char *func, const int line); // 发送并接收进程消息（内部函数）
#define send_recv_proc(pi, msg) _send_recv_proc(&(pi), msg, UNIX_WRITE_TIMEOUT, UNIX_READ_TIMEOUT, __FILE__, __func__, __LINE__) // 发送并接收进程消息宏
char *_send_recv_ckdb(const ckpool_t *ckp, const char *msg, const char *file, const char *func, const int line); // 发送并接收ckdb消息（内部函数）
#define send_recv_ckdb(ckp, msg) _send_recv_ckdb(ckp, msg, __FILE__, __func__, __LINE__) // 发送并接收ckdb消息宏
char *_ckdb_msg_call(const ckpool_t *ckp, const char *msg,  const char *file, const char *func,
		 const int line); // ckdb消息调用（内部函数）
#define ckdb_msg_call(ckp, msg) _ckdb_msg_call(ckp, msg, __FILE__, __func__, __LINE__) // ckdb消息调用宏

json_t *json_rpc_call(connsock_t *cs, const char *rpc_req); // JSON-RPC调用函数声明
json_t *json_rpc_response(connsock_t *cs, const char *rpc_req); // JSON-RPC响应函数声明
void json_rpc_msg(connsock_t *cs, const char *rpc_req); // JSON-RPC消息函数声明
bool _send_json_msg(connsock_t *cs, const json_t *json_msg, const char *file, const char *func, const int line); // 发送JSON消息（内部函数）
#define send_json_msg(CS, JSON_MSG) _send_json_msg(CS, JSON_MSG, __FILE__, __func__, __LINE__) // 发送JSON消息宏
json_t *json_msg_result(const char *msg, json_t **res_val, json_t **err_val); // JSON消息结果函数声明

bool json_get_string(char **store, const json_t *val, const char *res); // 从JSON获取字符串函数声明
bool json_get_int64(int64_t *store, const json_t *val, const char *res); // 从JSON获取int64函数声明
bool json_get_int(int *store, const json_t *val, const char *res); // 从JSON获取int函数声明
bool json_get_double(double *store, const json_t *val, const char *res); // 从JSON获取double函数声明
bool json_get_uint32(uint32_t *store, const json_t *val, const char *res); // 从JSON获取uint32函数声明
bool json_get_bool(bool *store, const json_t *val, const char *res); // 从JSON获取bool函数声明
bool json_getdel_int(int *store, json_t *val, const char *res); // 从JSON获取并删除int函数声明
bool json_getdel_int64(int64_t *store, json_t *val, const char *res); // 从JSON获取并删除int64函数声明


/* 未来API实现的占位符 */
typedef struct apimsg apimsg_t; // API消息类型定义

struct apimsg { // API消息结构体
	char *buf; // 消息缓冲区
	int sockd; // 套接字描述符
};

static inline void ckpool_api(ckpool_t __maybe_unused *ckp, apimsg_t __maybe_unused *apimsg) {}; // ckpool API函数（占位符）
static inline json_t *json_encode_errormsg(json_error_t __maybe_unused *err_val) { return NULL; }; // 编码JSON错误消息函数（占位符）
static inline json_t *json_errormsg(const char __maybe_unused *fmt, ...) { return NULL; }; // JSON错误消息函数（占位符）
static inline void send_api_response(json_t __maybe_unused *val, const int __maybe_unused sockd) {}; // 发送API响应函数（占位符）

/* 子客户端在高32位有client_ids。如果存在父客户端，则返回其父客户端的值。 */
static inline int64_t subclient(const int64_t client_id)
{
	return (client_id >> 32); // 返回父客户端ID
}

#endif /* CKPOOL_H */ // 文件结束宏
