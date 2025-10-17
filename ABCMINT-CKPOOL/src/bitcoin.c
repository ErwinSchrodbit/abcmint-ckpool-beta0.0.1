/*
 * 版权所有 2014-2018,2023 Con Kolivas
 * 适配abcmint挖矿算法和地址格式
 *
 * 本程序是自由软件；您可以按照自由软件基金会发布的GNU通用公共许可证（第3版或更高版本）
 * 的条款重新分发和/或修改它。详情请参阅COPYING文件。
 */

#include "config.h"

#include <string.h>

#include "ckpool.h" // 包含ABCMINT_BLOCK_VERSION定义
#include "bitcoin.h"
#include "libckpool.h"
#include "stratifier.h"

/* abcmint适配：不使用SHA-256算法，使用rainbow18算法 */

/* abcmint不使用segwit规则 */

/* abcmint不使用segwit，所以不需要规则检查 */
static bool check_required_rule(const char* rule)
{
	return false;
}

/* 接收abcmint地址并进行基本检查，然后发送到abcmintd验证其是否有效 */
bool validate_address(connsock_t *cs, const char *address, bool *script, bool *segwit)
{
	json_t *val, *res_val, *valid_val, *tmp_val;
	char rpc_req[128];
	bool ret = false;

	if (unlikely(!address)) {
		LOGWARNING("空地址传递给validate_address");
		return ret;
	}

	snprintf(rpc_req, 128, "{\"method\": \"validateaddress\", \"params\": [\"%s\"]}\n", address);
	val = json_rpc_response(cs, rpc_req);
	if (!val) {
		/* 无效地址可能导致解析错误 */
		LOGNOTICE("%s:%s 无法获取validate_address %s的有效json响应",
	  cs->url, cs->port, address);
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGERR("无法从validate_address的json响应中获取result");
		goto out;
	}
	valid_val = json_object_get(res_val, "isvalid");
	if (!valid_val) {
		LOGERR("无法从validate_address的json响应中获取isvalid");
		goto out;
	}
	if (!json_is_true(valid_val)) {
		LOGDEBUG("abcmint地址 %s 无效", address);
		goto out;
	}
	ret = true;
	tmp_val = json_object_get(res_val, "isscript");
	if (unlikely(!tmp_val)) {
		/* 所有支持钱包的新版abcmintd都应该支持这个字段，如果不支持，则通过简单方式
		 * 判断是否为脚本地址。 */
		LOGDEBUG("abcmintd不支持isscript字段");
		/* abcmint脚本地址检查：暂时使用默认规则 */
		*script = false;
		/* abcmint地址检查：标准地址以'8'开头，测试网地址可能不同 */
		if (address[0] != '8')
			ret = false;
		goto out;
	}
	*script = json_is_true(tmp_val);
	/* abcmint不使用segwit，所以我们忽略iswitness字段 */
	*segwit = false;
	LOGDEBUG("abcmint地址 %s 有效%s", address, *script ? " 脚本" : "");
out:
	if (val)
		json_decref(val);
	return ret;
}

json_t *validate_txn(connsock_t *cs, const char *txn)
{
	json_t *val = NULL;
	char *rpc_req;
	int len;

	if (unlikely(!txn || !strlen(txn))) {
		LOGWARNING("空交易传递给validate_txn");
		goto out;
	}
	len = strlen(txn) + 64;
	rpc_req = ckalloc(len);
	sprintf(rpc_req, "{\"method\": \"decoderawtransaction\", \"params\": [\"%s\"]}", txn);
	val = json_rpc_call(cs, rpc_req);
	dealloc(rpc_req);
	if (!val)
		LOGDEBUG("%s:%s 无法获取decoderawtransaction的有效json响应", cs->url, cs->port);
out:
	return val;
}

static const char *gbt_req = "{\"method\": \"getblocktemplate\", \"params\": [{\"capabilities\": [\"coinbasetxn\", \"workid\", \"coinbase/append\"]}]}";

/* 从已连接的abcmintd请求getblocktemplate，然后将信息总结为组装挖矿模板所需的最有效数据集合，
 * 并将其存储在gbtbase_t结构中 */
