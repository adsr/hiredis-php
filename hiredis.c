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
static HashTable hiredis_cmd_map;

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

#if PHP_MAJOR_VERSION >= 7
    typedef size_t strlen_t;
    #define Z_HIREDIS_P(zv) hiredis_obj_fetch(Z_OBJ_P((zv)))
    #define MAKE_STD_ZVAL(zv) do { \
        zval _sz; \
        (zv) = &_sz; \
    } while (0)
#else
    typedef int strlen_t;
    #define Z_HIREDIS_P(zv) hiredis_obj_fetch(zv TSRMLS_CC)
    #define ZEND_HASH_FOREACH_VAL(_ht, _ppv) do { \
        HashPosition _pos; \
        for (zend_hash_internal_pointer_reset_ex((_ht), &_pos); \
            zend_hash_get_current_data_ex((_ht), (void **) &(_ppv), &_pos) == SUCCESS; \
            zend_hash_move_forward_ex((_ht), &_pos) ) {
    #define ZEND_HASH_FOREACH_END() } } while (0)
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
#if PHP_MAJOR_VERSION >= 7
    #define PHP_HIREDIS_RETVAL_DTOR 1
#else
    #define PHP_HIREDIS_RETVAL_DTOR 0
#endif
#define PHP_HIREDIS_RETURN_OR_THROW(client, zv) do { \
    if (Z_TYPE_P(zv) == IS_OBJECT && instanceof_function(Z_OBJCE_P(zv), hiredis_exception_ce)) { \
        PHP_HIREDIS_SET_ERROR_EX((client), REDIS_ERR, _hidreis_get_exception_message(zv)); \
        if (PHP_HIREDIS_RETVAL_DTOR) zval_dtor(zv); \
        RETVAL_FALSE; \
    } else { \
        RETVAL_ZVAL((zv), 1, PHP_HIREDIS_RETVAL_DTOR); \
    } \
    hiredis_replyobj_free((zv)); \
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
static zval* _hiredis_replyobj_nest(const redisReadTask* task, zval* z) {
    zval* rv = z;
    if (task->parent) {
        zval* parent;
        parent = (zval*)task->parent->obj;
        assert(Z_TYPE_P(parent) == IS_ARRAY);
        #if PHP_MAJOR_VERSION >= 7
            rv = zend_hash_index_update(Z_ARRVAL_P(parent), task->idx, z);
        #else
            add_index_zval(parent, task->idx, z);
        #endif
    }
    return rv;
}

/* redisReplyObjectFunctions: Get zval to operate on */
static zval* _hiredis_replyobj_get_zval(const redisReadTask* task, zval* stack_zval) {
    zval* rv;
    if (task->parent) {
        #if PHP_MAJOR_VERSION >= 7
            rv = stack_zval;
        #else
            MAKE_STD_ZVAL(rv);
        #endif
    } else {
        rv = (zval*)task->privdata;
    }
    return rv;
}

/* redisReplyObjectFunctions: Create string */
static void* hiredis_replyobj_create_string(const redisReadTask* task, char* str, size_t len) {
    zval sz;
    zval* z = _hiredis_replyobj_get_zval(task, &sz);
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
    return (void*)_hiredis_replyobj_nest(task, z);
}

/* redisReplyObjectFunctions: Create array */
static void* hiredis_replyobj_create_array(const redisReadTask* task, int len) {
    zval sz;
    zval* z = _hiredis_replyobj_get_zval(task, &sz);
    array_init_size(z, len);
    return (void*)_hiredis_replyobj_nest(task, z);
}

/* redisReplyObjectFunctions: Create int */
static void* hiredis_replyobj_create_integer(const redisReadTask* task, long long i) {
    zval sz;
    zval* z = _hiredis_replyobj_get_zval(task, &sz);
    ZVAL_LONG(z, i);
    return (void*)_hiredis_replyobj_nest(task, z);
}

/* redisReplyObjectFunctions: Create nil */
static void* hiredis_replyobj_create_nil(const redisReadTask* task) {
    zval sz;
    zval* z = _hiredis_replyobj_get_zval(task, &sz);
    ZVAL_NULL(z);
    return (void*)_hiredis_replyobj_nest(task, z);
}

/* redisReplyObjectFunctions: Free object */
static void hiredis_replyobj_free(void* obj) {
    #if PHP_MAJOR_VERSION < 7
        zval* z = (zval*)obj;
        zval* ze;
        if (Z_TYPE_P(z) == IS_ARRAY) {
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(z), ze) {
                hiredis_replyobj_free((void*)ze);
            } ZEND_HASH_FOREACH_END();
            zend_hash_destroy(Z_ARRVAL_P(z));
        }
        zval_dtor(z);
        //FREE_ZVAL(z);
    #endif
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
    #if PHP_MAJOR_VERSION >= 7
        zend_string** string_zstrs;
        string_zstrs = (zend_string**)safe_emalloc(num_strings, sizeof(zend_string*), 0);
    #endif
    j = 0;
    if (cmd) {
        string_args[j] = cmd;
        string_lens[j] = strlen(cmd);
        #if PHP_MAJOR_VERSION >= 7
            string_zstrs[j] = NULL;
        #endif
        j++;
    }
    for (i = 0; i < argc; i++, j++) {
        zval* _zp = &args[i];
        #if PHP_MAJOR_VERSION >= 7
            string_zstrs[j] = NULL;
        #endif
        if (
            #if PHP_MAJOR_VERSION >= 7
                Z_TYPE_P(_zp) == IS_TRUE || Z_TYPE_P(_zp) == IS_FALSE
            #else
                Z_TYPE_P(_zp) == IS_BOOL
            #endif
        ) {
            string_args[j] = zend_is_true(_zp) ? "1" : "0";
            string_lens[j] = 1;
        } else {
            if (Z_TYPE_P(_zp) != IS_STRING) {
                #if PHP_MAJOR_VERSION >= 7
                    convert_to_string_ex(_zp);
                    string_zstrs[j] = Z_STR_P(_zp);
                #else
                    convert_to_string_ex(&_zp);
                #endif
            }
            string_args[j] = Z_STRVAL_P(_zp);
            string_lens[j] = Z_STRLEN_P(_zp);
        }
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
        redisReplyReaderSetPrivdata(client->ctx->reader, (void*)return_value);
        if (zv = (zval*)redisCommandArgv(client->ctx, num_strings, (const char**)string_args, string_lens)) {
            assert(zv == return_value);
            PHP_HIREDIS_RETURN_OR_THROW(client, return_value);
        } else {
            PHP_HIREDIS_SET_ERROR(client);
            RETVAL_FALSE;
        }
    }

    // Cleanup
    #if PHP_MAJOR_VERSION >= 7
        for (i = 0; i < num_strings; i++) {
            if (string_zstrs[i]) {
                zend_string_release(string_zstrs[i]);
            }
        }
        efree(string_zstrs);
    #endif
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
    char* ofunc;
    char* func;
    strlen_t func_len;
    char* cmd;
    zval* func_args;
    int func_argc;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sa", &ofunc, &func_len, &func_args) == FAILURE) {
        RETURN_FALSE;
    }

    client = Z_HIREDIS_P(getThis());
    PHP_HIREDIS_ENSURE_CTX(client);

    func = estrndup(ofunc, func_len);
    php_strtoupper(func, func_len);
    cmd = NULL;
    #if PHP_MAJOR_VERSION >= 7
        if (zend_hash_str_exists(&hiredis_cmd_map, func, func_len)) {
            cmd = func;
        }
    #else
        if (zend_hash_exists(&hiredis_cmd_map, func, func_len)) {
            cmd = func;
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
    efree(func);
}
/* }}} */

