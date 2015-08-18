/*
  +----------------------------------------------------------------------+
  | hiredis                                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License"); you  |
  | may not use this file except in compliance with the License. You may |
  | obtain a copy of the License at                                      |
  | http://www.apache.org/licenses/LICENSE-2.0                           |
  |                                                                      |
  | Unless required by applicable law or agreed to in writing, software  |
  | distributed under the License is distributed on an "AS IS" BASIS,    |
  | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or      |
  | implied. See the License for the specific language governing         |
  | permissions and limitations under the License.                       |
  +----------------------------------------------------------------------+
  | Author: Adam Saponara <adam@atoi.cc>                                 |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/basic_functions.h"
#include "php_hiredis.h"

#include "main/SAPI.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "SAPI.h"

#include <hiredis.h>

#define Z_HIREDIS_P(zv) hiredis_obj_fetch(Z_OBJ_P((zv)))

static zend_object_handlers hiredis_obj_handlers;
zend_class_entry *hiredis_ce;
zend_class_entry *hiredis_exception_ce;

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_none, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_call, 0, 0, 2)
    ZEND_ARG_INFO(0, func_name)
    ZEND_ARG_INFO(0, func_args)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_connect, 0, 0, 2)
    ZEND_ARG_INFO(0, ip)
    ZEND_ARG_INFO(0, port)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_connect_unix, 0, 0, 1)
    ZEND_ARG_INFO(0, path)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_set_timeout, 0, 0, 1)
    ZEND_ARG_INFO(0, timeout_us)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_set_keep_alive_int, 0, 0, 1)
    ZEND_ARG_INFO(0, interval_s)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_set_max_read_buf, 0, 0, 1)
    ZEND_ARG_INFO(0, max_bytes)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_command, 0, 0, 1)
    ZEND_ARG_INFO(0, command_args)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_command_array, 0, 0, 1)
    ZEND_ARG_INFO(0, command_argv)
ZEND_END_ARG_INFO()


/* Macro to set custom err and errstr together */
#define PHP_HIREDIS_SET_ERROR_EX(client, perr, perrstr) do { \
    (client)->err = (perr); \
    snprintf((client)->errstr, sizeof((client)->errstr), "%s", (perrstr)); \
} while(0)

/* Macro to set err and errstr together */
#define PHP_HIREDIS_SET_ERROR(client) do { \
    PHP_HIREDIS_SET_ERROR_EX((client), (client)->ctx->err, (client)->ctx->errstr); \
} while(0)

/* Macro to ensure ctx is not NULL */
#define PHP_HIREDIS_ENSURE_CTX(client) do { \
    if (!(client)->ctx) { \
        PHP_HIREDIS_SET_ERROR_EX(client, REDIS_ERR, "No redisContext"); \
        RETURN_FALSE; \
    } \
} while(0)

/* Fetch hiredis_t inside zend_object */
static inline hiredis_t* hiredis_obj_fetch(zend_object *obj) {
    return (hiredis_t*)((char*)(obj) - XtOffsetOf(hiredis_t, std));
}

/* Allocate new hiredis zend_object */
static inline zend_object* hiredis_obj_new(zend_class_entry *ce) {
    hiredis_t* client;
    client = ecalloc(1, sizeof(hiredis_t) + zend_object_properties_size(ce));
    zend_object_std_init(&client->std, ce);
    object_properties_init(&client->std, ce);
    client->std.handlers = &hiredis_obj_handlers;
    return &client->std;
}

/* Free hiredis zend_object */
static void hiredis_obj_free(zend_object *object) {
    hiredis_t* client = hiredis_obj_fetch(object);
    if (!client) {
        return;
    }
    if (client->ctx) {
        redisFree(client->ctx);
    }
    zend_object_std_dtor(&client->std);
}

