/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrey Hristov <andrey@php.net>                             |
  |          Ulf Wendel <uw@php.net>                                     |
  +----------------------------------------------------------------------+
*/

#include "php.h"
#include "mysqlnd.h"
#include "mysqlnd_vio.h"
#include "mysqlnd_protocol_frame_codec.h"
#include "mysqlnd_wireprotocol.h"
#include "mysqlnd_connection.h"
#include "mysqlnd_ps.h"
#include "mysqlnd_plugin.h"
#include "mysqlnd_priv.h"
#include "mysqlnd_statistics.h"
#include "mysqlnd_debug.h"
#include "mysqlnd_reverse_api.h"
#include "mysqlnd_ext_plugin.h"

static zend_bool mysqlnd_library_initted = FALSE;

static struct st_mysqlnd_plugin_core mysqlnd_plugin_core =
{
	{
		MYSQLND_PLUGIN_API_VERSION,
		"mysqlnd",
		MYSQLND_VERSION_ID,
		PHP_MYSQLND_VERSION,
		"PHP License 3.01",
		"Andrey Hristov <andrey@php.net>,  Ulf Wendel <uw@php.net>, Georg Richter <georg@php.net>",
		{
			NULL, /* will be filled later */
			mysqlnd_stats_values_names,
		},
		{
			NULL /* plugin shutdown */
		}
	}
};


/* {{{ mysqlnd_library_end */
PHPAPI void mysqlnd_library_end(void)
{
	if (mysqlnd_library_initted == TRUE) {
		mysqlnd_plugin_subsystem_end();
		mysqlnd_stats_end(mysqlnd_global_stats, 1);
		mysqlnd_global_stats = NULL;
		mysqlnd_library_initted = FALSE;
		mysqlnd_reverse_api_end();
	}
}
/* }}} */


/* {{{ mysqlnd_library_init */
PHPAPI void mysqlnd_library_init(void)
{
	if (mysqlnd_library_initted == FALSE) {
		mysqlnd_library_initted = TRUE;
		mysqlnd_conn_set_methods(&MYSQLND_CLASS_METHOD_TABLE_NAME(mysqlnd_conn));
		mysqlnd_conn_data_set_methods(&MYSQLND_CLASS_METHOD_TABLE_NAME(mysqlnd_conn_data));
		_mysqlnd_init_ps_subsystem();
		/* Should be calloc, as mnd_calloc will reference LOCK_access*/
		mysqlnd_stats_init(&mysqlnd_global_stats, STAT_LAST, 1);
		mysqlnd_plugin_subsystem_init();
		{
			mysqlnd_plugin_core.plugin_header.plugin_stats.values = mysqlnd_global_stats;
			mysqlnd_plugin_register_ex((struct st_mysqlnd_plugin_header *) &mysqlnd_plugin_core);
		}
#if defined(MYSQLND_DBG_ENABLED) && MYSQLND_DBG_ENABLED == 1
		mysqlnd_example_plugin_register();
#endif
		mysqlnd_debug_trace_plugin_register();
		mysqlnd_register_builtin_authentication_plugins();

		mysqlnd_reverse_api_init();
	}
}
/* }}} */


/* {{{ mysqlnd_object_factory::get_connection */
static MYSQLND *
MYSQLND_METHOD(mysqlnd_object_factory, get_connection)(MYSQLND_CLASS_METHODS_TYPE(mysqlnd_object_factory) *factory, const zend_bool persistent)
{
	size_t alloc_size_ret = sizeof(MYSQLND) + mysqlnd_plugin_count() * sizeof(void *);
	size_t alloc_size_ret_data = sizeof(MYSQLND_CONN_DATA) + mysqlnd_plugin_count() * sizeof(void *);
	MYSQLND * new_object;
	MYSQLND_CONN_DATA * data;

	DBG_ENTER("mysqlnd_driver::get_connection");
	DBG_INF_FMT("persistent=%u", persistent);
	new_object = mnd_pecalloc(1, alloc_size_ret, persistent);
	if (!new_object) {
		DBG_RETURN(NULL);
	}
	new_object->data = mnd_pecalloc(1, alloc_size_ret_data, persistent);
	if (!new_object->data) {
		mnd_pefree(new_object, persistent);
		DBG_RETURN(NULL);
	}
	new_object->persistent = persistent;
	new_object->m = mysqlnd_conn_get_methods();
	data = new_object->data;

	if (FAIL == mysqlnd_error_info_init(&data->error_info_impl, persistent)) {
		new_object->m->dtor(new_object);
		DBG_RETURN(NULL);
	}
	data->error_info = &data->error_info_impl;

	data->options = &(data->options_impl);

	mysqlnd_upsert_status_init(&data->upsert_status_impl);
	data->upsert_status = &(data->upsert_status_impl);
	UPSERT_STATUS_SET_AFFECTED_ROWS_TO_ERROR(data->upsert_status);

	data->persistent = persistent;
	data->m = mysqlnd_conn_data_get_methods();
	data->object_factory = *factory;

	mysqlnd_connection_state_init(&data->state);

	data->m->get_reference(data);

	mysqlnd_stats_init(&data->stats, STAT_LAST, persistent);

	data->protocol_frame_codec = mysqlnd_pfc_init(persistent, factory, data->stats, data->error_info);
	data->vio = mysqlnd_vio_init(persistent, factory, data->stats, data->error_info);
	data->payload_decoder_factory = mysqlnd_protocol_payload_decoder_factory_init(data, persistent);
	data->run_command = mysqlnd_command_factory_get();

	if (!data->protocol_frame_codec || !data->vio || !data->payload_decoder_factory || !data->run_command) {
		new_object->m->dtor(new_object);
		DBG_RETURN(NULL);
	}

	DBG_RETURN(new_object);
}
/* }}} */