bool gen_gbtbase(connsock_t *cs, gbtbase_t *gbt)
{
	json_t *rules_array, *coinbase_aux, *res_val, *val;
	const char *previousblockhash;
	char hash_swap[32], tmp[32];
	uint64_t coinbasevalue;
	const char *target;
	const char *flags;
	const char *bits;
	const char *rule;
	int version;
	int curtime;
	int height;
	int i;
	bool ret = false;

	val = json_rpc_call(cs, gbt_req);
	if (!val) {
		LOGWARNING("%s:%s 无法获取getblocktemplate的有效json响应", cs->url, cs->port);
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGWARNING("无法从getblocktemplate的json响应中获取result");
		goto out;
	}

	rules_array = json_object_get(res_val, "rules");
	if (rules_array) {
		int rule_count =  json_array_size(rules_array);

		for (i = 0; i < rule_count; i++) {
			rule = json_string_value(json_array_get(rules_array, i));
			if (rule && *rule++ == '!' && !check_required_rule(rule)) {
				LOGERR("不理解必需的规则: %s", rule);
				goto out;
			}
		}
	}

	previousblockhash = json_string_value(json_object_get(res_val, "previousblockhash"));
	target = json_string_value(json_object_get(res_val, "target"));
	// 为abcmint的rainbow18算法适配，强制使用ABCMINT_BLOCK_VERSION
	version = ABCMINT_BLOCK_VERSION;
	// 保留原来的获取方式，但覆盖它
	// version = json_integer_value(json_object_get(res_val, "version"));
	curtime = json_integer_value(json_object_get(res_val, "curtime"));
	bits = json_string_value(json_object_get(res_val, "bits"));
	height = json_integer_value(json_object_get(res_val, "height"));
	coinbasevalue = json_integer_value(json_object_get(res_val, "coinbasevalue"));
	coinbase_aux = json_object_get(res_val, "coinbaseaux");
	flags = json_string_value(json_object_get(coinbase_aux, "flags"));
	if (!flags)
		flags = "";

	if (unlikely(!previousblockhash || !target || !version || !curtime || !bits || !coinbase_aux)) {
		LOGERR("JSON解码GBT失败 %s %s %d %d %s %s", previousblockhash, target, version, curtime, bits, flags);
		goto out;
	}

	/* 按原样存储getblocktemplate的剩余json组件 */
	json_incref(res_val);
	json_object_del(val, "result");
	gbt->json = res_val;

	hex2bin(hash_swap, previousblockhash, 32);
	swap_256(tmp, hash_swap);
	__bin2hex(gbt->prevhash, tmp, 32);

	strncpy(gbt->target, target, 65);

	hex2bin(hash_swap, target, 32);
	bswap_256(tmp, hash_swap);
	gbt->diff = diff_from_target((uchar *)tmp);
	json_object_set_new_nocheck(gbt->json, "diff", json_real(gbt->diff));

	gbt->version = version;

	gbt->curtime = curtime;

	snprintf(gbt->ntime, 9, "%08x", curtime);
	json_object_set_new_nocheck(gbt->json, "ntime", json_string_nocheck(gbt->ntime));
	sscanf(gbt->ntime, "%x", &gbt->ntime32);

	snprintf(gbt->bbversion, 9, "%08x", version);
	json_object_set_new_nocheck(gbt->json, "bbversion", json_string_nocheck(gbt->bbversion));

	snprintf(gbt->nbit, 9, "%s", bits);
	json_object_set_new_nocheck(gbt->json, "nbit", json_string_nocheck(gbt->nbit));

	gbt->coinbasevalue = coinbasevalue;

	gbt->height = height;

	gbt->flags = strdup(flags);

	ret = true;
out:
	json_decref(val);
	return ret;
}

void clear_gbtbase(gbtbase_t *gbt)
{
	free(gbt->flags);
	if (gbt->json)
		json_decref(gbt->json);
	memset(gbt, 0, sizeof(gbtbase_t));
}

static const char *blockcount_req = "{\"method\": \"getblockcount\", \"params\": []}\n";

/* 从abcmintd请求getblockcount，成功返回区块数量，失败返回-1 */
int get_blockcount(connsock_t *cs)
{
	json_t *val, *res_val;
	int ret = -1;

	val = json_rpc_call(cs, blockcount_req);
	if (!val) {
		LOGWARNING("%s:%s 无法获取getblockcount的有效json响应", cs->url, cs->port);
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGWARNING("无法从getblockcount的json响应中获取result");
		goto out;
	}
	ret = json_integer_value(res_val);
out:
	json_decref(val);
	return ret;
}

/* 从abcmintd请求指定高度的getblockhash，将值写入*hash，
 * 由于哈希长度为64个字符，*hash应至少有65字节长 */
bool get_blockhash(connsock_t *cs, int height, char *hash)
{
	json_t *val, *res_val;
	const char *res_ret;
	char rpc_req[128];
	bool ret = false;

	sprintf(rpc_req, "{\"method\": \"getblockhash\", \"params\": [%d]}\n", height);
	val = json_rpc_call(cs, rpc_req);
	if (!val) {
		LOGWARNING("%s:%s 无法获取getblockhash的有效json响应", cs->url, cs->port);
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGWARNING("无法从getblockhash的json响应中获取result");
		goto out;
	}
	res_ret = json_string_value(res_val);
	if (!res_ret || !strlen(res_ret)) {
		LOGWARNING("getblockhash的结果为空字符串");
		goto out;
	}
	strncpy(hash, res_ret, 65);
	ret = true;
out:
	json_decref(val);
	return ret;
}