/* Wrap redisSetTimeout */
static int _hiredis_set_timeout(hiredis_t* client, long timeout_us) {
    struct timeval timeout_tv;
    timeout_tv.tv_sec = timeout_us / 1000000;
    timeout_tv.tv_usec = timeout_us % 1000000;
    if (REDIS_OK != redisSetTimeout(client->ctx, timeout_tv)) {
        PHP_HIREDIS_SET_ERROR_EX(client, REDIS_ERR, "redisSetTimeout failed");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Wrap redisKeepAlive */
static int _hiredis_set_keep_alive_int(hiredis_t* client, int interval) {
    if (REDIS_OK != redisKeepAlive(client->ctx, interval)) {
        PHP_HIREDIS_SET_ERROR_EX(client, REDIS_ERR, "redisKeepAlive failed");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* redisReplyObjectFunctions: Nest zval in parent array */
static void _hiredis_replyobj_nest(const redisReadTask* task, zval* z) {
    zval* parent;
    if (task && task->parent != NULL) {
        parent = (zval*)task->parent->obj;
        assert(Z_TYPE_P(parent) == IS_ARRAY);
        add_index_zval(parent, task->idx, z);
    }
}

/* redisReplyObjectFunctions: Create string */
static void* hiredis_replyobj_create_string(const redisReadTask* task, char* str, size_t len) {
    zval* z = (zval*)safe_emalloc(1, sizeof(zval), 0);
    if (task->type == REDIS_REPLY_ERROR) {
        object_init_ex(z, hiredis_exception_ce);
        zend_update_property_stringl(hiredis_exception_ce, z, "message", sizeof("message")-1, str, len);
    } else {
        ZVAL_STRINGL(z, str, len);
    }
    _hiredis_replyobj_nest(task, z);
    return (void*)z;
}

/* redisReplyObjectFunctions: Create array */
static void* hiredis_replyobj_create_array(const redisReadTask* task, int len) {
    zval* z = (zval*)safe_emalloc(1, sizeof(zval), 0);
    array_init_size(z, len);
    _hiredis_replyobj_nest(task, z);
    return (void*)z;
}

/* redisReplyObjectFunctions: Create int */
static void* hiredis_replyobj_create_integer(const redisReadTask* task, long long i) {
    zval* z = (zval*)safe_emalloc(1, sizeof(zval), 0);
    ZVAL_LONG(z, i);
    _hiredis_replyobj_nest(task, z);
    return (void*)z;
}

/* redisReplyObjectFunctions: Create nil */
static void* hiredis_replyobj_create_nil(const redisReadTask* task) {
    zval* z = (zval*)safe_emalloc(1, sizeof(zval), 0);
    ZVAL_NULL(z);
    _hiredis_replyobj_nest(task, z);
    return (void*)z;
}

/* redisReplyObjectFunctions: Free object */
static void hiredis_replyobj_free(void* obj) {
    // TODO confirm gc, else efree((zval*)obj);
}

/* Declare reply object funcs */
static redisReplyObjectFunctions hiredis_replyobj_funcs = {
    hiredis_replyobj_create_string,
    hiredis_replyobj_create_array,
    hiredis_replyobj_create_integer,
    hiredis_replyobj_create_nil,
    hiredis_replyobj_free
};

/* Convert zval of type array to a C array of zvals. Call must free ret_zvals. */
static void _hiredis_convert_zval_to_array_of_zvals(zval* arr, zval** ret_zvals, int* ret_num_zvals) {
    zval* zvals;
    zval* zv;
    int argc;
    int i;
    if (Z_TYPE_P(arr) != IS_ARRAY) {
        SEPARATE_ZVAL(arr);
        convert_to_array(arr);
    }
    argc = zend_hash_num_elements(Z_ARRVAL_P(arr));
    zvals = (zval*)safe_emalloc(argc, sizeof(zval), 0);
    i = 0;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(arr), zv) {
        ZVAL_COPY_VALUE(&zvals[i], zv);
        i++;
    } ZEND_HASH_FOREACH_END();
    *ret_zvals = zvals;
    *ret_num_zvals = argc;
}

/* Implementation of Hiredis::__call */
static void _hiredis_command_array(INTERNAL_FUNCTION_PARAMETERS, hiredis_t* client, char* cmd, zval* args, int argc, int is_append) {
    char** string_args;
    size_t* string_lens;
    int i, j;
    zval* zv;
    int num_strings;

    // Convert array of zvals to string + stringlen params. If cmd is not NULL
    // make it the first arg.
    num_strings = cmd ? argc + 1 : argc;
    string_args = (char**)safe_emalloc(num_strings, sizeof(char*), 0);
    string_lens = (size_t*)safe_emalloc(num_strings, sizeof(size_t), 0);
    j = 0;
    if (cmd) {
        string_args[j] = cmd;
        string_lens[j] = strlen(cmd);
        j++;
    }
    for (i = 0; i < argc; i++, j++) {
        convert_to_string_ex(&args[i]);
        string_args[j] = Z_STRVAL_P(&args[i]);
        string_lens[j] = Z_STRLEN_P(&args[i]);
    }

    // Send command to server
    if (is_append) {
        if (REDIS_OK != redisAppendCommandArgv(client->ctx, num_strings, (const char**)string_args, string_lens)) {
            PHP_HIREDIS_SET_ERROR(client);
            RETVAL_FALSE;
        } else {
            RETVAL_TRUE;
        }
    } else {
        if (zv = redisCommandArgv(client->ctx, num_strings, (const char**)string_args, string_lens)) {
            RETVAL_ZVAL(zv, 0, 0);
        } else {
            PHP_HIREDIS_SET_ERROR(client);//, REDIS_ERR, "redisCommandArgv returned NULL");
            RETVAL_FALSE;
        }
    }

    // Cleanup
    efree(string_args);
    efree(string_lens);
}

/* Implementation of hiredis_(append_)?command(_array)? */
static void _hiredis_command(INTERNAL_FUNCTION_PARAMETERS, int is_array, int is_append) {
    hiredis_t* client;
    zval* zobj;
    zval* args;
    int argc;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O+", &zobj, hiredis_ce, &args, &argc) == FAILURE) {
        RETURN_FALSE;
    }
    if ((is_array && argc != 1) || (!is_array && argc < 1)) {
        WRONG_PARAM_COUNT;
    }

    client = Z_HIREDIS_P(zobj);
    PHP_HIREDIS_ENSURE_CTX(client);

    if (is_array) _hiredis_convert_zval_to_array_of_zvals(args, &args, &argc);
    _hiredis_command_array(INTERNAL_FUNCTION_PARAM_PASSTHRU, client, NULL, args, argc, is_append);
    if (is_array) efree(args);
}

/* Invoked after connecting */
static int _hiredis_conn_init(hiredis_t* client) {
    int rc;
    rc = REDIS_OK;
    if (client->keep_alive_int_s >= 0) {
        if (REDIS_OK != _hiredis_set_keep_alive_int(client, client->keep_alive_int_s)) {
            rc = REDIS_ERR;
        }
    }
    if (client->timeout_us >= 0) {
        if (REDIS_OK != _hiredis_set_timeout(client, client->timeout_us)) {
            rc = REDIS_ERR;
        }
    }
    client->ctx->reader->maxbuf = client->max_read_buf;
    client->ctx->reader->fn = &hiredis_replyobj_funcs;
    return rc;
}

/* Invoked before connecting and at __destruct */
static void _hiredis_conn_deinit(hiredis_t* client) {
    if (client->ctx) {
        redisFree(client->ctx);
    }
    client->ctx = NULL;
}

/* {{{ proto void Hiredis::__construct()
   Constructor for Hiredis. */
PHP_METHOD(Hiredis, __construct) {
    hiredis_t* client;
    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }
    return_value = getThis();
    client = Z_HIREDIS_P(return_value);
    client->ctx = NULL;
    client->timeout_us = -1;
    client->keep_alive_int_s = -1;
    client->max_read_buf = REDIS_READER_MAX_BUF;
}
/* }}} */

/* {{{ proto void Hiredis::__destruct()
   Destructor for Hiredis. */
PHP_METHOD(Hiredis, __destruct) {
    hiredis_t* client;
    return_value = getThis();
    client = Z_HIREDIS_P(return_value);
    _hiredis_conn_deinit(client);
}
/* }}} */

/* {{{ proto void Hiredis::__call(string func, array args)
   Magic command handler. */
PHP_METHOD(Hiredis, __call) {
    hiredis_t* client;
    char* func;
    size_t func_len;
    zval* func_args;
    int func_argc;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sa", &func, &func_len, &func_args) == FAILURE) {
        RETURN_FALSE;
    }

    client = Z_HIREDIS_P(getThis());
    PHP_HIREDIS_ENSURE_CTX(client);

    _hiredis_convert_zval_to_array_of_zvals(func_args, &func_args, &func_argc);

    #define PHP_HIREDIS_ELSE_IF_CALL(pfunc, pcmd) \
        } else if (0 == strcasecmp(func, (pfunc))) { \
            _hiredis_command_array(INTERNAL_FUNCTION_PARAM_PASSTHRU, client, (pcmd), func_args, func_argc, 0)

    if (0) {
    PHP_HIREDIS_ELSE_IF_CALL("append", "APPEND");
    PHP_HIREDIS_ELSE_IF_CALL("auth", "AUTH");
    PHP_HIREDIS_ELSE_IF_CALL("bgrewriteaof", "BGREWRITEAOF");
    PHP_HIREDIS_ELSE_IF_CALL("bgsave", "BGSAVE");
    PHP_HIREDIS_ELSE_IF_CALL("bitcount", "BITCOUNT");
    PHP_HIREDIS_ELSE_IF_CALL("bitop", "BITOP");
    PHP_HIREDIS_ELSE_IF_CALL("bitpos", "BITPOS");
    PHP_HIREDIS_ELSE_IF_CALL("blpop", "BLPOP");
    PHP_HIREDIS_ELSE_IF_CALL("brpop", "BRPOP");
    PHP_HIREDIS_ELSE_IF_CALL("brpoplpush", "BRPOPLPUSH");
    PHP_HIREDIS_ELSE_IF_CALL("client", "CLIENT");
    PHP_HIREDIS_ELSE_IF_CALL("cluster", "CLUSTER");
    PHP_HIREDIS_ELSE_IF_CALL("command", "COMMAND");
    PHP_HIREDIS_ELSE_IF_CALL("config", "CONFIG");
    PHP_HIREDIS_ELSE_IF_CALL("dbsize", "DBSIZE");
    PHP_HIREDIS_ELSE_IF_CALL("debug", "DEBUG");
    PHP_HIREDIS_ELSE_IF_CALL("decr", "DECR");
    PHP_HIREDIS_ELSE_IF_CALL("decrby", "DECRBY");
    PHP_HIREDIS_ELSE_IF_CALL("del", "DEL");
    PHP_HIREDIS_ELSE_IF_CALL("discard", "DISCARD");
    PHP_HIREDIS_ELSE_IF_CALL("dump", "DUMP");
    PHP_HIREDIS_ELSE_IF_CALL("echo", "ECHO");
    PHP_HIREDIS_ELSE_IF_CALL("eval", "EVAL");
    PHP_HIREDIS_ELSE_IF_CALL("evalsha", "EVALSHA");
    PHP_HIREDIS_ELSE_IF_CALL("exec", "EXEC");
    PHP_HIREDIS_ELSE_IF_CALL("exists", "EXISTS");
    PHP_HIREDIS_ELSE_IF_CALL("expire", "EXPIRE");
    PHP_HIREDIS_ELSE_IF_CALL("expireat", "EXPIREAT");
    PHP_HIREDIS_ELSE_IF_CALL("flushall", "FLUSHALL");
    PHP_HIREDIS_ELSE_IF_CALL("flushdb", "FLUSHDB");
    PHP_HIREDIS_ELSE_IF_CALL("geoadd", "GEOADD");
    PHP_HIREDIS_ELSE_IF_CALL("geodist", "GEODIST");
    PHP_HIREDIS_ELSE_IF_CALL("geohash", "GEOHASH");
    PHP_HIREDIS_ELSE_IF_CALL("geopos", "GEOPOS");
    PHP_HIREDIS_ELSE_IF_CALL("georadius", "GEORADIUS");
    PHP_HIREDIS_ELSE_IF_CALL("georadiusbymember", "GEORADIUSBYMEMBER");
    PHP_HIREDIS_ELSE_IF_CALL("get", "GET");
    PHP_HIREDIS_ELSE_IF_CALL("getbit", "GETBIT");
    PHP_HIREDIS_ELSE_IF_CALL("getrange", "GETRANGE");
    PHP_HIREDIS_ELSE_IF_CALL("getset", "GETSET");
    PHP_HIREDIS_ELSE_IF_CALL("hdel", "HDEL");
    PHP_HIREDIS_ELSE_IF_CALL("hexists", "HEXISTS");
    PHP_HIREDIS_ELSE_IF_CALL("hget", "HGET");
    PHP_HIREDIS_ELSE_IF_CALL("hgetall", "HGETALL");
    PHP_HIREDIS_ELSE_IF_CALL("hincrby", "HINCRBY");
    PHP_HIREDIS_ELSE_IF_CALL("hincrbyfloat", "HINCRBYFLOAT");
    PHP_HIREDIS_ELSE_IF_CALL("hkeys", "HKEYS");
    PHP_HIREDIS_ELSE_IF_CALL("hlen", "HLEN");
    PHP_HIREDIS_ELSE_IF_CALL("hmget", "HMGET");
    PHP_HIREDIS_ELSE_IF_CALL("hmset", "HMSET");
    PHP_HIREDIS_ELSE_IF_CALL("hscan", "HSCAN");
    PHP_HIREDIS_ELSE_IF_CALL("hset", "HSET");
    PHP_HIREDIS_ELSE_IF_CALL("hsetnx", "HSETNX");
    PHP_HIREDIS_ELSE_IF_CALL("hstrlen", "HSTRLEN");
    PHP_HIREDIS_ELSE_IF_CALL("hvals", "HVALS");
    PHP_HIREDIS_ELSE_IF_CALL("incr", "INCR");
    PHP_HIREDIS_ELSE_IF_CALL("incrby", "INCRBY");
    PHP_HIREDIS_ELSE_IF_CALL("incrbyfloat", "INCRBYFLOAT");
    PHP_HIREDIS_ELSE_IF_CALL("info", "INFO");
    PHP_HIREDIS_ELSE_IF_CALL("keys", "KEYS");
    PHP_HIREDIS_ELSE_IF_CALL("lastsave", "LASTSAVE");
    PHP_HIREDIS_ELSE_IF_CALL("lindex", "LINDEX");
    PHP_HIREDIS_ELSE_IF_CALL("linsert", "LINSERT");
    PHP_HIREDIS_ELSE_IF_CALL("llen", "LLEN");
    PHP_HIREDIS_ELSE_IF_CALL("lpop", "LPOP");
    PHP_HIREDIS_ELSE_IF_CALL("lpush", "LPUSH");
    PHP_HIREDIS_ELSE_IF_CALL("lpushx", "LPUSHX");
    PHP_HIREDIS_ELSE_IF_CALL("lrange", "LRANGE");
    PHP_HIREDIS_ELSE_IF_CALL("lrem", "LREM");
    PHP_HIREDIS_ELSE_IF_CALL("lset", "LSET");
    PHP_HIREDIS_ELSE_IF_CALL("ltrim", "LTRIM");
    PHP_HIREDIS_ELSE_IF_CALL("mget", "MGET");
    PHP_HIREDIS_ELSE_IF_CALL("migrate", "MIGRATE");
    PHP_HIREDIS_ELSE_IF_CALL("monitor", "MONITOR");
    PHP_HIREDIS_ELSE_IF_CALL("move", "MOVE");
    PHP_HIREDIS_ELSE_IF_CALL("mset", "MSET");
    PHP_HIREDIS_ELSE_IF_CALL("msetnx", "MSETNX");
    PHP_HIREDIS_ELSE_IF_CALL("multi", "MULTI");
    PHP_HIREDIS_ELSE_IF_CALL("object", "OBJECT");
    PHP_HIREDIS_ELSE_IF_CALL("persist", "PERSIST");
    PHP_HIREDIS_ELSE_IF_CALL("pexpire", "PEXPIRE");
    PHP_HIREDIS_ELSE_IF_CALL("pexpireat", "PEXPIREAT");
    PHP_HIREDIS_ELSE_IF_CALL("pfadd", "PFADD");
    PHP_HIREDIS_ELSE_IF_CALL("pfcount", "PFCOUNT");
    PHP_HIREDIS_ELSE_IF_CALL("pfmerge", "PFMERGE");
    PHP_HIREDIS_ELSE_IF_CALL("ping", "PING");
    PHP_HIREDIS_ELSE_IF_CALL("psetex", "PSETEX");
    PHP_HIREDIS_ELSE_IF_CALL("psubscribe", "PSUBSCRIBE");
    PHP_HIREDIS_ELSE_IF_CALL("pttl", "PTTL");
    PHP_HIREDIS_ELSE_IF_CALL("publish", "PUBLISH");
    PHP_HIREDIS_ELSE_IF_CALL("pubsub", "PUBSUB");
    PHP_HIREDIS_ELSE_IF_CALL("punsubscribe", "PUNSUBSCRIBE");
    PHP_HIREDIS_ELSE_IF_CALL("quit", "QUIT");
    PHP_HIREDIS_ELSE_IF_CALL("randomkey", "RANDOMKEY");
    PHP_HIREDIS_ELSE_IF_CALL("rename", "RENAME");
    PHP_HIREDIS_ELSE_IF_CALL("renamenx", "RENAMENX");
    PHP_HIREDIS_ELSE_IF_CALL("restore", "RESTORE");
    PHP_HIREDIS_ELSE_IF_CALL("role", "ROLE");
    PHP_HIREDIS_ELSE_IF_CALL("rpop", "RPOP");
    PHP_HIREDIS_ELSE_IF_CALL("rpoplpush", "RPOPLPUSH");
    PHP_HIREDIS_ELSE_IF_CALL("rpush", "RPUSH");
    PHP_HIREDIS_ELSE_IF_CALL("rpushx", "RPUSHX");
    PHP_HIREDIS_ELSE_IF_CALL("sadd", "SADD");
    PHP_HIREDIS_ELSE_IF_CALL("save", "SAVE");
    PHP_HIREDIS_ELSE_IF_CALL("scan", "SCAN");
    PHP_HIREDIS_ELSE_IF_CALL("scard", "SCARD");
    PHP_HIREDIS_ELSE_IF_CALL("script", "SCRIPT");
    PHP_HIREDIS_ELSE_IF_CALL("sdiff", "SDIFF");
    PHP_HIREDIS_ELSE_IF_CALL("sdiffstore", "SDIFFSTORE");
    PHP_HIREDIS_ELSE_IF_CALL("select", "SELECT");
    PHP_HIREDIS_ELSE_IF_CALL("set", "SET");
    PHP_HIREDIS_ELSE_IF_CALL("setbit", "SETBIT");
    PHP_HIREDIS_ELSE_IF_CALL("setex", "SETEX");
    PHP_HIREDIS_ELSE_IF_CALL("setnx", "SETNX");
    PHP_HIREDIS_ELSE_IF_CALL("setrange", "SETRANGE");
    PHP_HIREDIS_ELSE_IF_CALL("shutdown", "SHUTDOWN");
    PHP_HIREDIS_ELSE_IF_CALL("sinter", "SINTER");
    PHP_HIREDIS_ELSE_IF_CALL("sinterstore", "SINTERSTORE");
    PHP_HIREDIS_ELSE_IF_CALL("sismember", "SISMEMBER");
    PHP_HIREDIS_ELSE_IF_CALL("slaveof", "SLAVEOF");
    PHP_HIREDIS_ELSE_IF_CALL("slowlog", "SLOWLOG");
    PHP_HIREDIS_ELSE_IF_CALL("smembers", "SMEMBERS");
    PHP_HIREDIS_ELSE_IF_CALL("smove", "SMOVE");
    PHP_HIREDIS_ELSE_IF_CALL("sort", "SORT");
    PHP_HIREDIS_ELSE_IF_CALL("spop", "SPOP");
    PHP_HIREDIS_ELSE_IF_CALL("srandmember", "SRANDMEMBER");
    PHP_HIREDIS_ELSE_IF_CALL("srem", "SREM");
    PHP_HIREDIS_ELSE_IF_CALL("sscan", "SSCAN");
    PHP_HIREDIS_ELSE_IF_CALL("strlen", "STRLEN");
    PHP_HIREDIS_ELSE_IF_CALL("subscribe", "SUBSCRIBE");
    PHP_HIREDIS_ELSE_IF_CALL("sunion", "SUNION");
    PHP_HIREDIS_ELSE_IF_CALL("sunionstore", "SUNIONSTORE");
    PHP_HIREDIS_ELSE_IF_CALL("sync", "SYNC");
    PHP_HIREDIS_ELSE_IF_CALL("time", "TIME");
    PHP_HIREDIS_ELSE_IF_CALL("ttl", "TTL");
    PHP_HIREDIS_ELSE_IF_CALL("type", "TYPE");
    PHP_HIREDIS_ELSE_IF_CALL("unsubscribe", "UNSUBSCRIBE");
    PHP_HIREDIS_ELSE_IF_CALL("unwatch", "UNWATCH");
    PHP_HIREDIS_ELSE_IF_CALL("wait", "WAIT");
    PHP_HIREDIS_ELSE_IF_CALL("watch", "WATCH");
    PHP_HIREDIS_ELSE_IF_CALL("zadd", "ZADD");
    PHP_HIREDIS_ELSE_IF_CALL("zcard", "ZCARD");
    PHP_HIREDIS_ELSE_IF_CALL("zcount", "ZCOUNT");
    PHP_HIREDIS_ELSE_IF_CALL("zincrby", "ZINCRBY");
    PHP_HIREDIS_ELSE_IF_CALL("zinterstore", "ZINTERSTORE");
    PHP_HIREDIS_ELSE_IF_CALL("zlexcount", "ZLEXCOUNT");
    PHP_HIREDIS_ELSE_IF_CALL("zrange", "ZRANGE");
    PHP_HIREDIS_ELSE_IF_CALL("zrangebylex", "ZRANGEBYLEX");
    PHP_HIREDIS_ELSE_IF_CALL("zrangebyscore", "ZRANGEBYSCORE");
    PHP_HIREDIS_ELSE_IF_CALL("zrank", "ZRANK");
    PHP_HIREDIS_ELSE_IF_CALL("zrem", "ZREM");
    PHP_HIREDIS_ELSE_IF_CALL("zremrangebylex", "ZREMRANGEBYLEX");
    PHP_HIREDIS_ELSE_IF_CALL("zremrangebyrank", "ZREMRANGEBYRANK");
    PHP_HIREDIS_ELSE_IF_CALL("zremrangebyscore", "ZREMRANGEBYSCORE");
    PHP_HIREDIS_ELSE_IF_CALL("zrevrange", "ZREVRANGE");
    PHP_HIREDIS_ELSE_IF_CALL("zrevrangebylex", "ZREVRANGEBYLEX");
    PHP_HIREDIS_ELSE_IF_CALL("zrevrangebyscore", "ZREVRANGEBYSCORE");
    PHP_HIREDIS_ELSE_IF_CALL("zrevrank", "ZREVRANK");
    PHP_HIREDIS_ELSE_IF_CALL("zscan", "ZSCAN");
    PHP_HIREDIS_ELSE_IF_CALL("zscore", "ZSCORE");
    PHP_HIREDIS_ELSE_IF_CALL("zunionstore", "ZUNIONSTORE");
    } else {
        zend_throw_error(NULL, "Call to undefined method Hiredis::%s()", func);
    }

    #undef PHP_HIREDIS_ELSE_IF_CALL

    efree(func_args);
}
/* }}} */

