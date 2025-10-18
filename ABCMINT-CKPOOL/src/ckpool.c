/*
 * 版权所有 2014-2020,2023 Con Kolivas
 * 适配abcmint区块链网络
 *
 * 本程序是自由软件；您可以按照自由软件基金会发布的GNU通用公共许可证（第3版或更高版本）
 * 的条款重新分发和/或修改它。详情请参阅COPYING文件。
 */

#include "config.h"

#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <fenv.h>
#include <getopt.h>
#include <grp.h>
#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ckpool.h"
#include "libckpool.h"
#include "generator.h"
#include "stratifier.h"
#include "connector.h"

ckpool_t *global_ckp;

static bool open_logfile(ckpool_t *ckp)
{
	if (ckp->logfd > 0) {
		flock(ckp->logfd, LOCK_EX);
		fflush(ckp->logfp);
		Close(ckp->logfd);
	}
	ckp->logfp = fopen(ckp->logfilename, "ae");
	if (unlikely(!ckp->logfp)) {
		LOGEMERG("无法打开日志文件 %s", ckp->logfilename);
		return false;
	}
	/* 设置日志为行缓冲模式 */
	setvbuf(ckp->logfp, NULL, _IOLBF, 0);
	ckp->logfd = fileno(ckp->logfp);
	ckp->lastopen_t = time(NULL);
	return true;
}

/* 使用ckmsgqs进行控制台和文件日志记录，以防止logmsg在任何延迟时阻塞。 */
static void console_log(ckpool_t __maybe_unused *ckp, char *msg)
{
	/* 只有当stderr指向控制台时才添加清除行 */
	if (isatty(fileno(stderr)))
		fprintf(stderr, "\33[2K\r");
	fprintf(stderr, "%s", msg);
	fflush(stderr);

	free(msg);
}

static void proclog(ckpool_t *ckp, char *msg)
{
	time_t log_t = time(NULL);

	/* 每分钟重新打开日志文件，允许我们移动/重命名它并创建新的日志文件 */
	if (log_t > ckp->lastopen_t + 60) {
		LOGDEBUG("重新打开日志文件");
		open_logfile(ckp);
	}

	flock(ckp->logfd, LOCK_EX);
	fprintf(ckp->logfp, "%s", msg);
	flock(ckp->logfd, LOCK_UN);

	free(msg);
}

void get_timestamp(char *stamp)
{
	struct tm tm;
	tv_t now_tv;
	int ms;

	tv_time(&now_tv);
	ms = (int)(now_tv.tv_usec / 1000);
	localtime_r(&(now_tv.tv_sec), &tm);
	sprintf(stamp, "[%d-%02d-%02d %02d:%02d:%02d.%03d]",
			tm.tm_year + 1900,
			tm.tm_mon + 1,
			tm.tm_mday,
			tm.tm_hour,
			tm.tm_min,
			tm.tm_sec, ms);
}

/* 将所有内容记录到日志文件，但同时在控制台上显示警告信息 */
void logmsg(int loglevel, const char *fmt, ...)
{
	int logfd = global_ckp->logfd;
	char *log, *buf = NULL;
	char stamp[128];
	va_list ap;

	if (global_ckp->loglevel < loglevel || !fmt)
		return;

	va_start(ap, fmt);
	VASPRINTF(&buf, fmt, ap);
	va_end(ap);

	if (unlikely(!buf)) {
		fprintf(stderr, "向logmsg发送空缓冲区\n");
		return;
	}
	if (unlikely(!strlen(buf))) {
		fprintf(stderr, "向logmsg发送零长度字符串\n");
		goto out;
	}
	get_timestamp(stamp);
	if (loglevel <= LOG_ERR && errno != 0)
		ASPRINTF(&log, "%s %s (错误码 %d: %s)\n", stamp, buf, errno, strerror(errno));
	else
		ASPRINTF(&log, "%s %s\n", stamp, buf);

	if (unlikely(!global_ckp->console_logger)) {
		fprintf(stderr, "%s", log);
		goto out_free;
	}
	if (loglevel <= LOG_WARNING)
		ckmsgq_add(global_ckp->console_logger, strdup(log));
	if (logfd > 0)
		ckmsgq_add(global_ckp->logger, strdup(log));
out_free:
	free(log);
out:
	free(buf);
}

/* 创建消息队列接收和解析线程的通用函数 */
static void *ckmsg_queue(void *arg)
{
	ckmsgq_t *ckmsgq = (ckmsgq_t *)arg;
	ckpool_t *ckp = ckmsgq->ckp;

	pthread_detach(pthread_self());
	rename_proc(ckmsgq->name);
	ckmsgq->active = true;

	while (42) {
		ckmsg_t *msg;
		tv_t now;
		ts_t abs;

		mutex_lock(ckmsgq->lock);
		tv_time(&now);
		tv_to_ts(&abs, &now);
		abs.tv_sec++;
		if (!ckmsgq->msgs)
			cond_timedwait(ckmsgq->cond, ckmsgq->lock, &abs);
		msg = ckmsgq->msgs;
		if (msg)
			DL_DELETE(ckmsgq->msgs, msg);
		mutex_unlock(ckmsgq->lock);

		if (!msg)
			continue;
		ckmsgq->func(ckp, msg->data);
		free(msg);
	}
	return NULL;
}

ckmsgq_t *create_ckmsgq(ckpool_t *ckp, const char *name, const void *func)
{
	ckmsgq_t *ckmsgq = ckzalloc(sizeof(ckmsgq_t));

	strncpy(ckmsgq->name, name, 15);
	ckmsgq->func = func;
	ckmsgq->ckp = ckp;
	ckmsgq->lock = ckalloc(sizeof(mutex_t));
	ckmsgq->cond = ckalloc(sizeof(pthread_cond_t));
	mutex_init(ckmsgq->lock);
	cond_init(ckmsgq->cond);
	create_pthread(&ckmsgq->pth, ckmsg_queue, ckmsgq);

	return ckmsgq;
}

ckmsgq_t *create_ckmsgqs(ckpool_t *ckp, const char *name, const void *func, const int count)
{
	ckmsgq_t *ckmsgq = ckzalloc(sizeof(ckmsgq_t) * count);
	mutex_t *lock;
	pthread_cond_t *cond;
	int i;

	lock = ckalloc(sizeof(mutex_t));
	cond = ckalloc(sizeof(pthread_cond_t));
	mutex_init(lock);
	cond_init(cond);

	for (i = 0; i < count; i++) {
		snprintf(ckmsgq[i].name, 15, "%.6s%x", name, i);
		ckmsgq[i].func = func;
		ckmsgq[i].ckp = ckp;
		ckmsgq[i].lock = lock;
		ckmsgq[i].cond = cond;
		create_pthread(&ckmsgq[i].pth, ckmsg_queue, &ckmsgq[i]);
	}

	return ckmsgq;
}

/* 将消息添加到ckmsgq链表并通知ckmsgq解析线程唤醒并处理它的通用函数。 */
bool _ckmsgq_add(ckmsgq_t *ckmsgq, void *data, const char *file, const char *func, const int line)
{
	ckmsg_t *msg;

	if (unlikely(!ckmsgq)) {
		LOGWARNING("从%s %s:%d向无效队列发送消息", file, func, line);
		/* 如果不幸发送到启动期间未设置的消息队列，则丢弃数据 */
		free(data);
		return false;
	}
	while (unlikely(!ckmsgq->active))
		cksleep_ms(10);

	msg = ckalloc(sizeof(ckmsg_t));
	msg->data = data;

	mutex_lock(ckmsgq->lock);
	ckmsgq->messages++;
	DL_APPEND(ckmsgq->msgs, msg);
	pthread_cond_broadcast(ckmsgq->cond);
	mutex_unlock(ckmsgq->lock);

	return true;
}

/* 返回ckmsgq链表中是否有任何排队的消息。 */
bool ckmsgq_empty(ckmsgq_t *ckmsgq)
{
	bool ret = true;

	if (unlikely(!ckmsgq || !ckmsgq->active))
		goto out;

	mutex_lock(ckmsgq->lock);
	if (ckmsgq->msgs)
		ret = (ckmsgq->msgs->next == ckmsgq->msgs->prev);
	mutex_unlock(ckmsgq->lock);
out:
	return ret;
}