/* {{{ proto bool hiredis_connect(string ip, int port [, float timeout_s])
   Connect to a server via TCP. */
PHP_FUNCTION(hiredis_connect) {
    zval* zobj;
    hiredis_t* client;
    char* ip;
    strlen_t ip_len;
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
    strlen_t path_len;
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
    redisReplyReaderSetPrivdata(client->ctx->reader, (void*)return_value);
    if (REDIS_OK != redisGetReply(client->ctx, (void**)&reply)) {
        PHP_HIREDIS_SET_ERROR(client);
        RETURN_FALSE;
    } else if (reply) {
        assert(reply == return_value);
        PHP_HIREDIS_RETURN_OR_THROW(client, return_value);
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

    // Init hiredis_cmd_map for __call
    zend_hash_init(&hiredis_cmd_map, 0, NULL, NULL, 1);
    #if PHP_MAJOR_VERSION >= 7
        #define PHP_HIREDIS_MAP_CMD(pcmd) \
            zend_hash_str_add_empty_element(&hiredis_cmd_map, (pcmd), sizeof((pcmd))-1);
    #else
        #define PHP_HIREDIS_MAP_CMD(pcmd) \
            zend_hash_add_empty_element(&hiredis_cmd_map, (pcmd), sizeof((pcmd))-1);
    #endif
    PHP_HIREDIS_MAP_CMD("APPEND");
    PHP_HIREDIS_MAP_CMD("AUTH");
    PHP_HIREDIS_MAP_CMD("BGREWRITEAOF");
    PHP_HIREDIS_MAP_CMD("BGSAVE");
    PHP_HIREDIS_MAP_CMD("BITCOUNT");
    PHP_HIREDIS_MAP_CMD("BITOP");
    PHP_HIREDIS_MAP_CMD("BITPOS");
    PHP_HIREDIS_MAP_CMD("BLPOP");
    PHP_HIREDIS_MAP_CMD("BRPOP");
    PHP_HIREDIS_MAP_CMD("BRPOPLPUSH");
    PHP_HIREDIS_MAP_CMD("CLIENT");
    PHP_HIREDIS_MAP_CMD("CLUSTER");
    PHP_HIREDIS_MAP_CMD("COMMAND");
    PHP_HIREDIS_MAP_CMD("CONFIG");
    PHP_HIREDIS_MAP_CMD("DBSIZE");
    PHP_HIREDIS_MAP_CMD("DEBUG");
    PHP_HIREDIS_MAP_CMD("DECR");
    PHP_HIREDIS_MAP_CMD("DECRBY");
    PHP_HIREDIS_MAP_CMD("DEL");
    PHP_HIREDIS_MAP_CMD("DISCARD");
    PHP_HIREDIS_MAP_CMD("DUMP");
    PHP_HIREDIS_MAP_CMD("ECHO");
    PHP_HIREDIS_MAP_CMD("EVAL");
    PHP_HIREDIS_MAP_CMD("EVALSHA");
    PHP_HIREDIS_MAP_CMD("EXEC");
    PHP_HIREDIS_MAP_CMD("EXISTS");
    PHP_HIREDIS_MAP_CMD("EXPIRE");
    PHP_HIREDIS_MAP_CMD("EXPIREAT");
    PHP_HIREDIS_MAP_CMD("FLUSHALL");
    PHP_HIREDIS_MAP_CMD("FLUSHDB");
    PHP_HIREDIS_MAP_CMD("GEOADD");
    PHP_HIREDIS_MAP_CMD("GEODIST");
    PHP_HIREDIS_MAP_CMD("GEOHASH");
    PHP_HIREDIS_MAP_CMD("GEOPOS");
    PHP_HIREDIS_MAP_CMD("GEORADIUS");
    PHP_HIREDIS_MAP_CMD("GEORADIUSBYMEMBER");
    PHP_HIREDIS_MAP_CMD("GET");
    PHP_HIREDIS_MAP_CMD("GETBIT");
    PHP_HIREDIS_MAP_CMD("GETRANGE");
    PHP_HIREDIS_MAP_CMD("GETSET");
    PHP_HIREDIS_MAP_CMD("HDEL");
    PHP_HIREDIS_MAP_CMD("HEXISTS");
    PHP_HIREDIS_MAP_CMD("HGET");
    PHP_HIREDIS_MAP_CMD("HGETALL");
    PHP_HIREDIS_MAP_CMD("HINCRBY");
    PHP_HIREDIS_MAP_CMD("HINCRBYFLOAT");
    PHP_HIREDIS_MAP_CMD("HKEYS");
    PHP_HIREDIS_MAP_CMD("HLEN");
    PHP_HIREDIS_MAP_CMD("HMGET");
    PHP_HIREDIS_MAP_CMD("HMSET");
    PHP_HIREDIS_MAP_CMD("HSCAN");
    PHP_HIREDIS_MAP_CMD("HSET");
    PHP_HIREDIS_MAP_CMD("HSETNX");
    PHP_HIREDIS_MAP_CMD("HSTRLEN");
    PHP_HIREDIS_MAP_CMD("HVALS");
    PHP_HIREDIS_MAP_CMD("INCR");
    PHP_HIREDIS_MAP_CMD("INCRBY");
    PHP_HIREDIS_MAP_CMD("INCRBYFLOAT");
    PHP_HIREDIS_MAP_CMD("INFO");
    PHP_HIREDIS_MAP_CMD("KEYS");
    PHP_HIREDIS_MAP_CMD("LASTSAVE");
    PHP_HIREDIS_MAP_CMD("LINDEX");
    PHP_HIREDIS_MAP_CMD("LINSERT");
    PHP_HIREDIS_MAP_CMD("LLEN");
    PHP_HIREDIS_MAP_CMD("LPOP");
    PHP_HIREDIS_MAP_CMD("LPUSH");
    PHP_HIREDIS_MAP_CMD("LPUSHX");
    PHP_HIREDIS_MAP_CMD("LRANGE");
    PHP_HIREDIS_MAP_CMD("LREM");
    PHP_HIREDIS_MAP_CMD("LSET");
    PHP_HIREDIS_MAP_CMD("LTRIM");
    PHP_HIREDIS_MAP_CMD("MGET");
    PHP_HIREDIS_MAP_CMD("MIGRATE");
    PHP_HIREDIS_MAP_CMD("MONITOR");
    PHP_HIREDIS_MAP_CMD("MOVE");
    PHP_HIREDIS_MAP_CMD("MSET");
    PHP_HIREDIS_MAP_CMD("MSETNX");
    PHP_HIREDIS_MAP_CMD("MULTI");
    PHP_HIREDIS_MAP_CMD("OBJECT");
    PHP_HIREDIS_MAP_CMD("PERSIST");
    PHP_HIREDIS_MAP_CMD("PEXPIRE");
    PHP_HIREDIS_MAP_CMD("PEXPIREAT");
    PHP_HIREDIS_MAP_CMD("PFADD");
    PHP_HIREDIS_MAP_CMD("PFCOUNT");
    PHP_HIREDIS_MAP_CMD("PFMERGE");
    PHP_HIREDIS_MAP_CMD("PING");
    PHP_HIREDIS_MAP_CMD("PSETEX");
    PHP_HIREDIS_MAP_CMD("PSUBSCRIBE");
    PHP_HIREDIS_MAP_CMD("PTTL");
    PHP_HIREDIS_MAP_CMD("PUBLISH");
    PHP_HIREDIS_MAP_CMD("PUBSUB");
    PHP_HIREDIS_MAP_CMD("PUNSUBSCRIBE");
    PHP_HIREDIS_MAP_CMD("QUIT");
    PHP_HIREDIS_MAP_CMD("RANDOMKEY");
    PHP_HIREDIS_MAP_CMD("RENAME");
    PHP_HIREDIS_MAP_CMD("RENAMENX");
    PHP_HIREDIS_MAP_CMD("RESTORE");
    PHP_HIREDIS_MAP_CMD("ROLE");
    PHP_HIREDIS_MAP_CMD("RPOP");
    PHP_HIREDIS_MAP_CMD("RPOPLPUSH");
    PHP_HIREDIS_MAP_CMD("RPUSH");
    PHP_HIREDIS_MAP_CMD("RPUSHX");
    PHP_HIREDIS_MAP_CMD("SADD");
    PHP_HIREDIS_MAP_CMD("SAVE");
    PHP_HIREDIS_MAP_CMD("SCAN");
    PHP_HIREDIS_MAP_CMD("SCARD");
    PHP_HIREDIS_MAP_CMD("SCRIPT");
    PHP_HIREDIS_MAP_CMD("SDIFF");
    PHP_HIREDIS_MAP_CMD("SDIFFSTORE");
    PHP_HIREDIS_MAP_CMD("SELECT");
    PHP_HIREDIS_MAP_CMD("SET");
    PHP_HIREDIS_MAP_CMD("SETBIT");
    PHP_HIREDIS_MAP_CMD("SETEX");
    PHP_HIREDIS_MAP_CMD("SETNX");
    PHP_HIREDIS_MAP_CMD("SETRANGE");
    PHP_HIREDIS_MAP_CMD("SHUTDOWN");
    PHP_HIREDIS_MAP_CMD("SINTER");
    PHP_HIREDIS_MAP_CMD("SINTERSTORE");
    PHP_HIREDIS_MAP_CMD("SISMEMBER");
    PHP_HIREDIS_MAP_CMD("SLAVEOF");
    PHP_HIREDIS_MAP_CMD("SLOWLOG");
    PHP_HIREDIS_MAP_CMD("SMEMBERS");
    PHP_HIREDIS_MAP_CMD("SMOVE");
    PHP_HIREDIS_MAP_CMD("SORT");
    PHP_HIREDIS_MAP_CMD("SPOP");
    PHP_HIREDIS_MAP_CMD("SRANDMEMBER");
    PHP_HIREDIS_MAP_CMD("SREM");
    PHP_HIREDIS_MAP_CMD("SSCAN");
    PHP_HIREDIS_MAP_CMD("STRLEN");
    PHP_HIREDIS_MAP_CMD("SUBSCRIBE");
    PHP_HIREDIS_MAP_CMD("SUNION");
    PHP_HIREDIS_MAP_CMD("SUNIONSTORE");
    PHP_HIREDIS_MAP_CMD("SYNC");
    PHP_HIREDIS_MAP_CMD("TIME");
    PHP_HIREDIS_MAP_CMD("TTL");
    PHP_HIREDIS_MAP_CMD("TYPE");
    PHP_HIREDIS_MAP_CMD("UNSUBSCRIBE");
    PHP_HIREDIS_MAP_CMD("UNWATCH");
    PHP_HIREDIS_MAP_CMD("WAIT");
    PHP_HIREDIS_MAP_CMD("WATCH");
    PHP_HIREDIS_MAP_CMD("ZADD");
    PHP_HIREDIS_MAP_CMD("ZCARD");
    PHP_HIREDIS_MAP_CMD("ZCOUNT");
    PHP_HIREDIS_MAP_CMD("ZINCRBY");
    PHP_HIREDIS_MAP_CMD("ZINTERSTORE");
    PHP_HIREDIS_MAP_CMD("ZLEXCOUNT");
    PHP_HIREDIS_MAP_CMD("ZRANGE");
    PHP_HIREDIS_MAP_CMD("ZRANGEBYLEX");
    PHP_HIREDIS_MAP_CMD("ZRANGEBYSCORE");
    PHP_HIREDIS_MAP_CMD("ZRANK");
    PHP_HIREDIS_MAP_CMD("ZREM");
    PHP_HIREDIS_MAP_CMD("ZREMRANGEBYLEX");
    PHP_HIREDIS_MAP_CMD("ZREMRANGEBYRANK");
    PHP_HIREDIS_MAP_CMD("ZREMRANGEBYSCORE");
    PHP_HIREDIS_MAP_CMD("ZREVRANGE");
    PHP_HIREDIS_MAP_CMD("ZREVRANGEBYLEX");
    PHP_HIREDIS_MAP_CMD("ZREVRANGEBYSCORE");
    PHP_HIREDIS_MAP_CMD("ZREVRANK");
    PHP_HIREDIS_MAP_CMD("ZSCAN");
    PHP_HIREDIS_MAP_CMD("ZSCORE");
    PHP_HIREDIS_MAP_CMD("ZUNIONSTORE");
    #undef PHP_HIREDIS_MAP_CMD
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(hiredis) {
    zend_hash_destroy(&hiredis_cmd_map);
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