/* {{{ proto bool hiredis_connect(string ip, int port)
   Connect to a server via TCP. */
PHP_FUNCTION(hiredis_connect) {
    zval* zobj;
    hiredis_t* client;
    char* ip;
    size_t ip_len;
    long port;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Osl", &zobj, hiredis_ce, &ip, &ip_len, &port) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    _hiredis_conn_deinit(client);
    if (!(client->ctx = redisConnect(ip, port))) {
        PHP_HIREDIS_SET_ERROR_EX(client, REDIS_ERR, "redisConnect returned NULL");
        RETURN_FALSE;
    } else if (client->ctx->err) {
        PHP_HIREDIS_SET_ERROR(client);
        RETURN_FALSE;
    }
    if (REDIS_OK == _hiredis_conn_init(client)) {
        RETURN_TRUE;
    }
    RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool hiredis_connect_unix(string path)
   Connect to a server via Unix socket. */
PHP_FUNCTION(hiredis_connect_unix) {
    zval* zobj;
    hiredis_t* client;
    char* path;
    size_t path_len;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Os", &zobj, hiredis_ce, &path, &path_len) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    _hiredis_conn_deinit(client);
    if (!(client->ctx = redisConnectUnix(path))) {
        PHP_HIREDIS_SET_ERROR_EX(client, REDIS_ERR, "redisConnectUnix returned NULL");
        RETURN_FALSE;
    } else if (client->ctx->err) {
        PHP_HIREDIS_SET_ERROR(client);
        RETURN_FALSE;
    }
    if (REDIS_OK != _hiredis_conn_init(client)) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool hiredis_reconnect()
   Reonnect to a server. */
#ifdef HAVE_HIREDIS_RECONNECT
PHP_FUNCTION(hiredis_reconnect) {
    zval* zobj;
    hiredis_t* client;
    char* path;
    size_t path_len;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    PHP_HIREDIS_ENSURE_CTX(client);
    if (REDIS_OK != redisReconnect(client->ctx)) {
        PHP_HIREDIS_SET_ERROR(client);
        RETURN_FALSE;
    }
    if (REDIS_OK != _hiredis_conn_init(client)) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
#endif
/* }}} */

/* {{{ proto bool hiredis_set_timeout(int timeout_us)
   Set read/write timeout in microseconds. */
PHP_FUNCTION(hiredis_set_timeout) {
    zval* zobj;
    hiredis_t* client;
    long timeout_us;
    struct timeval timeout_tv;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, hiredis_ce, &timeout_us) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    client->timeout_us = timeout_us;
    if (!client->ctx) {
        RETURN_TRUE;
    }
    if (REDIS_OK != _hiredis_set_timeout(client, timeout_us)) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto int hiredis_get_timeout()
   Get read/write timeout in microseconds. */
PHP_FUNCTION(hiredis_get_timeout) {
    zval* zobj;
    hiredis_t* client;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    RETURN_LONG(client->timeout_us);
}
/* }}} */

/* {{{ proto bool hiredis_set_keep_alive_int(int keep_alive_int_s)
   Set keep alive interval in seconds. */
PHP_FUNCTION(hiredis_set_keep_alive_int) {
    zval* zobj;
    hiredis_t* client;
    long keep_alive_int_s;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, hiredis_ce, &keep_alive_int_s) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    client->keep_alive_int_s = keep_alive_int_s;
    if (!client->ctx) {
        RETURN_TRUE;
    }
    if (REDIS_OK != _hiredis_set_keep_alive_int(client, keep_alive_int_s)) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto int hiredis_get_keep_alive_int()
   Get keep alive interval in seconds.*/
PHP_FUNCTION(hiredis_get_keep_alive_int) {
    zval* zobj;
    hiredis_t* client;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    RETURN_LONG(client->keep_alive_int_s);
}
/* }}} */

/* {{{ proto bool hiredis_set_max_read_buf(int max_bytes)
   Set max read buffer in bytes. */
PHP_FUNCTION(hiredis_set_max_read_buf) {
    zval* zobj;
    hiredis_t* client;
    long max_bytes;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ol", &zobj, hiredis_ce, &max_bytes) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    client->max_read_buf = max_bytes;
    if (client->ctx) {
        client->ctx->reader->maxbuf = max_bytes;
    }
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto int hiredis_get_max_read_buf()
   Get max read buffer in bytes. */
PHP_FUNCTION(hiredis_get_max_read_buf) {
    zval* zobj;
    hiredis_t* client;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    RETURN_LONG(client->max_read_buf);
}
/* }}} */

/* {{{ proto mixed hiredis_command(string args...)
   Execute command and return result. */
PHP_FUNCTION(hiredis_command) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 0);
}
/* }}} */

/* {{{ proto string hiredis_command_array(array args)
   Execute command and return result. */
PHP_FUNCTION(hiredis_command_array) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0);
}
/* }}} */

