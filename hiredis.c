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

static zend_object_handlers hiredis_obj_handlers;
static zend_class_entry *hiredis_ce;
static zend_class_entry *hiredis_exception_ce;
static HashTable func_cmd_map;

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_none, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_call, 0, 0, 2)
    ZEND_ARG_INFO(0, func_name)
    ZEND_ARG_INFO(0, func_args)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_connect, 0, 0, 2)
    ZEND_ARG_INFO(0, ip)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_INFO(0, timeout)
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

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_send_raw, 0, 0, 1)
    ZEND_ARG_INFO(0, command_args)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_send_raw_array, 0, 0, 1)
    ZEND_ARG_INFO(0, command_argv)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_hiredis_set_throw_exceptions, 0, 0, 1)
    ZEND_ARG_INFO(0, true_or_false)
ZEND_END_ARG_INFO()

/* Macro to get hiredis_t from zval */
#if PHP_MAJOR_VERSION >= 7
#define Z_HIREDIS_P(zv) hiredis_obj_fetch(Z_OBJ_P((zv)))
#else
#define Z_HIREDIS_P(zv) hiredis_obj_fetch(zv TSRMLS_CC)
#endif

/* Macro to set custom err and errstr together */
#define PHP_HIREDIS_SET_ERROR_EX(client, perr, perrstr) do { \
    (client)->err = (perr); \
    snprintf((client)->errstr, sizeof((client)->errstr), "%s", (perrstr)); \
    if ((client)->throw_exceptions) { \
        zend_throw_exception(hiredis_exception_ce, (client)->errstr, (client)->err); \
    } \
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

/* Macro to handle returning/throwing a zval to userland */
#define PHP_HIREDIS_RETURN_OR_THROW_ZVAL(client, zv) do { \
    if (Z_TYPE_P(zv) == IS_OBJECT && instanceof_function(Z_OBJCE_P(zv), hiredis_exception_ce)) { \
        PHP_HIREDIS_SET_ERROR_EX((client), REDIS_ERR, _hidreis_get_exception_message(zv)); \
        RETVAL_FALSE; \
    } else { \
        RETVAL_ZVAL((zv), 0, 0); \
    } \
} while(0)

/* Return exception message */
static char* _hidreis_get_exception_message(zval* ex) {
    zval* zp = NULL;
    #if PHP_MAJOR_VERSION >= 7
        zval z = {0};
        zp = zend_read_property(hiredis_exception_ce, ex, "message", sizeof("message")-1, 1, &z);
    #else
        zp = zend_read_property(hiredis_exception_ce, ex, "message", sizeof("message")-1, 1);
    #endif
    return Z_STRVAL_P(zp);
}

/* Fetch hiredis_t inside zval */
#if PHP_MAJOR_VERSION >= 7
static inline hiredis_t* hiredis_obj_fetch(zend_object* obj) {
    return (hiredis_t*)((char*)(obj) - XtOffsetOf(hiredis_t, std));
}
#else
static inline hiredis_t* hiredis_obj_fetch(zval* obj TSRMLS_DC) {
    return (hiredis_t*)zend_object_store_get_object(obj TSRMLS_CC);
}
#endif

/* Allocate/deallocate hiredis_t object */
#if PHP_MAJOR_VERSION >= 7
static void hiredis_obj_free(zend_object *object) {
    hiredis_t* client;
    #if PHP_MAJOR_VERSION >= 7
        client = hiredis_obj_fetch(object);
    #else
        client = hiredis_obj_fetch(object);
    #endif
    if (!client) {
        return;
    }
    if (client->ctx) {
        redisFree(client->ctx);
    }
    zend_object_std_dtor(&client->std);
    efree(client);
}
static inline zend_object* hiredis_obj_new(zend_class_entry *ce) {
    hiredis_t* client;
    client = ecalloc(1, sizeof(hiredis_t) + zend_object_properties_size(ce));
    zend_object_std_init(&client->std, ce);
    object_properties_init(&client->std, ce);
    client->std.handlers = &hiredis_obj_handlers;
    return &client->std;
}
#else
static void hiredis_obj_free(void *obj TSRMLS_DC) {
    hiredis_t *client = (hiredis_t*)obj;
    if (!client) {
        return;
    }
    if (client->ctx) {
        redisFree(client->ctx);
    }
    zend_object_std_dtor(&client->std TSRMLS_CC);
    efree(client);
}
static inline zend_object_value hiredis_obj_new(zend_class_entry *ce TSRMLS_DC) {
    hiredis_t* client;
    zend_object_value retval;
    client = ecalloc(1, sizeof(hiredis_t));
    zend_object_std_init(&client->std, ce TSRMLS_CC);
    object_properties_init((zend_object*)client, ce);
    retval.handle = zend_objects_store_put(client, NULL, hiredis_obj_free, NULL TSRMLS_CC);
    retval.handlers = (zend_object_handlers*)&hiredis_obj_handlers;
    return retval;
}
#endif