/* 创建一个独立线程，为进程实例排队接收的unix消息，并将它们添加到带有相关接收套接字的接收消息链表中，
 * 然后通知进程的rmsg_cond，表示我们有更多排队的消息。unix_msg_t内存必须由从链表中移除条目的代码释放。 */
static void *unix_receiver(void *arg)
{
	proc_instance_t *pi = (proc_instance_t *)arg;
	int rsockd = pi->us.sockd, sockd;
	char qname[16];

	sprintf(qname, "%cunixrq", pi->processname[0]);
	rename_proc(qname);
	pthread_detach(pthread_self());

	while (42) {
		unix_msg_t *umsg;
		char *buf;

		sockd = accept(rsockd, NULL, NULL);
		if (unlikely(sockd < 0)) {
			LOGEMERG("无法在%s套接字上接受连接，退出", qname);
			break;
		}
		buf = recv_unix_msg(sockd);
		if (unlikely(!buf)) {
			Close(sockd);
			LOGWARNING("无法在%s套接字上获取消息", qname);
			continue;
		}
		umsg = ckalloc(sizeof(unix_msg_t));
		umsg->sockd = sockd;
		umsg->buf = buf;

		mutex_lock(&pi->rmsg_lock);
		DL_APPEND(pi->unix_msgs, umsg);
		pthread_cond_signal(&pi->rmsg_cond);
		mutex_unlock(&pi->rmsg_lock);
	}

	return NULL;
}

/* 获取接收队列中的下一条消息，或最多等待5秒获取下一条消息，如果在该时间内未收到消息则返回NULL。 */
unix_msg_t *get_unix_msg(proc_instance_t *pi)
{
	unix_msg_t *umsg;

	mutex_lock(&pi->rmsg_lock);
	if (!pi->unix_msgs) {
		tv_t now;
		ts_t abs;

		tv_time(&now);
		tv_to_ts(&abs, &now);
		abs.tv_sec += 5;
		cond_timedwait(&pi->rmsg_cond, &pi->rmsg_lock, &abs);
	}
	umsg = pi->unix_msgs;
	if (umsg)
		DL_DELETE(pi->unix_msgs, umsg);
	mutex_unlock(&pi->rmsg_lock);

	return umsg;
}

static void create_unix_receiver(proc_instance_t *pi)
{
	pthread_t pth;

	mutex_init(&pi->rmsg_lock);
	cond_init(&pi->rmsg_cond);

	create_pthread(&pth, unix_receiver, pi);
}

/* 对kill调用进行合理性检查，确保我们不会向pid 0发送信号。 */
static int kill_pid(const int pid, const int sig)
{
	if (pid < 1)
		return -1;
	return kill(pid, sig);
}

static int pid_wait(const pid_t pid, const int ms)
{
	tv_t start, now;
	int ret;

	tv_time(&start);
	do {
		ret = kill_pid(pid, 0);
		if (ret)
			break;
		tv_time(&now);
	} while (ms_tvdiff(&now, &start) < ms);
	return ret;
}

static void api_message(ckpool_t *ckp, char **buf, int *sockd)
{
	apimsg_t *apimsg = ckalloc(sizeof(apimsg_t));

	apimsg->buf = *buf;
	*buf = NULL;
	apimsg->sockd = *sockd;
	*sockd = -1;
	ckmsgq_add(ckp->ckpapi, apimsg);
}

/* 监听传入的全局请求。尽可能始终返回响应 */
static void *listener(void *arg)
{
	proc_instance_t *pi = (proc_instance_t *)arg;
	unixsock_t *us = &pi->us;
	ckpool_t *ckp = pi->ckp;
	char *buf = NULL, *msg;
	int sockd;

	rename_proc(pi->sockname);
retry:
	dealloc(buf);
	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		LOGERR("监听器中无法接受套接字连接");
		goto out;
	}

	buf = recv_unix_msg(sockd);
	if (!buf) {
		LOGWARNING("监听器中无法获取消息");
		send_unix_msg(sockd, "failed");
	} else if (buf[0] == '{') {
		/* 收到的任何JSON消息都由RPC API处理 */
		api_message(ckp, &buf, &sockd);
	} else if (cmdmatch(buf, "shutdown")) {
		LOGWARNING("监听器收到关闭消息，终止ckpool");
		send_unix_msg(sockd, "exiting");
		goto out;
	} else if (cmdmatch(buf, "ping")) {
		LOGDEBUG("监听器收到ping请求");
		send_unix_msg(sockd, "pong");
	} else if (cmdmatch(buf, "loglevel")) {
		int loglevel;

		if (sscanf(buf, "loglevel=%d", &loglevel) != 1) {
			LOGWARNING("无法解析日志级别消息 %s", buf);
			send_unix_msg(sockd, "Failed");
		} else if (loglevel < LOG_EMERG || loglevel > LOG_DEBUG) {
			LOGWARNING("发送的日志级别%d无效", loglevel);
			send_unix_msg(sockd, "Invalid");
		} else {
			ckp->loglevel = loglevel;
			send_unix_msg(sockd, "success");
		}
	} else if (cmdmatch(buf, "getxfd")) {
		int fdno = -1;

		sscanf(buf, "getxfd%d", &fdno);
		connector_send_fd(ckp, fdno, sockd);
	} else if (cmdmatch(buf, "accept")) {
		LOGWARNING("监听器收到接受消息，正在接受客户端");
		send_proc(ckp->connector, "accept");
		send_unix_msg(sockd, "accepting");
	} else if (cmdmatch(buf, "reject")) {
		LOGWARNING("监听器收到拒绝消息，正在拒绝客户端");
		send_proc(ckp->connector, "reject");
		send_unix_msg(sockd, "rejecting");
	} else if (cmdmatch(buf, "dropall")) {
		LOGWARNING("监听器收到dropall消息，正在断开所有客户端连接");
		send_proc(ckp->stratifier, buf);
		send_unix_msg(sockd, "dropping all");
	} else if (cmdmatch(buf, "reconnect")) {
		LOGWARNING("监听器收到向客户端发送重新连接的请求");
		send_proc(ckp->stratifier, buf);
		send_unix_msg(sockd, "reconnecting");
	} else if (cmdmatch(buf, "restart")) {
		LOGWARNING("监听器收到重启消息，尝试交接");
		send_unix_msg(sockd, "restarting");
		if (!fork()) {
			if (!ckp->handover) {
				ckp->initial_args[ckp->args++] = strdup("-H");
				ckp->initial_args[ckp->args] = NULL;
			}
			execv(ckp->initial_args[0], (char *const *)ckp->initial_args);
		}
	} else if (cmdmatch(buf, "stratifierstats")) {
		LOGDEBUG("监听器收到stratifierstats请求");
		msg = stratifier_stats(ckp, ckp->sdata);
		send_unix_msg(sockd, msg);
		dealloc(msg);
	} else if (cmdmatch(buf, "connectorstats")) {
		LOGDEBUG("监听器收到connectorstats请求");
		msg = connector_stats(ckp->cdata, 0);
		send_unix_msg(sockd, msg);
		dealloc(msg);
	} else if (cmdmatch(buf, "resetshares")) {
		LOGWARNING("正在重置最佳份额");
		send_proc(ckp->stratifier, buf);
		send_unix_msg(sockd, "resetting");
	} else {
		LOGINFO("监听器收到未处理的消息: %s", buf);
		send_unix_msg(sockd, "unknown");
	}
	Close(sockd);
	goto retry;
out:
	dealloc(buf);
	close_unix_socket(us->sockd, us->path);
	return NULL;
}

void empty_buffer(connsock_t *cs)
{
	if (cs->buf)
		cs->buf[0] = '\0';
	cs->buflen = cs->bufofs = 0;
}

int set_sendbufsize(ckpool_t *ckp, const int fd, const int len)
{
	socklen_t optlen;
	int opt;

	optlen = sizeof(opt);
	opt = len * 4 / 3;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, optlen);
	getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, &optlen);
	opt /= 2;
	if (opt < len) {
		LOGDEBUG("无法以非特权方式设置所需的发送缓冲区大小%d，仅获得%d",
			 len, opt);
		optlen = sizeof(opt);
		opt = len * 4 / 3;
		setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &opt, optlen);
		getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, &optlen);
		opt /= 2;
	}
	if (opt < len) {
		LOGNOTICE("无法将发送缓冲区大小增加到%d，如果使用远程abcmintd，请增加wmem_max或以特权方式启动%s",
		   len, ckp->name);
		ckp->wmem_warn = true;
	} else
		LOGDEBUG("已将发送缓冲区大小增加到%d（期望%d）", opt, len);
	return opt;
}