static const char *bestblockhash_req = "{\"method\": \"getblockhash\", \"params\": [-1]}\n";

/* 从abcmintd请求getblockhash -1作为最佳区块哈希 */
bool get_bestblockhash(connsock_t *cs, char *hash)
{
	json_t *val, *res_val;
	const char *res_ret;
	bool ret = false;

	val = json_rpc_call(cs, bestblockhash_req);
	if (!val) {
		LOGWARNING("%s:%s 无法获取最佳区块哈希的有效json响应", cs->url, cs->port);
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGWARNING("无法从getblockhash -1的json响应中获取result");
		goto out;
	}
	res_ret = json_string_value(res_val);
	if (!res_ret || !strlen(res_ret)) {
		LOGWARNING("getblockhash -1的结果为空字符串");
		goto out;
	}
	strncpy(hash, res_ret, 65);
	ret = true;
out:
	json_decref(val);
	return ret;
}

bool submit_block(connsock_t *cs, const char *params)
{
	json_t *val, *res_val;
	int len, retries = 0;
	const char *res_ret;
	bool ret = false;
	char *rpc_req;

	len = strlen(params) + 64;
retry:
	rpc_req = ckalloc(len);
	sprintf(rpc_req, "{\"method\": \"submitblock\", \"params\": [\"%s\"]}\n", params);
	val = json_rpc_call(cs, rpc_req);
	dealloc(rpc_req);
	if (!val) {
		LOGWARNING("%s:%s 无法获取submitblock的有效json响应", cs->url, cs->port);
		if (++retries < 5)
			goto retry;
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGWARNING("无法从submitblock的json响应中获取result");
		if (++retries < 5) {
			json_decref(val);
			goto retry;
		}
		goto out;
	}
	if (!json_is_null(res_val)) {
		res_ret = json_string_value(res_val);
		if (res_ret && strlen(res_ret)) {
			LOGWARNING("提交区块返回: %s", res_ret);
			/* 将duplicate响应视为区块已接受 */
			if (safecmp(res_ret, "duplicate"))
				goto out;
		} else {
			LOGWARNING("提交区块无响应!");
			goto out;
		}
	}
	LOGWARNING("区块已接受!");
	ret = true;
out:
	json_decref(val);
	return ret;
}

void precious_block(connsock_t *cs, const char *params)
{
	char *rpc_req;
	int len;

	if (unlikely(!cs->alive)) {
		LOGDEBUG("由于连接套接字关闭，无法执行precious_block操作");
		return;
	}

	/* 检查abcmintd是否支持preciousblock命令 */
	len = strlen(params) + 64;
	rpc_req = ckalloc(len);
	sprintf(rpc_req, "{\"method\": \"preciousblock\", \"params\": [\"%s\"]}\n", params);
	json_rpc_msg(cs, rpc_req);
	dealloc(rpc_req);
}

void submit_txn(connsock_t *cs, const char *params)
{
	char *rpc_req;
	int len;

	if (unlikely(!cs->alive)) {
		LOGDEBUG("由于连接套接字关闭，无法提交交易");
		return;
	}

	len = strlen(params) + 64;
	rpc_req = ckalloc(len);
	sprintf(rpc_req, "{\"method\": \"sendrawtransaction\", \"params\": [\"%s\"]}\n", params);
	json_rpc_msg(cs, rpc_req);
	dealloc(rpc_req);
}

char *get_txn(connsock_t *cs, const char *hash)
{
	char *rpc_req, *ret = NULL;
	json_t *val, *res_val;

	if (unlikely(!cs->alive)) {
		LOGDEBUG("由于连接套接字关闭，无法获取交易");
		goto out;
	}

	ASPRINTF(&rpc_req, "{\"method\": \"getrawtransaction\", \"params\": [\"%s\"]}\n", hash);
	val = json_rpc_response(cs, rpc_req);
	dealloc(rpc_req);
	if (!val) {
		LOGDEBUG("%s:%s 无法获取get_txn的有效json响应", cs->url, cs->port);
		goto out;
	}
	res_val = json_object_get(val, "result");
	if (res_val && !json_is_null(res_val) && json_is_string(res_val)) {
		ret = strdup(json_string_value(res_val));
		LOGDEBUG("获取哈希 %s 的交易数据: %s", hash, ret);
	} else
		LOGDEBUG("无法获取哈希 %s 的交易数据", hash);
	json_decref(val);
out:
	return ret;
}