/* {{{ proto string hiredis_append_command(string args...)
   Append command to pipeline. */
PHP_FUNCTION(hiredis_append_command) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 1);
}
/* }}} */

/* {{{ proto string hiredis_append_command_array(array args)
   Append command to pipeline. */
PHP_FUNCTION(hiredis_append_command_array) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 1);
}
/* }}} */

/* {{{ proto string hiredis_get_reply()
   Get reply from pipeline. */
PHP_FUNCTION(hiredis_get_reply) {
    zval* zobj;
    hiredis_t* client;
    void* reply = NULL;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    PHP_HIREDIS_ENSURE_CTX(client);
    if (REDIS_OK != redisGetReply(client->ctx, &reply)) {
        PHP_HIREDIS_SET_ERROR(client);
        RETURN_FALSE;
    }
    if (reply) {
        RETURN_ZVAL((zval*)reply, 0, 0);
    }
    RETURN_NULL();
}
/* }}} */

/* {{{ proto string hiredis_get_last_error()
   Get last error string. */
PHP_FUNCTION(hiredis_get_last_error) {
    zval* zobj;
    hiredis_t* client;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    if (client->err) {
        RETURN_STRING(client->errstr);
    }
    RETURN_NULL();
}
/* }}} */



/* {{{ hiredis_methods */
zend_function_entry hiredis_methods[] = {
    PHP_ME(Hiredis, __construct, arginfo_hiredis_none, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
    PHP_ME(Hiredis, __destruct,  arginfo_hiredis_none, ZEND_ACC_PUBLIC)
    PHP_ME(Hiredis, __call,      arginfo_hiredis_call, ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(connect,              hiredis_connect,              arginfo_hiredis_connect,            ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(connectUnix,          hiredis_connect_unix,         arginfo_hiredis_connect_unix,       ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setTimeout,           hiredis_set_timeout,          arginfo_hiredis_set_timeout,        ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getTimeout,           hiredis_get_timeout,          arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setKeepAliveInterval, hiredis_set_keep_alive_int,   arginfo_hiredis_set_keep_alive_int, ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getKeepAliveInterval, hiredis_get_keep_alive_int,   arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setMaxReadBuf,        hiredis_set_max_read_buf,     arginfo_hiredis_set_max_read_buf,   ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getMaxReadBuf,        hiredis_get_max_read_buf,     arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(command,              hiredis_command,              arginfo_hiredis_command,            ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(commandArray,         hiredis_command_array,        arginfo_hiredis_command_array,      ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(appendCommand,        hiredis_append_command,       arginfo_hiredis_command,            ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(appendCommandArray,   hiredis_append_command_array, arginfo_hiredis_command_array,      ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getReply,             hiredis_get_reply,            arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getLastError,         hiredis_get_last_error,       arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
#ifdef HAVE_HIREDIS_RECONNECT
    PHP_ME_MAPPING(reconnect,            hiredis_reconnect,            arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
#endif
    PHP_FE_END
};
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(hiredis) {
    char hiredis_version[32];
    snprintf(hiredis_version, sizeof(hiredis_version), "%d.%d.%d", HIREDIS_MAJOR, HIREDIS_MINOR, HIREDIS_PATCH);
    php_info_print_table_start();
    php_info_print_table_header(2, "hiredis support", "enabled");
    php_info_print_table_row(2, "hiredis module version", PHP_HIREDIS_VERSION);
    php_info_print_table_row(2, "hiredis version", hiredis_version);
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(hiredis) {
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "Hiredis", hiredis_methods);
    hiredis_ce = zend_register_internal_class(&ce);
    hiredis_ce->create_object = hiredis_obj_new;
    memcpy(&hiredis_obj_handlers, zend_get_std_object_handlers(), sizeof(hiredis_obj_handlers));
    hiredis_obj_handlers.offset = XtOffsetOf(hiredis_t, std);
    hiredis_obj_handlers.free_obj = hiredis_obj_free;

    INIT_CLASS_ENTRY(ce, "HiredisException", NULL);
    hiredis_exception_ce = zend_register_internal_class_ex(&ce, zend_ce_exception);
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(hiredis) {
    return SUCCESS;
}
/* }}} */

/* {{{ hiredis_module_entry */
zend_module_entry hiredis_module_entry = {
    STANDARD_MODULE_HEADER,
    "hiredis",
    NULL,
    PHP_MINIT(hiredis),
    PHP_MSHUTDOWN(hiredis),
    NULL,
    NULL,
    PHP_MINFO(hiredis),
    PHP_HIREDIS_VERSION,
    STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_HIREDIS
ZEND_GET_MODULE(hiredis)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