int set_recvbufsize(ckpool_t *ckp, const int fd, const int len)
{
	socklen_t optlen;
	int opt;

	optlen = sizeof(opt);
	opt = len * 4 / 3;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, optlen);
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, &optlen);
	opt /= 2;
	if (opt < len) {
		LOGDEBUG("无法以非特权方式设置所需的接收缓冲区大小%d，仅获得%d",
			 len, opt);
		optlen = sizeof(opt);
		opt = len * 4 / 3;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &opt, optlen);
		getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, &optlen);
		opt /= 2;
	}
	if (opt < len) {
		LOGNOTICE("无法将接收缓冲区大小增加到%d，如果使用远程abcmintd，请增加rmem_max或以特权方式启动%s",
		   len, ckp->name);
		ckp->rmem_warn = true;
	} else
		LOGDEBUG("已将接收缓冲区大小增加到%d（期望%d）", opt, len);
	return opt;
}

/* 如果cs->buflen有任何值，则意味着在上一次通过read_socket_line时接收到了完整的一行并随后进行了处理，
 * 留下了cs->bufofs之后的未处理数据。否则，buflen为零表示只有长度为bufofs的未处理数据。 */
static void clear_bufline(connsock_t *cs)
{
	if (unlikely(!cs->buf)) {
		socklen_t optlen = sizeof(cs->rcvbufsiz);

		cs->buf = ckzalloc(PAGESIZE);
		cs->bufsize = PAGESIZE;
		getsockopt(cs->fd, SOL_SOCKET, SO_RCVBUF, &cs->rcvbufsiz, &optlen);
		cs->rcvbufsiz /= 2;
		LOGDEBUG("检测到连接套接字接收缓冲区大小为%d", cs->rcvbufsiz);
	} else if (cs->buflen) {
		memmove(cs->buf, cs->buf + cs->bufofs, cs->buflen);
		memset(cs->buf + cs->buflen, 0, cs->bufofs);
		cs->bufofs = cs->buflen;
		cs->buflen = 0;
		cs->buf[cs->bufofs] = '\0';
	}
}

static void add_buflen(ckpool_t *ckp, connsock_t *cs, const char *readbuf, const int len)
{
	int backoff = 1;
	int buflen;

	buflen = round_up_page(cs->bufofs + len + 1);
	while (cs->bufsize < buflen) {
		char *newbuf = realloc(cs->buf, buflen);

		if (likely(newbuf)) {
			cs->bufsize = buflen;
			cs->buf = newbuf;
			break;
		}
		if (backoff == 1)
			fprintf(stderr, "在read_socket_line中无法重新分配%d，重试中\n", (int)buflen);
		cksleep_ms(backoff);
		backoff <<= 1;
	}
	/* 如果可能，将接收缓冲区增加到大于我们可能缓冲的最大消息 */
	if (unlikely(!ckp->rmem_warn && buflen > cs->rcvbufsiz))
		cs->rcvbufsiz = set_recvbufsize(ckp, cs->fd, buflen);

	memcpy(cs->buf + cs->bufofs, readbuf, len);
	cs->bufofs += len;
	cs->buf[cs->bufofs] = '\0';
}

/* 非阻塞地接收当前可用的所有数据到连接套接字缓冲区。返回读取的数据总长度。 */
static int recv_available(ckpool_t *ckp, connsock_t *cs)
{
	char readbuf[PAGESIZE];
	int len = 0, ret;

	do {
		ret = recv(cs->fd, readbuf, PAGESIZE - 4, MSG_DONTWAIT);
		if (ret > 0) {
			add_buflen(ckp, cs, readbuf, ret);
			len += ret;
		}
	} while (ret > 0);

	return len;
}

/* Read from a socket into cs->buf till we get an '\n', converting it to '\0'
 * 并存储我们已接收的额外数据量，这些数据将在下一次接收时移到缓冲区开头使用。如果接收到整行，则返回行长度；
 * 如果接收了无EOL的部分数据或无数据，则返回零；错误时返回-1。
int read_socket_line(connsock_t *cs, float *timeout)
{
	ckpool_t *ckp = cs->ckp;
	bool quiet = ckp->proxy | ckp->remote;
	char *eom = NULL;
	tv_t start, now;
	float diff;
	int ret;

	clear_bufline(cs);
	recv_available(ckp, cs); // Intentionally ignore return value
	eom = memchr(cs->buf, '\n', cs->bufofs);

	tv_time(&start);

	while (!eom) {
		if (unlikely(cs->fd < 0)) {
			ret = -1;
			goto out;
		}

		if (*timeout < 0) {
			if (quiet)
				LOGINFO("在read_socket_line中超时");
			else
				LOGERR("在read_socket_line中超时");
			ret = 0;
			goto out;
		}
		ret = wait_read_select(cs->fd, *timeout);
		if (ret < 1) {
			if (quiet)
				LOGINFO("read_socket_line中的select %s", !ret ? "超时" : "失败");
			else
				LOGERR("read_socket_line中的select %s", !ret ? "超时" : "失败");
			goto out;
		}
		ret = recv_available(ckp, cs);
		if (ret < 1) {
			/* 如果我们已经执行了wait_read_select，那么应该有数据可读，如果没有获取到任何数据，则意味着套接字已关闭。 */
			if (quiet)
				LOGINFO("在read_socket_line中接收失败");
			else
				LOGERR("在read_socket_line中接收失败");
			ret = -1;
			goto out;
		}
		eom = memchr(cs->buf, '\n', cs->bufofs);
		tv_time(&now);
		diff = tvdiff(&now, &start);
		copy_tv(&start, &now);
		*timeout -= diff;
	}
	ret = eom - cs->buf;

	cs->buflen = cs->buf + cs->bufofs - eom - 1;
	if (cs->buflen)
		cs->bufofs = eom - cs->buf + 1;
	else
		cs->bufofs = 0;
	*eom = '\0';
out:
	if (ret < 0) {
		empty_buffer(cs);
		dealloc(cs->buf);
	}
	return ret;
}

/* 当ckpool是多进程模型时，我们曾经通过unix套接字在每个进程实例之间发送消息，但现在不再需要，
 * 因此我们可以直接将消息放在另一个进程实例的队列上，直到我们弃用此机制。 */
void _queue_proc(proc_instance_t *pi, const char *msg, const char *file, const char *func, const int line)
{
	unix_msg_t *umsg;

	if (unlikely(!msg || !strlen(msg))) {
		LOGWARNING("从%s %s:%d向queue_proc传递了空消息", file, func, line);
		return;
	}
	umsg = ckalloc(sizeof(unix_msg_t));
	umsg->sockd = -1;
	umsg->buf = strdup(msg);

	mutex_lock(&pi->rmsg_lock);
	DL_APPEND(pi->unix_msgs, umsg);
	pthread_cond_signal(&pi->rmsg_cond);
	mutex_unlock(&pi->rmsg_lock);
}

/* 向进程实例发送单个消息并检索响应，然后关闭套接字。 */
char *_send_recv_proc(const proc_instance_t *pi, const char *msg, int writetimeout, int readtimedout,
		      const char *file, const char *func, const int line)
{
	char *path = pi->us.path, *buf = NULL;
	int sockd;

	if (unlikely(!path || !strlen(path))) {
		LOGERR("尝试在send_proc中将消息%s发送到空路径", msg ? msg : "");
		goto out;
	}
	if (unlikely(!msg || !strlen(msg))) {
		LOGERR("尝试在send_proc中将空消息发送到套接字%s", path);
		goto out;
	}
	sockd = open_unix_client(path);
	if (unlikely(sockd < 0)) {
		LOGWARNING("在send_recv_proc中无法打开套接字%s", path);
		goto out;
	}
	if (unlikely(!_send_unix_msg(sockd, msg, writetimeout, file, func, line)))
		LOGWARNING("无法将%s发送到套接字%s", msg, path);
	else
		buf = _recv_unix_msg(sockd, readtimedout, readtimedout, file, func, line);
	Close(sockd);
out:
	if (unlikely(!buf))
		LOGERR("来自%s %s:%d的send_recv_proc失败", file, func, line);
	return buf;
}