/* {{{ mysqlnd_object_factory::clone_connection_object */
static MYSQLND *
MYSQLND_METHOD(mysqlnd_object_factory, clone_connection_object)(MYSQLND * to_be_cloned)
{
	size_t alloc_size_ret = sizeof(MYSQLND) + mysqlnd_plugin_count() * sizeof(void *);
	MYSQLND * new_object;

	DBG_ENTER("mysqlnd_driver::clone_connection_object");
	DBG_INF_FMT("persistent=%u", to_be_cloned->persistent);
	if (!to_be_cloned || !to_be_cloned->data) {
		DBG_RETURN(NULL);
	}
	new_object = mnd_pecalloc(1, alloc_size_ret, to_be_cloned->persistent);
	if (!new_object) {
		DBG_RETURN(NULL);
	}
	new_object->persistent = to_be_cloned->persistent;
	new_object->m = to_be_cloned->m;

	new_object->data = to_be_cloned->data->m->get_reference(to_be_cloned->data);
	if (!new_object->data) {
		new_object->m->dtor(new_object);
		new_object = NULL;
	}
	DBG_RETURN(new_object);
}
/* }}} */


/* {{{ mysqlnd_object_factory::get_prepared_statement */
static MYSQLND_STMT *
MYSQLND_METHOD(mysqlnd_object_factory, get_prepared_statement)(MYSQLND_CONN_DATA * const conn)
{
	size_t alloc_size = sizeof(MYSQLND_STMT) + mysqlnd_plugin_count() * sizeof(void *);
	MYSQLND_STMT * ret = mnd_ecalloc(1, alloc_size);
	MYSQLND_STMT_DATA * stmt = NULL;

	DBG_ENTER("mysqlnd_object_factory::get_prepared_statement");
	do {
		if (!ret) {
			break;
		}
		ret->m = mysqlnd_stmt_get_methods();

		stmt = ret->data = mnd_ecalloc(1, sizeof(MYSQLND_STMT_DATA));
		DBG_INF_FMT("stmt=%p", stmt);
		if (!stmt) {
			break;
		}

		if (FAIL == mysqlnd_error_info_init(&stmt->error_info_impl, 0)) {
			break;		
		}
		stmt->error_info = &stmt->error_info_impl;

		mysqlnd_upsert_status_init(&stmt->upsert_status_impl);
		stmt->upsert_status = &(stmt->upsert_status_impl);
		stmt->state = MYSQLND_STMT_INITTED;
		stmt->execute_cmd_buffer.length = 4096;
		stmt->execute_cmd_buffer.buffer = mnd_emalloc(stmt->execute_cmd_buffer.length);
		if (!stmt->execute_cmd_buffer.buffer) {
			break;
		}

		stmt->prefetch_rows = MYSQLND_DEFAULT_PREFETCH_ROWS;

		/*
		  Mark that we reference the connection, thus it won't be
		  be destructed till there is open statements. The last statement
		  or normal query result will close it then.
		*/
		stmt->conn = conn->m->get_reference(conn);

		DBG_RETURN(ret);
	} while (0);

	SET_OOM_ERROR(conn->error_info);
	if (ret) {
		ret->m->dtor(ret, TRUE);
		ret = NULL;
	}
	DBG_RETURN(NULL);
}
/* }}} */


