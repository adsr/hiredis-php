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
#include "php_hiredis.h"

#include "main/SAPI.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "SAPI.h"

#include <hiredis.h>

#define Z_HIREDIS_P(zv) hiredis_obj_fetch(Z_OBJ_P((zv)))

static zend_object_handlers hiredis_obj_handlers;
zend_class_entry *hiredis_ce;

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_none, 0, 0, 0)
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
    ZEND_ARG_INFO(0, command)
    ZEND_ARG_INFO(0, command_args)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_command_array, 0, 0, 2)
    ZEND_ARG_INFO(0, command)
    ZEND_ARG_INFO(0, command_argv)
ZEND_END_ARG_INFO()


/* Macro to set err and errstr together */
#define HIREDIS_SET_ERROR(client, perr, perrstr) do { \
    (client)->err = (perr); \
    snprintf((client)->errstr, sizeof((client)->errstr), "%s", (perrstr)); \
} while(0)

/* Macro to ensure ctx is not NULL */
#define HIREDIS_ENSURE_CTX(client) do { \
    if (!(client)->ctx) { \
        HIREDIS_SET_ERROR(client, REDIS_ERR, "No redisContext"); \
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
        HIREDIS_SET_ERROR(client, REDIS_ERR, "redisSetTimeout failed");
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Wrap redisKeepAlive */
static int _hiredis_set_keep_alive_int(hiredis_t* client, int interval) {
    if (REDIS_OK != redisKeepAlive(client->ctx, interval)) {
        HIREDIS_SET_ERROR(client, REDIS_ERR, "redisKeepAlive failed");
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
    ZVAL_STRINGL(z, str, len);
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

/* Implementation of hiredis_(append_)?command(_array)? */
static void _hiredis_command(INTERNAL_FUNCTION_PARAMETERS, int is_array, int is_append) {
    hiredis_t* client;
    zval* zobj;
    zval* args;
    zval* newargs = NULL;
    zval* zv;
    int argc;
    int i;
    char** string_args;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O+", &zobj, hiredis_ce, &args, &argc) == FAILURE) {
        RETURN_FALSE;
    }

    if ((is_array && argc != 2) || (!is_array && argc < 1)) {
        WRONG_PARAM_COUNT;
    }

    client = Z_HIREDIS_P(zobj);
    HIREDIS_ENSURE_CTX(client);

    if (is_array) {
        zval* arr;
        arr = &args[1];
        if (Z_TYPE_P(arr) != IS_ARRAY) {
            SEPARATE_ZVAL(arr);
            convert_to_array(arr);
        }
        argc = 1 + zend_hash_num_elements(Z_ARRVAL_P(arr));
        newargs = (zval*)safe_emalloc(argc, sizeof(zval), 0);
        i = 0;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(arr), zv) {
            ZVAL_COPY_VALUE(&newargs[i], zv);
            i++;
        } ZEND_HASH_FOREACH_END();
        args = newargs;
    }

    string_args = (char**)safe_emalloc(argc, sizeof(char*), 0);
    for (i = 0; i < argc; i++) {
        convert_to_string_ex(&args[i]);
        string_args[i] = Z_STRVAL_P(&args[i]);
    }

    if (is_append) {
        if (REDIS_OK != redisAppendCommandArgv(client->ctx, argc, (const char**)string_args, NULL)) {
            HIREDIS_SET_ERROR(client, client->err, client->errstr);
            RETVAL_FALSE;
        } else {
            RETVAL_TRUE;
        }
    } else {
        if (zv = redisCommandArgv(client->ctx, argc, (const char**)string_args, NULL)) {
            RETVAL_ZVAL(zv, 0, 0);
        } else {
            HIREDIS_SET_ERROR(client, REDIS_ERR, "redisCommandArgv returned NULL");
            RETVAL_FALSE;
        }
    }

    if (newargs) {
        efree(newargs);
    }
    efree(string_args);
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
        HIREDIS_SET_ERROR(client, REDIS_ERR, "redisConnect returned NULL");
        RETURN_FALSE;
    } else if (client->ctx->err) {
        HIREDIS_SET_ERROR(client, client->ctx->err, client->ctx->errstr);
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
        HIREDIS_SET_ERROR(client, REDIS_ERR, "redisConnectUnix returned NULL");
        RETURN_FALSE;
    } else if (client->ctx->err) {
        HIREDIS_SET_ERROR(client, client->ctx->err, client->ctx->errstr);
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
PHP_FUNCTION(hiredis_reconnect) {
    zval* zobj;
    hiredis_t* client;
    char* path;
    size_t path_len;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    HIREDIS_ENSURE_CTX(client);
    if (REDIS_OK != redisReconnect(client->ctx)) {
        HIREDIS_SET_ERROR(client, client->ctx->err, client->ctx->errstr);
        RETURN_FALSE;
    }
    if (REDIS_OK != _hiredis_conn_init(client)) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
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

/* {{{ proto mixed hiredis_command(string fmt, args...)
   Execute command and return result. */
PHP_FUNCTION(hiredis_command) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 0);
}
/* }}} */

/* {{{ proto string hiredis_command_array(string fmt, array args)
   Execute command and return result. */
PHP_FUNCTION(hiredis_command_array) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0);
}
/* }}} */

/* {{{ proto string hiredis_append_command(string fmt, args...)
   Append command to pipeline. */
PHP_FUNCTION(hiredis_append_command) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0);
}
/* }}} */

/* {{{ proto string hiredis_append_command_array(string fmt, array args)
   Append command to pipeline. */
PHP_FUNCTION(hiredis_append_command_array) {
    _hiredis_command(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 1);
}
/* }}} */

/* {{{ proto string hiredis_get_reply()
   Get reply from pipeline. */
PHP_FUNCTION(hiredis_get_reply) {
    // TODO
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
    PHP_ME_MAPPING(connect,              hiredis_connect,              arginfo_hiredis_connect,            ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(connectUnix,          hiredis_connect_unix,         arginfo_hiredis_connect_unix,       ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(reconnect,            hiredis_reconnect,            arginfo_hiredis_none,               ZEND_ACC_PUBLIC)
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