static const char *rpc_method(const char *rpc_req)
{
	const char *ptr = strchr(rpc_req, ':');
	if (ptr)
		return ptr+1;
	return rpc_req;
}

/* 所有这些调用都针对abcmintd，它更喜欢打开/关闭连接而不是持久连接，所以cs->fd始终是无效的。 */
static json_t *_json_rpc_call(connsock_t *cs, const char *rpc_req, const bool info_only)
{
	float timeout = RPC_TIMEOUT;
	char *http_req = NULL;
	json_error_t err_val;
	char *warning = NULL;
	json_t *val = NULL;
	tv_t stt_tv, fin_tv;
	double elapsed;
	int len, ret;

	/* Serialise all calls in case we use cs from multiple threads */
	cksem_wait(&cs->sem);
	cs->fd = connect_socket(cs->url, cs->port);
	if (unlikely(cs->fd < 0)) {
		ASPRINTF(&warning, "无法在%s中连接套接字到%s:%s", __func__, cs->url, cs->port);
		goto out;
	}
	if (unlikely(!cs->url)) {
		ASPRINTF(&warning, "%s中没有URL", __func__);
		goto out;
	}
	if (unlikely(!cs->port)) {
		ASPRINTF(&warning, "%s中没有端口", __func__);
		goto out;
	}
	if (unlikely(!cs->auth)) {
		ASPRINTF(&warning, "%s中没有认证信息", __func__);
		goto out;
	}
	if (unlikely(!rpc_req)) {
		ASPRINTF(&warning, "向%s传递了空rpc_req", __func__);
		goto out;
	}
	len = strlen(rpc_req);
	if (unlikely(!len)) {
		ASPRINTF(&warning, "向%s传递了零长度的rpc_req", __func__);
		goto out;
	}
	http_req = ckalloc(len + 256); // Leave room for headers
	sprintf(http_req,
		 "POST / HTTP/1.1\n"
		 "Authorization: Basic %s\n"
		 "Host: %s:%s\n"
		 "Content-type: application/json\n"
		 "Content-Length: %d\n\n%s",
		 cs->auth, cs->url, cs->port, len, rpc_req);

	len = strlen(http_req);
	tv_time(&stt_tv);
	ret = write_socket(cs->fd, http_req, len);
	if (ret != len) {
		tv_time(&fin_tv);
		elapsed = tvdiff(&fin_tv, &stt_tv);
		ASPRINTF(&warning, "在%s中无法写入套接字 (%.10s...) %.3fs",
			 __func__, rpc_method(rpc_req), elapsed);
		goto out_empty;
	}
	ret = read_socket_line(cs, &timeout);
	if (ret < 1) {
		tv_time(&fin_tv);
		elapsed = tvdiff(&fin_tv, &stt_tv);
		ASPRINTF(&warning, "Failed to read socket line in %s (%.10s...) %.3fs",
			 __func__, rpc_method(rpc_req), elapsed);
		goto out_empty;
	}
	if (strncasecmp(cs->buf, "HTTP/1.1 200 OK", 15)) {
		tv_time(&fin_tv);
		elapsed = tvdiff(&fin_tv, &stt_tv);
		ASPRINTF(&warning, "HTTP response to (%.10s...) %.3fs not ok: %s",
			 rpc_method(rpc_req), elapsed, cs->buf);
		timeout = 0;
		/* 如果存在，查找JSON响应 */
		while (read_socket_line(cs, &timeout) > 0) {
			timeout = 0;
			if (*cs->buf != '{')
				continue;
			free(warning);
			/* 用JSON响应替换警告信息 */
			ASPRINTF(&warning, "JSON response to (%.10s...) %.3fs not ok: %s",
				 rpc_method(rpc_req), elapsed, cs->buf);
			break;
		}
		goto out_empty;
	}
	do {
		ret = read_socket_line(cs, &timeout);
		if (ret < 1) {
			tv_time(&fin_tv);
			elapsed = tvdiff(&fin_tv, &stt_tv);
			ASPRINTF(&warning, "Failed to read http socket lines in %s (%.10s...) %.3fs",
				 __func__, rpc_method(rpc_req), elapsed);
			goto out_empty;
		}
	} while (strncmp(cs->buf, "{", 1));
	tv_time(&fin_tv);
	elapsed = tvdiff(&fin_tv, &stt_tv);
	if (elapsed > 5.0) {
		ASPRINTF(&warning, "HTTP socket read+write took %.3fs in %s (%.10s...)",
			 elapsed, __func__, rpc_method(rpc_req));
	}

	val = json_loads(cs->buf, 0, &err_val);
	if (!val) {
		ASPRINTF(&warning, "JSON decode (%.10s...) failed(%d): %s",
			 rpc_method(rpc_req), err_val.line, err_val.text);
	}
out_empty:
	empty_socket(cs->fd);
	empty_buffer(cs);
out:
	if (warning) {
		if (info_only)
			LOGINFO("%s", warning);
		else
			LOGWARNING("%s", warning);
		free(warning);
	}
	Close(cs->fd);
	free(http_req);
	dealloc(cs->buf);
	cksem_post(&cs->sem);
	return val;
}

json_t *json_rpc_call(connsock_t *cs, const char *rpc_req)
{
	return _json_rpc_call(cs, rpc_req, false);
}

json_t *json_rpc_response(connsock_t *cs, const char *rpc_req)
{
	return _json_rpc_call(cs, rpc_req, true);
}

/* 当我们提交不重要的信息且不关心响应时使用。 */
void json_rpc_msg(connsock_t *cs, const char *rpc_req)
{
	json_t *val = _json_rpc_call(cs, rpc_req, true);

	/* We don't care about the result */
	json_decref(val);
}

static void terminate_oldpid(const ckpool_t *ckp, proc_instance_t *pi, const pid_t oldpid)
{
	if (!ckp->killold) {
		quit(1, "进程%s pid %d仍然存在，使用-H选项启动ckpool进行交接，或使用-k选项强制终止",
				pi->processname, oldpid);
	}
	LOGNOTICE("Terminating old process %s pid %d", pi->processname, oldpid);
	if (kill_pid(oldpid, 15))
		quit(1, "无法终止旧进程%s pid %d", pi->processname, oldpid);
	LOGWARNING("Terminating old process %s pid %d", pi->processname, oldpid);
	if (pid_wait(oldpid, 500))
		return;
	LOGWARNING("Old process %s pid %d failed to respond to terminate request, killing",
			pi->processname, oldpid);
	if (kill_pid(oldpid, 9) || !pid_wait(oldpid, 3000))
		quit(1, "无法终止旧进程%s pid %d", pi->processname, oldpid);
}

/* 用于JSON消息的阻塞发送 */
bool _send_json_msg(connsock_t *cs, const json_t *json_msg, const char *file, const char *func, const int line)
{
	bool ret = false;
	int len, sent;
	char *s;

	if (unlikely(!json_msg)) {
		LOGWARNING("来自%s %s:%d的send_json_msg中的空json消息", file, func, line);
		goto out;
	}
	s = json_dumps(json_msg, JSON_ESCAPE_SLASH | JSON_EOL);
	if (unlikely(!s)) {
		LOGWARNING("来自%s %s:%d的send_json_msg中的空json转储", file, func, line);
		goto out;
	}
	LOGDEBUG("Sending json msg: %s", s);
	len = strlen(s);
	if (unlikely(!len)) {
		LOGWARNING("来自%s %s:%d的send_json_msg中的零长度字符串", file, func, line);
		goto out;
	}
	sent = write_socket(cs->fd, s, len);
	dealloc(s);
	if (sent != len) {
		LOGNOTICE("在send_json_msg中发送失败，应发送%d字节，实际发送%d字节", len, sent);
		goto out;
	}
	ret = true;
out:
	return ret;
}

/* 解码应该包含json消息的字符串，并仅返回result键的内容或NULL。 */
static json_t *json_result(json_t *val)
{
	json_t *res_val = NULL, *err_val;

	res_val = json_object_get(val, "result");
	/* (null)是有效的结果，而无值表示错误，因此掩盖掉(null)并只处理缺少结果的情况 */
	if (json_is_null(res_val))
		res_val = NULL;
	else if (!res_val) {
		char *ss;

		err_val = json_object_get(val, "error");
		if (err_val)
			ss = json_dumps(err_val, 0);
		else
			ss = strdup("(unknown reason)");

		LOGNOTICE("JSON-RPC decode of json_result failed: %s", ss);
		free(ss);
	}
	return res_val;
}