/* Throw errstr as exception */
static void _hiredis_throw_err_exception(hiredis_t* client) {
    zend_throw_exception(hiredis_exception_ce, client->errstr, client->err);
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
        #if PHP_MAJOR_VERSION >= 7
        ZVAL_STRINGL(z, str, len);
        #else
        ZVAL_STRINGL(z, str, len, 1);
        #endif
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
        convert_to_array(arr);
    }
    argc = zend_hash_num_elements(Z_ARRVAL_P(arr));
    zvals = (zval*)safe_emalloc(argc, sizeof(zval), 0);
    i = 0;

    #if PHP_MAJOR_VERSION >= 7
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(arr), zv) {
            ZVAL_COPY_VALUE(&zvals[i], zv);
            i++;
        } ZEND_HASH_FOREACH_END();
    #else
        HashPosition hash_pos = NULL;
        zval** hash_entry = NULL;
        zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(arr), &hash_pos);
        while (zend_hash_get_current_data_ex(Z_ARRVAL_P(arr), (void**)&hash_entry, &hash_pos) == SUCCESS) {
            ZVAL_COPY_VALUE(&zvals[i], *hash_entry);
            i++;
            zend_hash_move_forward_ex(Z_ARRVAL_P(arr), &hash_pos);
        }
    #endif

    *ret_zvals = zvals;
    *ret_num_zvals = argc;
}

/* Actually send/queue a redis command. If `cmd` is not NULL, it is sent as the
   first token, followed by `args`. Is `is_append` is set,
   redisAppendCommandArgv is called instead of redisCommandArgv. */
static void _hiredis_send_raw_array(INTERNAL_FUNCTION_PARAMETERS, hiredis_t* client, char* cmd, zval* args, int argc, int is_append) {
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
        #if PHP_MAJOR_VERSION >= 7
            convert_to_string_ex(&args[i]);
        #else
            zval* _zp = &args[i];
            convert_to_string_ex(&_zp);
        #endif
        string_args[j] = Z_STRVAL_P(&args[i]);
        string_lens[j] = Z_STRLEN_P(&args[i]);
    }

    // Send/queue command
    if (is_append) {
        if (REDIS_OK != redisAppendCommandArgv(client->ctx, num_strings, (const char**)string_args, string_lens)) {
            PHP_HIREDIS_SET_ERROR(client);
            RETVAL_FALSE;
        } else {
            RETVAL_TRUE;
        }
    } else {
        if (zv = (zval*)redisCommandArgv(client->ctx, num_strings, (const char**)string_args, string_lens)) {
            PHP_HIREDIS_RETURN_OR_THROW_ZVAL(client, zv);
        } else {
            PHP_HIREDIS_SET_ERROR(client);
            RETVAL_FALSE;
        }
    }

    // Cleanup
    efree(string_args);
    efree(string_lens);
}