/* {{{ mysqlnd_object_factory::get_pfc */
static MYSQLND_PFC *
MYSQLND_METHOD(mysqlnd_object_factory, get_pfc)(const zend_bool persistent, MYSQLND_STATS * stats, MYSQLND_ERROR_INFO * error_info)
{
	size_t pfc_alloc_size = sizeof(MYSQLND_PFC) + mysqlnd_plugin_count() * sizeof(void *);
	size_t pfc_data_alloc_size = sizeof(MYSQLND_PFC_DATA) + mysqlnd_plugin_count() * sizeof(void *);
	MYSQLND_PFC * pfc = mnd_pecalloc(1, pfc_alloc_size, persistent);
	MYSQLND_PFC_DATA * pfc_data = mnd_pecalloc(1, pfc_data_alloc_size, persistent);

	DBG_ENTER("mysqlnd_object_factory::get_pfc");
	DBG_INF_FMT("persistent=%u", persistent);
	if (pfc && pfc_data) {
		pfc->data = pfc_data;
		pfc->persistent = pfc->data->persistent = persistent;
		pfc->data->m = *mysqlnd_pfc_get_methods();

		if (PASS != pfc->data->m.init(pfc, stats, error_info)) {
			pfc->data->m.dtor(pfc, stats, error_info);
			pfc = NULL;
		}
	} else {
		if (pfc_data) {
			mnd_pefree(pfc_data, persistent);
			pfc_data = NULL;
		}
		if (pfc) {
			mnd_pefree(pfc, persistent);
			pfc = NULL;
		}
	}
	DBG_RETURN(pfc);
}
/* }}} */


/* {{{ mysqlnd_object_factory::get_vio */
static MYSQLND_VIO *
MYSQLND_METHOD(mysqlnd_object_factory, get_vio)(const zend_bool persistent, MYSQLND_STATS * stats, MYSQLND_ERROR_INFO * error_info)
{
	size_t vio_alloc_size = sizeof(MYSQLND_VIO) + mysqlnd_plugin_count() * sizeof(void *);
	size_t vio_data_alloc_size = sizeof(MYSQLND_VIO_DATA) + mysqlnd_plugin_count() * sizeof(void *);
	MYSQLND_VIO * vio = mnd_pecalloc(1, vio_alloc_size, persistent);
	MYSQLND_VIO_DATA * vio_data = mnd_pecalloc(1, vio_data_alloc_size, persistent);

	DBG_ENTER("mysqlnd_object_factory::get_vio");
	DBG_INF_FMT("persistent=%u", persistent);
	if (vio && vio_data) {
		vio->data = vio_data;
		vio->persistent = vio->data->persistent = persistent;
		vio->data->m = *mysqlnd_vio_get_methods();

		if (PASS != vio->data->m.init(vio, stats, error_info)) {
			vio->data->m.dtor(vio, stats, error_info);
			vio = NULL;
		}
	} else {
		if (vio_data) {
			mnd_pefree(vio_data, persistent);
			vio_data = NULL;
		}
		if (vio) {
			mnd_pefree(vio, persistent);
			vio = NULL;
		}
	}
	DBG_RETURN(vio);
}
/* }}} */


/* {{{ mysqlnd_object_factory::get_protocol_payload_decoder_factory */
static MYSQLND_PROTOCOL_PAYLOAD_DECODER_FACTORY *
MYSQLND_METHOD(mysqlnd_object_factory, get_protocol_payload_decoder_factory)(MYSQLND_CONN_DATA * conn, const zend_bool persistent)
{
	size_t alloc_size = sizeof(MYSQLND_PROTOCOL_PAYLOAD_DECODER_FACTORY) + mysqlnd_plugin_count() * sizeof(void *);
	MYSQLND_PROTOCOL_PAYLOAD_DECODER_FACTORY *ret = mnd_pecalloc(1, alloc_size, persistent);

	DBG_ENTER("mysqlnd_object_factory::get_protocol_payload_decoder_factory");
	DBG_INF_FMT("persistent=%u", persistent);
	if (ret) {
		ret->persistent = persistent;
		ret->conn = conn;
		ret->m = MYSQLND_CLASS_METHOD_TABLE_NAME(mysqlnd_protocol_payload_decoder_factory);
	}

	DBG_RETURN(ret);
}
/* }}} */


PHPAPI MYSQLND_CLASS_METHODS_START(mysqlnd_object_factory)
	MYSQLND_METHOD(mysqlnd_object_factory, get_connection),
	MYSQLND_METHOD(mysqlnd_object_factory, clone_connection_object),
	MYSQLND_METHOD(mysqlnd_object_factory, get_prepared_statement),
	MYSQLND_METHOD(mysqlnd_object_factory, get_pfc),
	MYSQLND_METHOD(mysqlnd_object_factory, get_vio),
	MYSQLND_METHOD(mysqlnd_object_factory, get_protocol_payload_decoder_factory)
MYSQLND_CLASS_METHODS_END;

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