/* 如果存在错误值，则返回错误值 */
static json_t *json_errval(json_t *val)
{
	json_t *err_val = json_object_get(val, "error");

	return err_val;
}

/* 解析字符串并返回它包含的json值（如果有），以及res_val中的结果。如果未找到result键，则返回NULL。 */
json_t *json_msg_result(const char *msg, json_t **res_val, json_t **err_val)
{
	json_error_t err;
	json_t *val;

	*res_val = NULL;
	val = json_loads(msg, 0, &err);
	if (!val) {
		LOGWARNING("Json decode failed(%d): %s", err.line, err.text);
		goto out;
	}
	*res_val = json_result(val);
	*err_val = json_errval(val);

out:
	return val;
}

/* 打开路径中的文件，检查其中是否存在仍然存在的pid，如果不存在，则将当前pid写入该文件。 */
static bool write_pid(ckpool_t *ckp, const char *path, proc_instance_t *pi, const pid_t pid, const pid_t oldpid)
{
	FILE *fp;

	if (ckp->handover && oldpid && !pid_wait(oldpid, 500)) {
		LOGWARNING("Old process pid %d failed to shutdown cleanly, terminating", oldpid);
		terminate_oldpid(ckp, pi, oldpid);
	}

	fp = fopen(path, "we");
	if (!fp) {
		LOGERR("无法打开文件%s", path);
		return false;
	}
	fprintf(fp, "%d", pid);
	fclose(fp);

	return true;
}

static void name_process_sockname(unixsock_t *us, const proc_instance_t *pi)
{
	us->path = strdup(pi->ckp->socket_dir);
	realloc_strcat(&us->path, pi->sockname);
}

static void open_process_sock(ckpool_t *ckp, const proc_instance_t *pi, unixsock_t *us)
{
	LOGDEBUG("Opening %s", us->path);
	us->sockd = open_unix_server(us->path);
	if (unlikely(us->sockd < 0))
		quit(1, "无法打开%s套接字", pi->sockname);
	if (chown(us->path, -1, ckp->gr_gid))
		quit(1, "无法将%s设置为组ID %d", us->path, ckp->gr_gid);
}

static void create_process_unixsock(proc_instance_t *pi)
{
	unixsock_t *us = &pi->us;
	ckpool_t *ckp = pi->ckp;

	name_process_sockname(us, pi);
	open_process_sock(ckp, pi, us);
}

static void write_namepid(proc_instance_t *pi)
{
	char s[256];

	pi->pid = getpid();
	sprintf(s, "%s%s.pid", pi->ckp->socket_dir, pi->processname);
	if (!write_pid(pi->ckp, s, pi, pi->pid, pi->oldpid))
		quit(1, "无法写入%s pid %d", pi->processname, pi->pid);
}

static void rm_namepid(const proc_instance_t *pi)
{
	char s[256];

	sprintf(s, "%s%s.pid", pi->ckp->socket_dir, pi->processname);
	unlink(s);
}

static void launch_logger(ckpool_t *ckp)
{
	ckp->logger = create_ckmsgq(ckp, "logger", &proclog);
	ckp->console_logger = create_ckmsgq(ckp, "conlog", &console_log);
}

static void clean_up(ckpool_t *ckp)
{
	rm_namepid(&ckp->main);
	dealloc(ckp->socket_dir);
}

static void cancel_pthread(pthread_t *pth)
{
	if (!pth || !*pth)
		return;
	pthread_cancel(*pth);
	pth = NULL;
}