/* Prepare args for _hiredis_send_raw_array */
static void _hiredis_send_raw(INTERNAL_FUNCTION_PARAMETERS, int is_array, int is_append) {
    hiredis_t* client;
    zval* zobj;
    int argc;
    zval* args;
    #if PHP_MAJOR_VERSION >= 7
        zval* varargs;
    #else
        zval*** varargs;
    #endif

    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O+", &zobj, hiredis_ce, &varargs, &argc) == FAILURE) {
        RETURN_FALSE;
    }
    if ((is_array && argc != 1) || (!is_array && argc < 1)) {
        WRONG_PARAM_COUNT;
    }

    client = Z_HIREDIS_P(zobj);
    PHP_HIREDIS_ENSURE_CTX(client);

    #if PHP_MAJOR_VERSION >= 7
        if (is_array) {
            _hiredis_convert_zval_to_array_of_zvals(varargs, &args, &argc);
        } else {
            args = varargs;
        }
    #else
        if (is_array) {
            _hiredis_convert_zval_to_array_of_zvals(**varargs, &args, &argc);
        } else {
            int i;
            args = (zval*)safe_emalloc(argc, sizeof(zval), 0);
            for (i = 0; i < argc; i++) memcpy(&args[i], *(varargs[i]), sizeof(zval));
        }
    #endif

    _hiredis_send_raw_array(INTERNAL_FUNCTION_PARAM_PASSTHRU, client, NULL, args, argc, is_append);

    #if PHP_MAJOR_VERSION >= 7
        if (is_array) efree(args);
    #else
        efree(args);
    #endif
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
    client->throw_exceptions = 0;
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
    char* cmd = NULL;
    size_t func_len;
    zval* func_args;
    int func_argc;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sa", &func, &func_len, &func_args) == FAILURE) {
        RETURN_FALSE;
    }

    client = Z_HIREDIS_P(getThis());
    PHP_HIREDIS_ENSURE_CTX(client);

    #if PHP_MAJOR_VERSION >= 7
        cmd = zend_hash_str_find_ptr(&func_cmd_map, func, func_len);
    #else
        // TODO This entire block feels wrong
        void** data;
        if (SUCCESS == zend_hash_find(&func_cmd_map, func, func_len, (void**)&data)) {
            cmd = (char*)data;
        }
    #endif
    if (cmd) {
        _hiredis_convert_zval_to_array_of_zvals(func_args, &func_args, &func_argc);
        _hiredis_send_raw_array(INTERNAL_FUNCTION_PARAM_PASSTHRU, client, cmd, func_args, func_argc, 0);
        efree(func_args);
    } else {
        #if PHP_MAJOR_VERSION >= 7
            zend_throw_error(NULL, "Call to undefined method Hiredis::%s()", func);
        #else
            zend_throw_exception_ex(NULL, 0 TSRMLS_CC, "Call to undefined method Hiredis::%s()", func);
        #endif
    }
}
/* }}} */

/* {{{ proto bool hiredis_connect(string ip, int port [, float timeout_s])
   Connect to a server via TCP. */
PHP_FUNCTION(hiredis_connect) {
    zval* zobj;
    hiredis_t* client;
    char* ip;
    size_t ip_len;
    long port;
    double timeout_s = -1;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Osl|d", &zobj, hiredis_ce, &ip, &ip_len, &port, &timeout_s) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    _hiredis_conn_deinit(client);
    if (timeout_s >= 0) {
        client->timeout_us = (long)(timeout_s * 1000 * 1000);
    }
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

/* {{{ proto bool hiredis_set_throw_exceptions(bool on_off)
   Set whether to throw exceptions on ERR replies from server. */
PHP_FUNCTION(hiredis_set_throw_exceptions) {
    zval* zobj;
    hiredis_t* client;
    zend_bool on_off;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "Ob", &zobj, hiredis_ce, &on_off) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    client->throw_exceptions = on_off ? 1 : 0;
    RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool hiredis_get_throw_exceptions()
   Get whether throw_exceptions is enabled. */
PHP_FUNCTION(hiredis_get_throw_exceptions) {
    zval* zobj;
    hiredis_t* client;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    RETURN_BOOL(client->throw_exceptions);
}
/* }}} */

/* {{{ proto mixed hiredis_send_raw(string args...)
   Send command and return result. */
PHP_FUNCTION(hiredis_send_raw) {
    _hiredis_send_raw(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 0);
}
/* }}} */

/* {{{ proto string hiredis_send_raw_array(array args)
   Send command and return result. */
PHP_FUNCTION(hiredis_send_raw_array) {
    _hiredis_send_raw(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 0);
}
/* }}} */

/* {{{ proto string hiredis_append_command(string args...)
   Append command to pipeline. */
PHP_FUNCTION(hiredis_append_command) {
    _hiredis_send_raw(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0, 1);
}
/* }}} */

/* {{{ proto string hiredis_append_command_array(array args)
   Append command to pipeline. */
PHP_FUNCTION(hiredis_append_command_array) {
    _hiredis_send_raw(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1, 1);
}
/* }}} */

/* {{{ proto string hiredis_get_reply()
   Get reply from pipeline. */
PHP_FUNCTION(hiredis_get_reply) {
    zval* zobj;
    hiredis_t* client;
    zval* reply = NULL;
    if (zend_parse_method_parameters(ZEND_NUM_ARGS(), getThis(), "O", &zobj, hiredis_ce) == FAILURE) {
        RETURN_FALSE;
    }
    client = Z_HIREDIS_P(zobj);
    PHP_HIREDIS_ENSURE_CTX(client);
    if (REDIS_OK != redisGetReply(client->ctx, (void*)&reply)) {
        PHP_HIREDIS_SET_ERROR(client);
        RETURN_FALSE;
    } else if (reply) {
        PHP_HIREDIS_RETURN_OR_THROW_ZVAL(client, reply);
    } else {
        PHP_HIREDIS_SET_ERROR_EX(client, REDIS_ERR, "redisGetReply returned NULL");
        RETURN_FALSE;
    }
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
        #if PHP_MAJOR_VERSION >= 7
            RETURN_STRING(client->errstr);
        #else
            RETURN_STRING(client->errstr, 1);
        #endif
    }
    RETURN_NULL();
}
/* }}} */