static void sighandler(const int sig)
{
	ckpool_t *ckp = global_ckp;

	signal(sig, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	LOGWARNING("进程%s收到信号%d，正在关闭",
		   ckp->name, sig);

	cancel_pthread(&ckp->pth_listener);
	exit(0);
}

static bool _json_get_string(char **store, const json_t *entry, const char *res)
{
	bool ret = false;
	const char *buf;

	*store = NULL;
	if (!entry || json_is_null(entry)) {
		LOGDEBUG("Json未找到条目%s", res);
		goto out;
	}
	if (!json_is_string(entry)) {
		LOGWARNING("Json条目%s不是字符串", res);
		goto out;
	}
	buf = json_string_value(entry);
	LOGDEBUG("Json找到条目%s: %s", res, buf);
	*store = strdup(buf);
	ret = true;
out:
	return ret;
}

bool json_get_string(char **store, const json_t *val, const char *res)
{
	return _json_get_string(store, json_object_get(val, res), res);
}

/* 当必须有有效字符串时使用 */
static void json_get_configstring(char **store, const json_t *val, const char *res)
{
	bool ret = _json_get_string(store, json_object_get(val, res), res);

	if (!ret) {
		LOGEMERG("%s的配置字符串无效或缺少对象", res);
		exit(1);
	}
}

bool json_get_int64(int64_t *store, const json_t *val, const char *res)
{
	json_t *entry = json_object_get(val, res);
	bool ret = false;

	if (!entry) {
		LOGDEBUG("Json未找到条目%s", res);
		goto out;
	}
	if (!json_is_integer(entry)) {
		LOGINFO("Json条目%s不是整数", res);
		goto out;
	}
	*store = json_integer_value(entry);
	LOGDEBUG("Json找到条目%s: %"PRId64, res, *store);
	ret = true;
out:
	return ret;
}

bool json_get_int(int *store, const json_t *val, const char *res)
{
	json_t *entry = json_object_get(val, res);
	bool ret = false;

	if (!entry) {
		LOGDEBUG("Json未找到条目%s", res);
		goto out;
	}
	if (!json_is_integer(entry)) {
		LOGWARNING("Json条目%s不是整数", res);
		goto out;
	}
	*store = json_integer_value(entry);
	LOGDEBUG("Json找到条目%s: %d", res);
	ret = true;
out:
	return ret;
}

bool json_get_double(double *store, const json_t *val, const char *res)
{
	json_t *entry = json_object_get(val, res);
	bool ret = false;

	if (!entry) {
		LOGDEBUG("Json未找到条目%s", res);
		goto out;
	}
	if (!json_is_real(entry)) {
		LOGWARNING("Json条目%s不是双精度浮点数", res);
		goto out;
	}
	*store = json_real_value(entry);
	LOGDEBUG("Json找到条目%s: %f", res);
	ret = true;
out:
	return ret;
}

bool json_get_uint32(uint32_t *store, const json_t *val, const char *res)
{
	json_t *entry = json_object_get(val, res);
	bool ret = false;

	if (!entry) {
		LOGDEBUG("Json未找到条目%s", res);
		goto out;
	}
	if (!json_is_integer(entry)) {
		LOGWARNING("Json条目%s不是整数", res);
		goto out;
	}
	*store = json_integer_value(entry);
	LOGDEBUG("Json找到条目%s: %u", res);
	ret = true;
out:
	return ret;
}

bool json_get_bool(bool *store, const json_t *val, const char *res)
{
	json_t *entry = json_object_get(val, res);
	bool ret = false;

	if (!entry) {
		LOGDEBUG("Json未找到条目%s", res);
		goto out;
	}
	if (!json_is_boolean(entry)) {
		LOGINFO("Json条目%s不是布尔值", res);
		goto out;
	}
	*store = json_is_true(entry);
	LOGDEBUG("Json找到条目%s: %s", res, *store ? "true" : "false");
	ret = true;
out:
	return ret;
}

bool json_getdel_int(int *store, json_t *val, const char *res)
{
	bool ret;

	ret = json_get_int(store, val, res);
	if (ret)
		json_object_del(val, res);
	return ret;
}

bool json_getdel_int64(int64_t *store, json_t *val, const char *res)
{
	bool ret;

	ret = json_get_int64(store, val, res);
	if (ret)
		json_object_del(val, res);
	return ret;
}

static void parse_btcds(ckpool_t *ckp, const json_t *arr_val, const int arr_size)
{
	json_t *val;
	int i;

	ckp->btcds = arr_size;
	ckp->btcdurl = ckzalloc(sizeof(char *) * arr_size);
	ckp->btcdauth = ckzalloc(sizeof(char *) * arr_size);
	ckp->btcdpass = ckzalloc(sizeof(char *) * arr_size);
	ckp->btcdnotify = ckzalloc(sizeof(bool *) * arr_size);
	for (i = 0; i < arr_size; i++) {
		val = json_array_get(arr_val, i);
		json_get_configstring(&ckp->btcdurl[i], val, "url");
		json_get_configstring(&ckp->btcdauth[i], val, "auth");
		json_get_configstring(&ckp->btcdpass[i], val, "pass");
		json_get_bool(&ckp->btcdnotify[i], val, "notify");
	}
}

static void parse_proxies(ckpool_t *ckp, const json_t *arr_val, const int arr_size)
{
	json_t *val;
	int i;

	ckp->proxies = arr_size;
	ckp->proxyurl = ckzalloc(sizeof(char *) * arr_size);
	ckp->proxyauth = ckzalloc(sizeof(char *) * arr_size);
	ckp->proxypass = ckzalloc(sizeof(char *) * arr_size);
	for (i = 0; i < arr_size; i++) {
		val = json_array_get(arr_val, i);
		json_get_configstring(&ckp->proxyurl[i], val, "url");
		json_get_configstring(&ckp->proxyauth[i], val, "auth");
		if (!json_get_string(&ckp->proxypass[i], val, "pass"))
			ckp->proxypass[i] = strdup("");
	}
}

static bool parse_serverurls(ckpool_t *ckp, const json_t *arr_val)
{
	bool ret = false;
	int arr_size, i;

	if (!arr_val)
		goto out;
	if (!json_is_array(arr_val)) {
		LOGINFO("无法将serverurl条目解析为数组");
		goto out;
	}
	arr_size = json_array_size(arr_val);
	if (!arr_size) {
		LOGWARNING("Serverurl数组为空");
		goto out;
	}
	ckp->serverurls = arr_size;
	ckp->serverurl = ckalloc(sizeof(char *) * arr_size);
	ckp->server_highdiff = ckzalloc(sizeof(bool) * arr_size);
	ckp->nodeserver = ckzalloc(sizeof(bool) * arr_size);
	ckp->trusted = ckzalloc(sizeof(bool) * arr_size);
	for (i = 0; i < arr_size; i++) {
		json_t *val = json_array_get(arr_val, i);

		if (!_json_get_string(&ckp->serverurl[i], val, "serverurl"))
			LOGWARNING("第%d个serverurl条目无效", i);
	}
	ret = true;
out:
	return ret;
}

static void parse_nodeservers(ckpool_t *ckp, const json_t *arr_val)
{
	int arr_size, i, j, total_urls;

	if (!arr_val)
		return;
	if (!json_is_array(arr_val)) {
		LOGWARNING("无法将nodeservers条目解析为数组");
		return;
	}
	arr_size = json_array_size(arr_val);
	if (!arr_size) {
		LOGWARNING("Nodeserver数组为空");
		return;
	}
	total_urls = ckp->serverurls + arr_size;
	ckp->serverurl = realloc(ckp->serverurl, sizeof(char *) * total_urls);
	ckp->nodeserver = realloc(ckp->nodeserver, sizeof(bool) * total_urls);
	ckp->trusted = realloc(ckp->trusted, sizeof(bool) * total_urls);
	for (i = 0, j = ckp->serverurls; j < total_urls; i++, j++) {
		json_t *val = json_array_get(arr_val, i);

		if (!_json_get_string(&ckp->serverurl[j], val, "nodeserver"))
			LOGWARNING("第%d个nodeserver条目无效", i);
		ckp->nodeserver[j] = true;
		ckp->nodeservers++;
	}
	ckp->serverurls = total_urls;
}

static void parse_trusted(ckpool_t *ckp, const json_t *arr_val)
{
	int arr_size, i, j, total_urls;

	if (!arr_val)
		return;
	if (!json_is_array(arr_val)) {
		LOGWARNING("无法将trusted server条目解析为数组");
		return;
	}
	arr_size = json_array_size(arr_val);
	if (!arr_size) {
		LOGWARNING("Trusted数组为空");
		return;
	}
	total_urls = ckp->serverurls + arr_size;
	ckp->serverurl = realloc(ckp->serverurl, sizeof(char *) * total_urls);
	ckp->nodeserver = realloc(ckp->nodeserver, sizeof(bool) * total_urls);
	ckp->trusted = realloc(ckp->trusted, sizeof(bool) * total_urls);
	for (i = 0, j = ckp->serverurls; j < total_urls; i++, j++) {
		json_t *val = json_array_get(arr_val, i);

		if (!_json_get_string(&ckp->serverurl[j], val, "trusted"))
			LOGWARNING("第%d个trusted server条目无效", i);
		ckp->trusted[j] = true;
	}
	ckp->serverurls = total_urls;
}


static bool parse_redirecturls(ckpool_t *ckp, const json_t *arr_val)
{
	bool ret = false;
	int arr_size, i;
	char *redirecturl, url[INET6_ADDRSTRLEN], port[8];
	redirecturl = alloca(INET6_ADDRSTRLEN);

	if (!arr_val)
		goto out;
	if (!json_is_array(arr_val)) {
		LOGNOTICE("无法将redirecturl条目解析为数组");
		goto out;
	}
	arr_size = json_array_size(arr_val);
	if (!arr_size) {
		LOGWARNING("redirecturl数组为空");
		goto out;
	}
	ckp->redirecturls = arr_size;
	ckp->redirecturl = ckalloc(sizeof(char *) * arr_size);
	ckp->redirectport = ckalloc(sizeof(char *) * arr_size);
	for (i = 0; i < arr_size; i++) {
		json_t *val = json_array_get(arr_val, i);

		strncpy(redirecturl, json_string_value(val), INET6_ADDRSTRLEN - 1);
		/* 确保URL能够正确解析 */
		if (!url_from_serverurl(redirecturl, url, port))
			quit(1, "第%d个redirecturl条目无效: %s", i, redirecturl);
		ckp->redirecturl[i] = strdup(strsep(&redirecturl, ":"));
		ckp->redirectport[i] = strdup(port);
	}
	ret = true;
out:
	return ret;
}


static void parse_config(ckpool_t *ckp)
{
	json_t *json_conf, *arr_val;
	json_error_t err_val;
	char *url, *vmask;
	int arr_size;

	json_conf = json_load_file(ckp->config, JSON_DISABLE_EOF_CHECK, &err_val);
	if (!json_conf) {
		LOGWARNING("配置文件%s的Json解析错误: (%d): %s", ckp->config,
		   err_val.line, err_val.text);
		return;
	}
	arr_val = json_object_get(json_conf, "btcd");
	if (arr_val && json_is_array(arr_val)) {
		arr_size = json_array_size(arr_val);
		if (arr_size)
			parse_btcds(ckp, arr_val, arr_size);
	}
	json_get_string(&ckp->btcaddress, json_conf, "btcaddress");
	json_get_string(&ckp->btcsig, json_conf, "btcsig");
	if (ckp->btcsig && strlen(ckp->btcsig) > 38) {
		LOGWARNING("签名%s太长，截断到38字节", ckp->btcsig);
		ckp->btcsig[38] = '\0';
	}
	json_get_int(&ckp->blockpoll, json_conf, "blockpoll");
	json_get_int(&ckp->nonce1length, json_conf, "nonce1length");
	json_get_int(&ckp->nonce2length, json_conf, "nonce2length");
	json_get_int(&ckp->update_interval, json_conf, "update_interval");
	json_get_string(&vmask, json_conf, "version_mask");
	if (vmask && strlen(vmask) && validhex(vmask))
		sscanf(vmask, "%x", &ckp->version_mask);
	else
		ckp->version_mask = ABCMINT_VERSION_MASK; // 默认使用abcmint版本掩码，允许矿工修改低8位
	/* 首先查找数组，然后查找单个条目 */
	arr_val = json_object_get(json_conf, "serverurl");
	if (!parse_serverurls(ckp, arr_val)) {
		if (json_get_string(&url, json_conf, "serverurl")) {
			ckp->serverurl = ckalloc(sizeof(char *));
			ckp->serverurl[0] = url;
			ckp->serverurls = 1;
		}
	}
	arr_val = json_object_get(json_conf, "nodeserver");
	parse_nodeservers(ckp, arr_val);
	arr_val = json_object_get(json_conf, "trusted");
	parse_trusted(ckp, arr_val);
	json_get_string(&ckp->upstream, json_conf, "upstream");
	json_get_int64(&ckp->mindiff, json_conf, "mindiff");
	json_get_int64(&ckp->startdiff, json_conf, "startdiff");
	json_get_int64(&ckp->highdiff, json_conf, "highdiff");
	json_get_int64(&ckp->maxdiff, json_conf, "maxdiff");
	json_get_string(&ckp->logdir, json_conf, "logdir");
	json_get_int(&ckp->maxclients, json_conf, "maxclients");
	json_get_double(&ckp->donation, json_conf, "donation");
	/* 避免微小金额的捐赠 */
	if (ckp->donation < 0.1)
		ckp->donation = 0;
	else if (ckp->donation > 99.9)
		ckp->donation = 99.9;
	arr_val = json_object_get(json_conf, "proxy");
	if (arr_val && json_is_array(arr_val)) {
		arr_size = json_array_size(arr_val);
		if (arr_size)
			parse_proxies(ckp, arr_val, arr_size);
	}
	arr_val = json_object_get(json_conf, "redirecturl");
	if (arr_val)
		parse_redirecturls(ckp, arr_val);
	json_get_string(&ckp->zmqblock, json_conf, "zmqblock");

	json_decref(json_conf);
}

static void manage_old_instance(ckpool_t *ckp, proc_instance_t *pi)
{
	struct stat statbuf;
	char path[256];
	FILE *fp;

	sprintf(path, "%s%s.pid", pi->ckp->socket_dir, pi->processname);
	if (!stat(path, &statbuf)) {
		int oldpid, ret;

		LOGNOTICE("文件%s存在", path);
		fp = fopen(path, "re");
		if (!fp)
			quit(1, "无法打开文件%s", path);
		ret = fscanf(fp, "%d", &oldpid);
		fclose(fp);
		if (ret == 1 && !(kill_pid(oldpid, 0))) {
			LOGNOTICE("旧进程%s pid %d仍然存在", pi->processname, oldpid);
			if (ckp->handover) {
				LOGINFO("保存pid以便在交接时处理");
				pi->oldpid = oldpid;
				return;
			}
			terminate_oldpid(ckp, pi, oldpid);
		}
	}
}

static void prepare_child(ckpool_t *ckp, proc_instance_t *pi, void *process, char *name)
{
	pi->ckp = ckp;
	pi->processname = name;
	pi->sockname = pi->processname;
	create_process_unixsock(pi);
	create_pthread(&pi->pth_process, process, pi);
	create_unix_receiver(pi);
}

static struct option long_options[] = {
	{"config",	required_argument,	0,	'c'},
	{"daemonise",	no_argument,		0,	'D'},
	{"group",	required_argument,	0,	'g'},
	{"handover",	no_argument,		0,	'H'},
	{"help",	no_argument,		0,	'h'},
	{"killold",	no_argument,		0,	'k'},
	{"log-shares",	no_argument,		0,	'L'},
	{"loglevel",	required_argument,	0,	'l'},
	{"name",	required_argument,	0,	'n'},
	{"node",	no_argument,		0,	'N'},
	{"passthrough",	no_argument,		0,	'P'},
	{"proxy",	no_argument,		0,	'p'},
	{"quiet",	no_argument,		0,	'q'},
	{"redirector",	no_argument,		0,	'R'},
	{"sockdir",	required_argument,	0,	's'},
	{"trusted",	no_argument,		0,	't'},
	{"userproxy",	no_argument,		0,	'u'},
	{0, 0, 0, 0}
};

static bool send_recv_path(const char *path, const char *msg)
{
	int sockd = open_unix_client(path);
	bool ret = false;
	char *response;

	send_unix_msg(sockd, msg);
	response = recv_unix_msg(sockd);
	if (response) {
		ret = true;
		LOGWARNING("收到: %s 作为对 %s 请求的响应", response, msg);
		dealloc(response);
	} else
		LOGWARNING("未收到对%s请求的响应", msg);
	Close(sockd);
	return ret;
}

int main(int argc, char **argv)
{
	struct sigaction handler;
	int c, ret, i = 0, j;
	char buf[512] = {};
	char *appname;
	ckpool_t ckp;

	/* 使严重的浮点错误成为致命错误，以避免遗漏细微的bug */
	feenableexcept(FE_DIVBYZERO | FE_INVALID);
	json_set_alloc_funcs(json_ckalloc, free);

	global_ckp = &ckp;
	memset(&ckp, 0, sizeof(ckp));
	ckp.starttime = time(NULL);
	ckp.startpid = getpid();
	ckp.loglevel = LOG_NOTICE;
	ckp.initial_args = ckalloc(sizeof(char *) * (argc + 2)); /* Leave room for extra -H */
	for (ckp.args = 0; ckp.args < argc; ckp.args++)
		ckp.initial_args[ckp.args] = strdup(argv[ckp.args]);
	ckp.initial_args[ckp.args] = NULL;

	appname = basename(argv[0]);
	if (!strcmp(appname, "ckproxy"))
		ckp.proxy = true;

	while ((c = getopt_long(argc, argv, "c:Dd:g:HhkLl:Nn:PpqRS:s:tu", long_options, &i)) != -1) {
		switch (c) {
			case 'c':
				ckp.config = optarg;
				break;
			case 'D':
				ckp.daemon = true;
				break;
			case 'g':
				ckp.grpnam = optarg;
				break;
			case 'H':
				ckp.handover = true;
				ckp.killold = true;
				break;
			case 'h':
				for (j = 0; long_options[j].val; j++) {
					struct option *jopt = &long_options[j];

					if (jopt->has_arg) {
						char *upper = alloca(strlen(jopt->name) + 1);
						int offset = 0;

						do {
							upper[offset] = toupper(jopt->name[offset]);
						} while (upper[offset++] != '\0');
						printf("-%c %s | --%s %s\n", jopt->val,
						       upper, jopt->name, upper);
					} else
						printf("-%c | --%s\n", jopt->val, jopt->name);
				}
				exit(0);
			case 'k':
				ckp.killold = true;
				break;
			case 'L':
				ckp.logshares = true;
				break;
			case 'l':
				ckp.loglevel = atoi(optarg);
				if (ckp.loglevel < LOG_EMERG || ckp.loglevel > LOG_DEBUG) {
					quit(1, "无效的日志级别（范围 %d - %d）: %d",
			     LOG_EMERG, LOG_DEBUG, ckp.loglevel);
				}
				break;
			case 'N':
				if (ckp.proxy || ckp.redirector || ckp.userproxy || ckp.passthrough)
					quit(1, "无法同时设置其他代理类型或重定向器和节点模式");
				ckp.proxy = ckp.passthrough = ckp.node = true;
				break;
			case 'n':
				ckp.name = optarg;
				break;
			case 'P':
				if (ckp.proxy || ckp.redirector || ckp.userproxy || ckp.node)
					quit(1, "无法同时设置其他代理类型或重定向器和直通模式");
				ckp.proxy = ckp.passthrough = true;
				break;
			case 'p':
				if (ckp.passthrough || ckp.redirector || ckp.userproxy || ckp.node)
					quit(1, "无法同时设置其他代理类型或重定向器和代理模式");
				ckp.proxy = true;
				break;
			case 'q':
				ckp.quiet = true;
				break;
			case 'R':
				if (ckp.proxy || ckp.passthrough || ckp.userproxy || ckp.node)
					quit(1, "无法同时设置代理类型或直通模式和重定向器模式");
				ckp.proxy = ckp.passthrough = ckp.redirector = true;
				break;
			case 's':
				ckp.socket_dir = strdup(optarg);
				break;
			case 't':
				if (ckp.proxy)
					quit(1, "无法同时设置代理类型和可信远程模式");
				ckp.remote = true;
				break;
			case 'u':
				if (ckp.proxy || ckp.redirector || ckp.passthrough || ckp.node)
					quit(1, "无法同时设置userproxy和其他代理类型或重定向器");
				ckp.userproxy = ckp.proxy = true;
				break;
		}
	}

	if (!ckp.name) {
		if (ckp.node)
			ckp.name = "cknode";
		else if (ckp.redirector)
			ckp.name = "ckredirector";
		else if (ckp.passthrough)
			ckp.name = "ckpassthrough";
		else if (ckp.proxy)
			ckp.name = "ckproxy";
		else
			ckp.name = "ckpool";
	}
	snprintf(buf, 15, "%s", ckp.name);
	prctl(PR_SET_NAME, buf, 0, 0, 0);
	memset(buf, 0, 15);

	if (ckp.grpnam) {
		struct group *group = getgrnam(ckp.grpnam);

		if (!group)
			quit(1, "无法找到组%s", ckp.grpnam);
		ckp.gr_gid = group->gr_gid;
	} else
		ckp.gr_gid = getegid();

	if (!ckp.config) {
		ckp.config = strdup(ckp.name);
		realloc_strcat(&ckp.config, ".conf");
	}
	if (!ckp.socket_dir) {
		ckp.socket_dir = strdup("/tmp/");
		realloc_strcat(&ckp.socket_dir, ckp.name);
	}
	trail_slash(&ckp.socket_dir);

	/* 忽略SIGPIPE信号 */
	signal(SIGPIPE, SIG_IGN);

	ret = mkdir(ckp.socket_dir, 0750);
	if (ret && errno != EEXIST)
		quit(1, "无法创建目录%s", ckp.socket_dir);

	parse_config(&ckp);
	/* 如果在配置文件中未找到，则设置默认值 */
	if (!ckp.btcds) {
		ckp.btcds = 1;
		ckp.btcdurl = ckzalloc(sizeof(char *));
		ckp.btcdauth = ckzalloc(sizeof(char *));
		ckp.btcdpass = ckzalloc(sizeof(char *));
		ckp.btcdnotify = ckzalloc(sizeof(bool));
	}
	for (i = 0; i < ckp.btcds; i++) {
		if (!ckp.btcdurl[i])
			ckp.btcdurl[i] = strdup("localhost:8882"); // 使用abcmint默认RPC端口
		if (!ckp.btcdauth[i])
			ckp.btcdauth[i] = strdup("user"); // abcmintd RPC用户名
		if (!ckp.btcdpass[i])
			ckp.btcdpass[i] = strdup("pass"); // abcmintd RPC密码
	}

	/* abcmint捐赠地址 - 使用以'8'开头的标准地址格式 */
	ckp.donaddress = "8XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";

	/* 在测试网上的捐赠没有意义，但对于完整测试是必要的。测试网和regtest地址 */
	ckp.tndonaddress = "8XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";
	ckp.rtdonaddress = "8XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";

	if (!ckp.btcaddress && !ckp.proxy)
		quit(0, "非单矿工挖矿必须在配置中设置abcmint地址，正在中止！");
	if (!ckp.blockpoll)
		ckp.blockpoll = 100; // 轮询abcmint守护进程的频率(毫秒)
	if (!ckp.nonce1length)
		ckp.nonce1length = 4; // abcmint nonce1长度
	else if (ckp.nonce1length < 2 || ckp.nonce1length > 8)
		quit(0, "指定的nonce1length %d无效，必须为2~8", ckp.nonce1length);
	if (!ckp.nonce2length) {
		/* 在代理模式下，nonce2length默认为零 */
		if (!ckp.proxy)
			ckp.nonce2length = 8; // abcmint nonce2长度
	} else if (ckp.nonce2length < 2 || ckp.nonce2length > 8)
		quit(0, "指定的nonce2length %d无效，必须为2~8", ckp.nonce2length);
	if (!ckp.update_interval)
		ckp.update_interval = 30; // abcmint难度更新间隔(秒)
	if (!ckp.mindiff)
		ckp.mindiff = 1; // abcmint最低难度
	if (!ckp.startdiff)
		ckp.startdiff = 100; // abcmint起始难度，适合rainbow18算法
	if (!ckp.highdiff)
		ckp.highdiff = 1000000; // abcmint高难度阈值
	if (!ckp.logdir)
		ckp.logdir = strdup("logs");
	if (!ckp.serverurls)
		ckp.serverurl = ckzalloc(sizeof(char *));
	if (ckp.proxy && !ckp.proxies)
		quit(0, "在配置文件%s中未找到代理条目", ckp.config);
	if (ckp.redirector && !ckp.redirecturls)
		quit(0, "在配置文件%s中未找到重定向条目", ckp.config);
	if (!ckp.zmqblock)
		ckp.zmqblock = "tcp://127.0.0.1:28882"; // 使用abcmint默认ZMQ端口(假设为28882)

	/* 创建日志目录 */
	trail_slash(&ckp.logdir);
	ret = mkdir(ckp.logdir, 0750);
	if (ret && errno != EEXIST)
		quit(1, "无法创建日志目录%s", ckp.logdir);

	/* 创建用户日志目录 */
	sprintf(buf, "%s/users", ckp.logdir);
	ret = mkdir(buf, 0750);
	if (ret && errno != EEXIST)
		quit(1, "无法创建用户日志目录%s", buf);

	/* 创建矿池日志目录 */
	sprintf(buf, "%s/pool", ckp.logdir);
	ret = mkdir(buf, 0750);
	if (ret && errno != EEXIST)
		quit(1, "无法创建矿池日志目录%s", buf);

	/* 创建日志文件 */
	ASPRINTF(&ckp.logfilename, "%s%s.log", ckp.logdir, ckp.name);
	if (!open_logfile(&ckp))
		quit(1, "无法创建或打开日志文件%s", buf);
	launch_logger(&ckp);

	ckp.main.ckp = &ckp;
	ckp.main.processname = strdup("main");
	ckp.main.sockname = strdup("listener");
	name_process_sockname(&ckp.main.us, &ckp.main);
	ckp.oldconnfd = ckzalloc(sizeof(int *) * ckp.serverurls);
	manage_old_instance(&ckp, &ckp.main);
	if (ckp.handover) {
		const char *path = ckp.main.us.path;

		if (send_recv_path(path, "ping")) {
			for (i = 0; i < ckp.serverurls; i++) {
				char oldurl[INET6_ADDRSTRLEN], oldport[8];
				char getfd[16];
				int sockd;

				snprintf(getfd, 15, "getxfd%d", i);
				sockd = open_unix_client(path);
				if (sockd < 1)
					break;
				if (!send_unix_msg(sockd, getfd))
					break;
				ckp.oldconnfd[i] = get_fd(sockd);
				Close(sockd);
				sockd = ckp.oldconnfd[i];
				if (!sockd)
					break;
				if (url_from_socket(sockd, oldurl, oldport)) {
					LOGWARNING("已继承旧服务器套接字%d url %s:%s !",
			   i, oldurl, oldport);
				} else {
					LOGWARNING("已继承旧服务器套接字%d，新文件描述符为%d!",
			   i, ckp.oldconnfd[i]);
				}
			}
			send_recv_path(path, "reject");
			send_recv_path(path, "reconnect");
			send_recv_path(path, "shutdown");
		}
	}

	if (ckp.daemon) {
		int fd;

		if (fork())
			exit(0);
		setsid();
		fd = open("/dev/null",O_RDWR, 0);
		if (fd != -1) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}
	}

	write_namepid(&ckp.main);
	open_process_sock(&ckp, &ckp.main, &ckp.main.us);

	ret = sysconf(_SC_OPEN_MAX);
	if (ckp.maxclients > ret * 9 / 10) {
		LOGWARNING("由于最大文件打开限制%d，无法将maxclients设置为%d，正在减少到%d",
		   ckp.maxclients, ret, ret * 9 / 10);
		ckp.maxclients = ret * 9 / 10;
	} else if (!ckp.maxclients) {
		LOGNOTICE("由于最大文件打开限制%d，正在将maxclients设置为%d",
		  ret * 9 / 10, ret);
		ckp.maxclients = ret * 9 / 10;
	}

	// ckp.ckpapi = create_ckmsgq(&ckp, "api", &ckpool_api);
	create_pthread(&ckp.pth_listener, listener, &ckp.main);

	handler.sa_handler = &sighandler;
	handler.sa_flags = 0;
	sigemptyset(&handler.sa_mask);
	sigaction(SIGTERM, &handler, NULL);
	sigaction(SIGINT, &handler, NULL);

	/* 从这里启动单独的进程 */
	prepare_child(&ckp, &ckp.generator, generator, "generator");
	prepare_child(&ckp, &ckp.stratifier, stratifier, "stratifier");
	prepare_child(&ckp, &ckp.connector, connector, "connector");

	/* 如果监听器收到关闭消息，则从这里开始关闭 */
	if (ckp.pth_listener)
		join_pthread(ckp.pth_listener);

	clean_up(&ckp);

	return 0;
}