/* {{{ hiredis_methods */
zend_function_entry hiredis_methods[] = {
    PHP_ME(Hiredis, __construct, arginfo_hiredis_none, ZEND_ACC_CTOR | ZEND_ACC_PUBLIC)
    PHP_ME(Hiredis, __destruct,  arginfo_hiredis_none, ZEND_ACC_PUBLIC)
    PHP_ME(Hiredis, __call,      arginfo_hiredis_call, ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(connect,              hiredis_connect,              arginfo_hiredis_connect,              ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(connectUnix,          hiredis_connect_unix,         arginfo_hiredis_connect_unix,         ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setTimeout,           hiredis_set_timeout,          arginfo_hiredis_set_timeout,          ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getTimeout,           hiredis_get_timeout,          arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setKeepAliveInterval, hiredis_set_keep_alive_int,   arginfo_hiredis_set_keep_alive_int,   ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getKeepAliveInterval, hiredis_get_keep_alive_int,   arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setMaxReadBuf,        hiredis_set_max_read_buf,     arginfo_hiredis_set_max_read_buf,     ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getMaxReadBuf,        hiredis_get_max_read_buf,     arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(setThrowExceptions,   hiredis_set_throw_exceptions, arginfo_hiredis_set_throw_exceptions, ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getThrowExceptions,   hiredis_get_throw_exceptions, arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(sendRaw,              hiredis_send_raw,             arginfo_hiredis_send_raw,             ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(sendRawArray,         hiredis_send_raw_array,       arginfo_hiredis_send_raw_array,       ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(appendRaw,            hiredis_append_command,       arginfo_hiredis_send_raw,             ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(appendRawArray,       hiredis_append_command_array, arginfo_hiredis_send_raw_array,       ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getReply,             hiredis_get_reply,            arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
    PHP_ME_MAPPING(getLastError,         hiredis_get_last_error,       arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
#ifdef HAVE_HIREDIS_RECONNECT
    PHP_ME_MAPPING(reconnect,            hiredis_reconnect,            arginfo_hiredis_none,                 ZEND_ACC_PUBLIC)
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

    // Register Hiredis class
    INIT_CLASS_ENTRY(ce, "Hiredis", hiredis_methods);
    #if PHP_MAJOR_VERSION >= 7
        hiredis_ce = zend_register_internal_class(&ce);
        hiredis_ce->create_object = hiredis_obj_new;
    #else
        ce.create_object = hiredis_obj_new;
        hiredis_ce = zend_register_internal_class(&ce TSRMLS_CC);
    #endif
    memcpy(&hiredis_obj_handlers, zend_get_std_object_handlers(), sizeof(hiredis_obj_handlers));
    #if PHP_MAJOR_VERSION >= 7
        hiredis_obj_handlers.offset = XtOffsetOf(hiredis_t, std);
        hiredis_obj_handlers.free_obj = hiredis_obj_free;
    #endif

    // Register HiredisException class
    INIT_CLASS_ENTRY(ce, "HiredisException", NULL);
    #if PHP_MAJOR_VERSION >= 7
        hiredis_exception_ce = zend_register_internal_class_ex(&ce, zend_ce_exception);
    #else
        hiredis_exception_ce = zend_register_internal_class_ex(&ce, zend_exception_get_default(TSRMLS_C), NULL TSRMLS_CC);
    #endif

    // Init func_cmd_map for __call
    zend_hash_init(&func_cmd_map, 0, NULL, NULL, 1);
    #if PHP_MAJOR_VERSION >= 7
        #define PHP_HIREDIS_MAP_FUNC_CMD(pfunc, pcmd) \
            zend_hash_str_add_ptr(&func_cmd_map, (pfunc), sizeof((pfunc))-1, (pcmd));
    #else
        #define PHP_HIREDIS_MAP_FUNC_CMD(pfunc, pcmd) \
            zend_hash_add(&func_cmd_map, (pfunc), sizeof((pfunc))-1, (void**)(pcmd), sizeof(char*), NULL);
    #endif
    PHP_HIREDIS_MAP_FUNC_CMD("append", "APPEND");
    PHP_HIREDIS_MAP_FUNC_CMD("auth", "AUTH");
    PHP_HIREDIS_MAP_FUNC_CMD("bgrewriteaof", "BGREWRITEAOF");
    PHP_HIREDIS_MAP_FUNC_CMD("bgsave", "BGSAVE");
    PHP_HIREDIS_MAP_FUNC_CMD("bitcount", "BITCOUNT");
    PHP_HIREDIS_MAP_FUNC_CMD("bitop", "BITOP");
    PHP_HIREDIS_MAP_FUNC_CMD("bitpos", "BITPOS");
    PHP_HIREDIS_MAP_FUNC_CMD("blpop", "BLPOP");
    PHP_HIREDIS_MAP_FUNC_CMD("brpop", "BRPOP");
    PHP_HIREDIS_MAP_FUNC_CMD("brpoplpush", "BRPOPLPUSH");
    PHP_HIREDIS_MAP_FUNC_CMD("client", "CLIENT");
    PHP_HIREDIS_MAP_FUNC_CMD("cluster", "CLUSTER");
    PHP_HIREDIS_MAP_FUNC_CMD("command", "COMMAND");
    PHP_HIREDIS_MAP_FUNC_CMD("config", "CONFIG");
    PHP_HIREDIS_MAP_FUNC_CMD("dbsize", "DBSIZE");
    PHP_HIREDIS_MAP_FUNC_CMD("debug", "DEBUG");
    PHP_HIREDIS_MAP_FUNC_CMD("decr", "DECR");
    PHP_HIREDIS_MAP_FUNC_CMD("decrby", "DECRBY");
    PHP_HIREDIS_MAP_FUNC_CMD("del", "DEL");
    PHP_HIREDIS_MAP_FUNC_CMD("discard", "DISCARD");
    PHP_HIREDIS_MAP_FUNC_CMD("dump", "DUMP");
    PHP_HIREDIS_MAP_FUNC_CMD("echo", "ECHO");
    PHP_HIREDIS_MAP_FUNC_CMD("eval", "EVAL");
    PHP_HIREDIS_MAP_FUNC_CMD("evalsha", "EVALSHA");
    PHP_HIREDIS_MAP_FUNC_CMD("exec", "EXEC");
    PHP_HIREDIS_MAP_FUNC_CMD("exists", "EXISTS");
    PHP_HIREDIS_MAP_FUNC_CMD("expire", "EXPIRE");
    PHP_HIREDIS_MAP_FUNC_CMD("expireat", "EXPIREAT");
    PHP_HIREDIS_MAP_FUNC_CMD("flushall", "FLUSHALL");
    PHP_HIREDIS_MAP_FUNC_CMD("flushdb", "FLUSHDB");
    PHP_HIREDIS_MAP_FUNC_CMD("geoadd", "GEOADD");
    PHP_HIREDIS_MAP_FUNC_CMD("geodist", "GEODIST");
    PHP_HIREDIS_MAP_FUNC_CMD("geohash", "GEOHASH");
    PHP_HIREDIS_MAP_FUNC_CMD("geopos", "GEOPOS");
    PHP_HIREDIS_MAP_FUNC_CMD("georadius", "GEORADIUS");
    PHP_HIREDIS_MAP_FUNC_CMD("georadiusbymember", "GEORADIUSBYMEMBER");
    PHP_HIREDIS_MAP_FUNC_CMD("get", "GET");
    PHP_HIREDIS_MAP_FUNC_CMD("getbit", "GETBIT");
    PHP_HIREDIS_MAP_FUNC_CMD("getrange", "GETRANGE");
    PHP_HIREDIS_MAP_FUNC_CMD("getset", "GETSET");
    PHP_HIREDIS_MAP_FUNC_CMD("hdel", "HDEL");
    PHP_HIREDIS_MAP_FUNC_CMD("hexists", "HEXISTS");
    PHP_HIREDIS_MAP_FUNC_CMD("hget", "HGET");
    PHP_HIREDIS_MAP_FUNC_CMD("hgetall", "HGETALL");
    PHP_HIREDIS_MAP_FUNC_CMD("hincrby", "HINCRBY");
    PHP_HIREDIS_MAP_FUNC_CMD("hincrbyfloat", "HINCRBYFLOAT");
    PHP_HIREDIS_MAP_FUNC_CMD("hkeys", "HKEYS");
    PHP_HIREDIS_MAP_FUNC_CMD("hlen", "HLEN");
    PHP_HIREDIS_MAP_FUNC_CMD("hmget", "HMGET");
    PHP_HIREDIS_MAP_FUNC_CMD("hmset", "HMSET");
    PHP_HIREDIS_MAP_FUNC_CMD("hscan", "HSCAN");
    PHP_HIREDIS_MAP_FUNC_CMD("hset", "HSET");
    PHP_HIREDIS_MAP_FUNC_CMD("hsetnx", "HSETNX");
    PHP_HIREDIS_MAP_FUNC_CMD("hstrlen", "HSTRLEN");
    PHP_HIREDIS_MAP_FUNC_CMD("hvals", "HVALS");
    PHP_HIREDIS_MAP_FUNC_CMD("incr", "INCR");
    PHP_HIREDIS_MAP_FUNC_CMD("incrby", "INCRBY");
    PHP_HIREDIS_MAP_FUNC_CMD("incrbyfloat", "INCRBYFLOAT");
    PHP_HIREDIS_MAP_FUNC_CMD("info", "INFO");
    PHP_HIREDIS_MAP_FUNC_CMD("keys", "KEYS");
    PHP_HIREDIS_MAP_FUNC_CMD("lastsave", "LASTSAVE");
    PHP_HIREDIS_MAP_FUNC_CMD("lindex", "LINDEX");
    PHP_HIREDIS_MAP_FUNC_CMD("linsert", "LINSERT");
    PHP_HIREDIS_MAP_FUNC_CMD("llen", "LLEN");
    PHP_HIREDIS_MAP_FUNC_CMD("lpop", "LPOP");
    PHP_HIREDIS_MAP_FUNC_CMD("lpush", "LPUSH");
    PHP_HIREDIS_MAP_FUNC_CMD("lpushx", "LPUSHX");
    PHP_HIREDIS_MAP_FUNC_CMD("lrange", "LRANGE");
    PHP_HIREDIS_MAP_FUNC_CMD("lrem", "LREM");
    PHP_HIREDIS_MAP_FUNC_CMD("lset", "LSET");
    PHP_HIREDIS_MAP_FUNC_CMD("ltrim", "LTRIM");
    PHP_HIREDIS_MAP_FUNC_CMD("mget", "MGET");
    PHP_HIREDIS_MAP_FUNC_CMD("migrate", "MIGRATE");
    PHP_HIREDIS_MAP_FUNC_CMD("monitor", "MONITOR");
    PHP_HIREDIS_MAP_FUNC_CMD("move", "MOVE");
    PHP_HIREDIS_MAP_FUNC_CMD("mset", "MSET");
    PHP_HIREDIS_MAP_FUNC_CMD("msetnx", "MSETNX");
    PHP_HIREDIS_MAP_FUNC_CMD("multi", "MULTI");
    PHP_HIREDIS_MAP_FUNC_CMD("object", "OBJECT");
    PHP_HIREDIS_MAP_FUNC_CMD("persist", "PERSIST");
    PHP_HIREDIS_MAP_FUNC_CMD("pexpire", "PEXPIRE");
    PHP_HIREDIS_MAP_FUNC_CMD("pexpireat", "PEXPIREAT");
    PHP_HIREDIS_MAP_FUNC_CMD("pfadd", "PFADD");
    PHP_HIREDIS_MAP_FUNC_CMD("pfcount", "PFCOUNT");
    PHP_HIREDIS_MAP_FUNC_CMD("pfmerge", "PFMERGE");
    PHP_HIREDIS_MAP_FUNC_CMD("ping", "PING");
    PHP_HIREDIS_MAP_FUNC_CMD("psetex", "PSETEX");
    PHP_HIREDIS_MAP_FUNC_CMD("psubscribe", "PSUBSCRIBE");
    PHP_HIREDIS_MAP_FUNC_CMD("pttl", "PTTL");
    PHP_HIREDIS_MAP_FUNC_CMD("publish", "PUBLISH");
    PHP_HIREDIS_MAP_FUNC_CMD("pubsub", "PUBSUB");
    PHP_HIREDIS_MAP_FUNC_CMD("punsubscribe", "PUNSUBSCRIBE");
    PHP_HIREDIS_MAP_FUNC_CMD("quit", "QUIT");
    PHP_HIREDIS_MAP_FUNC_CMD("randomkey", "RANDOMKEY");
    PHP_HIREDIS_MAP_FUNC_CMD("rename", "RENAME");
    PHP_HIREDIS_MAP_FUNC_CMD("renamenx", "RENAMENX");
    PHP_HIREDIS_MAP_FUNC_CMD("restore", "RESTORE");
    PHP_HIREDIS_MAP_FUNC_CMD("role", "ROLE");
    PHP_HIREDIS_MAP_FUNC_CMD("rpop", "RPOP");
    PHP_HIREDIS_MAP_FUNC_CMD("rpoplpush", "RPOPLPUSH");
    PHP_HIREDIS_MAP_FUNC_CMD("rpush", "RPUSH");
    PHP_HIREDIS_MAP_FUNC_CMD("rpushx", "RPUSHX");
    PHP_HIREDIS_MAP_FUNC_CMD("sadd", "SADD");
    PHP_HIREDIS_MAP_FUNC_CMD("save", "SAVE");
    PHP_HIREDIS_MAP_FUNC_CMD("scan", "SCAN");
    PHP_HIREDIS_MAP_FUNC_CMD("scard", "SCARD");
    PHP_HIREDIS_MAP_FUNC_CMD("script", "SCRIPT");
    PHP_HIREDIS_MAP_FUNC_CMD("sdiff", "SDIFF");
    PHP_HIREDIS_MAP_FUNC_CMD("sdiffstore", "SDIFFSTORE");
    PHP_HIREDIS_MAP_FUNC_CMD("select", "SELECT");
    PHP_HIREDIS_MAP_FUNC_CMD("set", "SET");
    PHP_HIREDIS_MAP_FUNC_CMD("setbit", "SETBIT");
    PHP_HIREDIS_MAP_FUNC_CMD("setex", "SETEX");
    PHP_HIREDIS_MAP_FUNC_CMD("setnx", "SETNX");
    PHP_HIREDIS_MAP_FUNC_CMD("setrange", "SETRANGE");
    PHP_HIREDIS_MAP_FUNC_CMD("shutdown", "SHUTDOWN");
    PHP_HIREDIS_MAP_FUNC_CMD("sinter", "SINTER");
    PHP_HIREDIS_MAP_FUNC_CMD("sinterstore", "SINTERSTORE");
    PHP_HIREDIS_MAP_FUNC_CMD("sismember", "SISMEMBER");
    PHP_HIREDIS_MAP_FUNC_CMD("slaveof", "SLAVEOF");
    PHP_HIREDIS_MAP_FUNC_CMD("slowlog", "SLOWLOG");
    PHP_HIREDIS_MAP_FUNC_CMD("smembers", "SMEMBERS");
    PHP_HIREDIS_MAP_FUNC_CMD("smove", "SMOVE");
    PHP_HIREDIS_MAP_FUNC_CMD("sort", "SORT");
    PHP_HIREDIS_MAP_FUNC_CMD("spop", "SPOP");
    PHP_HIREDIS_MAP_FUNC_CMD("srandmember", "SRANDMEMBER");
    PHP_HIREDIS_MAP_FUNC_CMD("srem", "SREM");
    PHP_HIREDIS_MAP_FUNC_CMD("sscan", "SSCAN");
    PHP_HIREDIS_MAP_FUNC_CMD("strlen", "STRLEN");
    PHP_HIREDIS_MAP_FUNC_CMD("subscribe", "SUBSCRIBE");
    PHP_HIREDIS_MAP_FUNC_CMD("sunion", "SUNION");
    PHP_HIREDIS_MAP_FUNC_CMD("sunionstore", "SUNIONSTORE");
    PHP_HIREDIS_MAP_FUNC_CMD("sync", "SYNC");
    PHP_HIREDIS_MAP_FUNC_CMD("time", "TIME");
    PHP_HIREDIS_MAP_FUNC_CMD("ttl", "TTL");
    PHP_HIREDIS_MAP_FUNC_CMD("type", "TYPE");
    PHP_HIREDIS_MAP_FUNC_CMD("unsubscribe", "UNSUBSCRIBE");
    PHP_HIREDIS_MAP_FUNC_CMD("unwatch", "UNWATCH");
    PHP_HIREDIS_MAP_FUNC_CMD("wait", "WAIT");
    PHP_HIREDIS_MAP_FUNC_CMD("watch", "WATCH");
    PHP_HIREDIS_MAP_FUNC_CMD("zadd", "ZADD");
    PHP_HIREDIS_MAP_FUNC_CMD("zcard", "ZCARD");
    PHP_HIREDIS_MAP_FUNC_CMD("zcount", "ZCOUNT");
    PHP_HIREDIS_MAP_FUNC_CMD("zincrby", "ZINCRBY");
    PHP_HIREDIS_MAP_FUNC_CMD("zinterstore", "ZINTERSTORE");
    PHP_HIREDIS_MAP_FUNC_CMD("zlexcount", "ZLEXCOUNT");
    PHP_HIREDIS_MAP_FUNC_CMD("zrange", "ZRANGE");
    PHP_HIREDIS_MAP_FUNC_CMD("zrangebylex", "ZRANGEBYLEX");
    PHP_HIREDIS_MAP_FUNC_CMD("zrangebyscore", "ZRANGEBYSCORE");
    PHP_HIREDIS_MAP_FUNC_CMD("zrank", "ZRANK");
    PHP_HIREDIS_MAP_FUNC_CMD("zrem", "ZREM");
    PHP_HIREDIS_MAP_FUNC_CMD("zremrangebylex", "ZREMRANGEBYLEX");
    PHP_HIREDIS_MAP_FUNC_CMD("zremrangebyrank", "ZREMRANGEBYRANK");
    PHP_HIREDIS_MAP_FUNC_CMD("zremrangebyscore", "ZREMRANGEBYSCORE");
    PHP_HIREDIS_MAP_FUNC_CMD("zrevrange", "ZREVRANGE");
    PHP_HIREDIS_MAP_FUNC_CMD("zrevrangebylex", "ZREVRANGEBYLEX");
    PHP_HIREDIS_MAP_FUNC_CMD("zrevrangebyscore", "ZREVRANGEBYSCORE");
    PHP_HIREDIS_MAP_FUNC_CMD("zrevrank", "ZREVRANK");
    PHP_HIREDIS_MAP_FUNC_CMD("zscan", "ZSCAN");
    PHP_HIREDIS_MAP_FUNC_CMD("zscore", "ZSCORE");
    PHP_HIREDIS_MAP_FUNC_CMD("zunionstore", "ZUNIONSTORE");
    #undef PHP_HIREDIS_MAP_FUNC_CMD
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(hiredis) {
    zend_hash_destroy(&func_cmd_map);
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
