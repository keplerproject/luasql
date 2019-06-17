/*
** LuaSQL, Firebird driver
** Authors: Scott Morgan
** ls_firebird.c
*/

#include <ibase.h>	/* The Firebird API*/
#include <time.h>	/* For managing time values */
#include <stdlib.h>
#include <string.h>

/* Lua API */
#include <lua.h>
#include <lauxlib.h>

#include "luasql.h"

/* backwards compat for Lua 5.1 */
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM == 501
static void *luaL_testudata (lua_State *L, int i, const char *tname)
{
	void *p = lua_touserdata(L, i);
	luaL_checkstack(L, 2, "not enough stack slots");
	if (p == NULL || !lua_getmetatable(L, i)) {
		return NULL;
	} else {
		int res = 0;
		luaL_getmetatable(L, tname);
		res = lua_rawequal(L, -1, -2);
		lua_pop(L, 2);
		if (!res) {
			p = NULL;
		}
	}
	return p;
}
#endif

#define LUASQL_ENVIRONMENT_FIREBIRD "Firebird environment"
#define LUASQL_CONNECTION_FIREBIRD "Firebird connection"
#define LUASQL_STATEMENT_FIREBIRD "Firebird statement"
#define LUASQL_CURSOR_FIREBIRD "Firebird cursor"

typedef struct {
	short           closed;
	int             lock;             /* lock count for open connections */
	ISC_STATUS      status_vector[20];/* for error results */
} env_data;

typedef struct {
	/* general */
	short           closed;
	int             lock;             /* lock count for open cursors */
	env_data        *env;             /* the DB enviroment this is in */
	int             autocommit;       /* should each statement be commited */
	/* implimentation */
	isc_db_handle   db;               /* the database handle */
	char            dpb_buffer[1024]; /* holds the database paramet buffer */
	short           dpb_length;       /* the used amount of the dpb */
	isc_tr_handle   transaction;      /* the transaction handle */
	/* config */
	unsigned short  dialect;          /* dialect of SQL used */
} conn_data;

typedef struct {
	short           closed;
	int             lock;             /* lock count for open statements */
	env_data        *env;             /* the DB enviroment this is in */
	conn_data       *conn;            /* the DB connection this cursor is from */
	/* implimentation */
	XSQLDA          *in_sqlda;        /* the parameter data array */
	isc_stmt_handle handle;           /* the statement handle */
	int             type;             /* the statment's type (SELECT, UPDATE,
	                                     etc...) */
	unsigned char   hidden;           /* statement was used interally i.e. from a
	                                     direct con:execute */
} stmt_data;

typedef struct {
	short           closed;
	env_data        *env;             /* the DB enviroment this is in */
	stmt_data       *stmt;            /* the DB statment this cursor is from */
	XSQLDA          *out_sqlda;       /* the cursor data array */
} cur_data;

/* How many fields to pre-alloc to the cursor */
#define CURSOR_PREALLOC 10

/* Macro to ease code reading */
#define CHECK_DB_ERROR( X ) ( (X)[0] == 1 && (X)[1] )

/* Use the new interpret API if available */
#undef FB_INTERPRET
#if FB_API_VER >= 20
  #define FB_INTERPRET(BUF, LEN, VECTOR) fb_interpret(BUF, LEN, VECTOR)
#else
  #define FB_INTERPRET(BUF, LEN, VECTOR) isc_interpret(BUF, VECTOR)
#endif

#if LUA_VERSION_NUM>=503
#define luasql_pushinteger lua_pushinteger
#else
#define luasql_pushinteger lua_pushnumber
#endif

/* MSVC still doesn't support C99 properly until 2015 */
#if defined(_MSC_VER) && _MSC_VER<1900
#pragma warning(disable:4996)	/* and complains if you try to work around it */
#define snprintf _snprintf
#endif

LUASQL_API int luaopen_luasql_firebird (lua_State *L);

/*
** Sets a simple custom error status with given message
*/
static void custom_fb_error(ISC_STATUS *pvector, const char *msg)
{
	*pvector++ = 1;          /* isc_arg_gds */
	*pvector++ = 335544382L; /* isc_random */
	*pvector++ = 2;          /* isc_arg_string */
	*pvector++ = (ISC_STATUS)msg;
	*pvector++ = 0;          /* isc_arg_end */
}

/*
** Returns a standard database error message
*/
static int return_db_error(lua_State *L, const ISC_STATUS *pvector)
{
	char errmsg[512];

	lua_pushnil(L);
	FB_INTERPRET(errmsg, 512, &pvector);
	lua_pushstring(L, errmsg);
	while(FB_INTERPRET(errmsg, 512, &pvector)) {
		lua_pushstring(L, "\n * ");
		lua_pushstring(L, errmsg);
		lua_concat(L, 3);
	}

	return 2;
}

/*
** Registers a given C object in the registry to avoid GC
*/
void registerobj(lua_State *L, int index, void *obj)
{
	lua_pushvalue(L, index);
	lua_pushlightuserdata(L, obj);
	lua_pushvalue(L, -2);
	lua_settable(L, LUA_REGISTRYINDEX);
	lua_pop(L, 1);
}

/*
** Unregisters a given C object from the registry
*/
void unregisterobj(lua_State *L, void *obj)
{
	lua_pushlightuserdata(L, obj);
	lua_pushnil(L);
	lua_settable(L, LUA_REGISTRYINDEX);
}

/*
** Allocates and initialises an XSQLDA struct
*/
static XSQLDA *malloc_xsqlda(ISC_SHORT len)
{
	XSQLDA *res = (XSQLDA *)malloc(XSQLDA_LENGTH(len));

	memset(res, 0, XSQLDA_LENGTH(len));
	res->version = SQLDA_VERSION1;
	res->sqln = len;

	return res;
}

static void *malloc_zero(size_t len)
{
	void *res = malloc(len);
	memset(res, 0, len);
	return res;
}

/*
** Allocate memory for XSQLDA data
*/
static void malloc_sqlda_vars(XSQLDA *sqlda)
{
	int i;
	XSQLVAR *var;

	/* prep the result set ready to handle the data */
	for (i=0, var = sqlda->sqlvar; i < sqlda->sqld; i++, var++) {
		switch(var->sqltype & ~1) {
		case SQL_VARYING:
			var->sqldata = (char *)malloc_zero(sizeof(char)*var->sqllen + 2);
			break;
		case SQL_TEXT:
			var->sqldata = (char *)malloc_zero(sizeof(char)*var->sqllen);
			break;
		case SQL_SHORT:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_SHORT));
			break;
		case SQL_LONG:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_LONG));
			break;
		case SQL_INT64:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_INT64));
			break;
		case SQL_FLOAT:
			var->sqldata = (char *)malloc_zero(sizeof(float));
			break;
		case SQL_DOUBLE:
			var->sqldata = (char *)malloc_zero(sizeof(double));
			break;
		case SQL_TYPE_TIME:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_TIME));
			break;
		case SQL_TYPE_DATE:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_DATE));
			break;
		case SQL_TIMESTAMP:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_TIMESTAMP));
			break;
		case SQL_BLOB:
			var->sqldata = (char *)malloc_zero(sizeof(ISC_QUAD));
			break;
			/* TODO : add extra data type handles here */
		}

		if (var->sqltype & 1) {
			/* allocate variable to hold NULL status */
			var->sqlind = (short *)malloc(sizeof(short));
		} else {
			var->sqlind = NULL;
		}
	}
}

/*
** Frees memory allocated to XSQLDA data
*/
static void free_sqlda_vars(XSQLDA *sqlda)
{
	int i;
	XSQLVAR *var;

	if(sqlda != NULL) {
		for (i=0, var = sqlda->sqlvar; i < sqlda->sqln; i++, var++) {
			free(var->sqldata);
			free(var->sqlind);
		}
	}
}

/*
** Frees all XSQLDA data
*/
static void free_xsqlda(XSQLDA *sqlda)
{
	free_sqlda_vars(sqlda);
	free(sqlda);
}

/*
** Free's up the memory alloc'd to the statement data
*/
static void free_stmt(stmt_data *stmt)
{
	/* free the input DA */
	free_xsqlda(stmt->in_sqlda);
}

static int stmt_shut(lua_State *L, stmt_data *stmt)
{
	isc_dsql_free_statement(stmt->env->status_vector, &stmt->handle, DSQL_drop);
	if ( CHECK_DB_ERROR(stmt->env->status_vector) ) {
		return return_db_error(L, stmt->env->status_vector);
	}

	free_stmt(stmt);

	/* remove statement from lock count and check if connection can be unregistered */
	stmt->closed = 1;
	if(--stmt->conn->lock == 0) {
		unregisterobj(L, stmt->conn);
	}

	return 0;
}

/*
** Free's up the memory alloc'd to the cursor data
*/
static void free_cur(cur_data *cur)
{
	/* free the output DA */
	free_xsqlda(cur->out_sqlda);
}

/*
** Shuts down a cursor
*/
static int cur_shut(lua_State *L, cur_data *cur)
{
	isc_dsql_free_statement(cur->env->status_vector, &cur->stmt->handle,
	                        DSQL_close);
	if ( CHECK_DB_ERROR(cur->env->status_vector) ) {
		return return_db_error(L, cur->env->status_vector);
	}

	/* free the cursor data */
	free_cur(cur);

	/* remove cursor from lock count and check if statment can be unregistered */
	cur->closed = 1;
	if(--cur->stmt->lock == 0) {
		unregisterobj(L, cur->stmt);

		/* hidden statement, needs closing now */
		if(cur->stmt->hidden) {
			return stmt_shut(L, cur->stmt);
		}
	}

	return 0;
}

/*
** Check for valid environment.
*/
static env_data *getenvironment (lua_State *L, int i)
{
	env_data *env = (env_data *)luaL_checkudata (L, i, LUASQL_ENVIRONMENT_FIREBIRD);
	luaL_argcheck (L, env != NULL, i, "environment expected");
	luaL_argcheck (L, !env->closed, i, "environment is closed");
	return env;
}

/*
** Check for valid connection.
*/
static conn_data *getconnection (lua_State *L, int i)
{
	conn_data *conn = (conn_data *)luaL_checkudata (L, i,
	                  LUASQL_CONNECTION_FIREBIRD);
	luaL_argcheck (L, conn != NULL, i, "connection expected");
	luaL_argcheck (L, !conn->closed, i, "connection is closed");
	return conn;
}

/*
** Check for valid statement.
*/
static stmt_data *getstatement (lua_State *L, int i)
{
	stmt_data *stmt = (stmt_data *)luaL_checkudata (L, i,
	                  LUASQL_STATEMENT_FIREBIRD);
	luaL_argcheck (L, stmt != NULL, i, "statement expected");
	luaL_argcheck (L, !stmt->closed, i, "statement is closed");
	return stmt;
}

/*
** Check for valid cursor.
*/
static cur_data *getcursor (lua_State *L, int i)
{
	cur_data *cur = (cur_data *)luaL_checkudata (L, i, LUASQL_CURSOR_FIREBIRD);
	luaL_argcheck (L, cur != NULL, i, "cursor expected");
	luaL_argcheck (L, !cur->closed, i, "cursor is closed");
	return cur;
}

/*
** Dumps the list of item's types into a new table
*/
static int dump_xsqlda_types(lua_State *L, XSQLDA *sqlda)
{
	int i;
	XSQLVAR *var;

	lua_newtable(L);

	for (i=1, var = sqlda->sqlvar; i <= sqlda->sqld; i++, var++) {
		lua_pushnumber(L, i);
		switch(var->sqltype & ~1) {
		case SQL_VARYING:
		case SQL_TEXT:
		case SQL_TYPE_TIME:
		case SQL_TYPE_DATE:
		case SQL_TIMESTAMP:
		case SQL_BLOB:
			lua_pushstring(L, "string");
			break;
		case SQL_SHORT:
		case SQL_LONG:
		case SQL_INT64:
#if LUA_VERSION_NUM>=503
			lua_pushstring(L, "integer");
			break;
#endif
		case SQL_FLOAT:
		case SQL_DOUBLE:
			lua_pushstring(L, "number");
			break;
		default:
			lua_pushstring(L, "unknown");
			break;
		}
		lua_settable(L, -3);
	}

	return 1;
}

/*
** Returns the statement type
*/
static int get_statement_type(stmt_data *stmt)
{
	int length, type;
	char type_item[] = { isc_info_sql_stmt_type };
	char res_buffer[88], *pres;

	pres = res_buffer;

	isc_dsql_sql_info(  stmt->env->status_vector, &stmt->handle,
	                    sizeof(type_item), type_item,
	                    sizeof(res_buffer), res_buffer );
	if (stmt->env->status_vector[0] == 1 && stmt->env->status_vector[1] > 0) {
		return -1;
	}

	/* check the type of the statement */
	if (*pres == isc_info_sql_stmt_type) {
		pres++;
		length = isc_vax_integer(pres, 2);
		pres += 2;
		type = isc_vax_integer(pres, length);
		pres += length;
	} else {
		return -2;     /* should have had the isc_info_sql_stmt_type info */
	}

	return type;
}

/*
** Return the number of rows affected by last operation
*/
static int count_rows_affected(env_data *env, cur_data *cur)
{
	int length, type, res=0;
	int del_count = 0, ins_count = 0, upd_count = 0, sel_count = 0;
	char type_item[] = { isc_info_sql_stmt_type, isc_info_sql_records };
	char res_buffer[88], *pres;

	pres = res_buffer;

	isc_dsql_sql_info( env->status_vector, &cur->stmt->handle,
	                   sizeof(type_item), type_item,
	                   sizeof(res_buffer), res_buffer );
	if (cur->env->status_vector[0] == 1 && cur->env->status_vector[1] > 0) {
		return -1;
	}

	/* check the type of the statement */
	if (*pres == isc_info_sql_stmt_type) {
		pres++;
		length = isc_vax_integer(pres, 2);
		pres += 2;
		type = isc_vax_integer(pres, length);
		pres += length;
	} else {
		return -2;     /* should have had the isc_info_sql_stmt_type info */
	}

	if(type > 4) {
		return 0;     /* not a SELECT, INSERT, UPDATE or DELETE SQL statement */
	}

	if (*pres == isc_info_sql_records) {
		pres++;
		length = isc_vax_integer(pres, 2); /* normally 29 bytes */
		pres += 2;

		while(*pres != 1) {
			switch(*pres) {
			case isc_info_req_select_count:
				pres++;
				length = isc_vax_integer(pres, 2);
				pres += 2;
				sel_count = isc_vax_integer(pres, length);
				pres += length;
				break;
			case isc_info_req_insert_count:
				pres++;
				length = isc_vax_integer(pres, 2);
				pres += 2;
				ins_count = isc_vax_integer(pres, length);
				pres += length;
				break;
			case isc_info_req_update_count:
				pres++;
				length = isc_vax_integer(pres, 2);
				pres += 2;
				upd_count = isc_vax_integer(pres, length);
				pres += length;
				break;
			case isc_info_req_delete_count:
				pres++;
				length = isc_vax_integer(pres, 2);
				pres += 2;
				del_count = isc_vax_integer(pres, length);
				pres += length;
				break;
			default:
				pres++;
				break;
			}
		}
	} else {
		return -3;
	}

	switch(type) {
	case isc_info_sql_stmt_select:
		res = sel_count;
		break;
	case isc_info_sql_stmt_delete:
		res = del_count;
		break;
	case isc_info_sql_stmt_update:
		res = upd_count;
		break;
	case isc_info_sql_stmt_insert:
		res = ins_count;
		break;
	}
	return res;
}

static void fill_param(XSQLVAR *var, ISC_SHORT type, ISC_SCHAR *data,
                       ISC_SHORT len)
{
	var->sqltype = type;
	*var->sqlind = 0;
	var->sqllen = len;

	if((type & ~1) == SQL_TEXT) {
		--var->sqllen;
	}

	if(var->sqldata != NULL) {
		free(var->sqldata);
	}
	if(var->sqldata = (ISC_SCHAR *)malloc(len)) {
		memcpy(var->sqldata, data, len);
	}
}

static void write_blob(stmt_data *stmt, ISC_QUAD *blob_id, ISC_SCHAR *data,
                       size_t len)
{
	isc_blob_handle blob_handle = NULL;
	memset(blob_id, 0, sizeof(ISC_QUAD));

	isc_create_blob2(
		stmt->env->status_vector,
		&stmt->conn->db, &stmt->conn->transaction,
		&blob_handle, blob_id,
		0, NULL);
	if(CHECK_DB_ERROR(stmt->env->status_vector)) {
		return;
	}

	while(len > 10000) {
		isc_put_segment(stmt->env->status_vector, &blob_handle, 10000, data);
		if(CHECK_DB_ERROR(stmt->env->status_vector)) {
			return;
		}

		len -= 10000;
		data += 10000;
	}
	isc_put_segment(stmt->env->status_vector, &blob_handle, len, data);
	if(CHECK_DB_ERROR(stmt->env->status_vector)) {
		return;
	}

	isc_close_blob(stmt->env->status_vector, &blob_handle);
}

static void set_param(lua_State *L, stmt_data *stmt, XSQLVAR *var)
{
	const char *str;
	ISC_QUAD blob_id;
	ISC_INT64 inum;
	double fnum;

	if(var->sqlind == NULL) {
		var->sqlind = (ISC_SHORT *)malloc(sizeof(ISC_SHORT));
	}

	if(lua_isnil(L, -1)) {
		// nil -> NULL
		*var->sqlind = -1;
	} else {
		switch(var->sqltype & ~1) {
		case SQL_VARYING:
		case SQL_BLOB:
		case SQL_TEXT:
			str = lua_tostring(L, -1);
			if(strlen(str) > 0x7FF0) {
				/* need to use BLOB for >32K chars */
				write_blob(stmt, &blob_id, (ISC_SCHAR *)str, strlen(str));
				fill_param(var, SQL_BLOB+1, (ISC_SCHAR *)&blob_id, sizeof(ISC_QUAD));
			} else {
				/* plain text */
				fill_param(var, SQL_TEXT+1, (ISC_SCHAR *)str, strlen(str)+1);
			}
			break;

		case SQL_INT64:
		case SQL_LONG:
		case SQL_SHORT:
			inum = (ISC_INT64)lua_tonumber(L, -1);
			fill_param(var, SQL_INT64+1, (ISC_SCHAR *)&inum,
				        sizeof(ISC_INT64));
			break;

		case SQL_DOUBLE:
		case SQL_D_FLOAT:
		case SQL_FLOAT:
			fnum = (double)lua_tonumber(L, -1);
			fill_param(var, SQL_DOUBLE+1, (ISC_SCHAR *)&fnum,
				        sizeof(double));
			break;

		case SQL_TIMESTAMP:
		case SQL_TYPE_TIME:
		case SQL_TYPE_DATE:
			switch(lua_type(L, -1)) {
			case LUA_TNUMBER: {
				/* os.time type value passed */
				time_t t_time = (time_t)lua_tointeger(L,-1);
				struct tm *tm_time = localtime(&t_time);
				ISC_TIMESTAMP isc_ts;
				isc_encode_timestamp(tm_time, &isc_ts);

				fill_param(var, SQL_TIMESTAMP+1, (ISC_SCHAR *)&isc_ts,
					        sizeof(ISC_TIMESTAMP));
			}	break;

			case LUA_TSTRING: {
				/* date/time string passed */
				str = lua_tostring(L, -1);
				fill_param(var, SQL_TEXT+1, (ISC_SCHAR *)str, strlen(str)+1);
			}	break;

			default: {
				/* unknown pass empty string, which should error out */
				str = lua_tostring(L, -1);
				fill_param(var, SQL_TEXT+1, (ISC_SCHAR *)"", 1);
			}	break;
			}
			break;
		}

		if(!var->sqldata) {
			custom_fb_error(stmt->conn->env->status_vector, "Problem allocating SQL param memory");
			return;
		}
	}
}

static void parse_params(lua_State *L, stmt_data *stmt, int params)
{
	XSQLVAR *var;
	int i, ltop;

	if(lua_type(L, params) == LUA_TTABLE) {
		for(i=0; i<stmt->in_sqlda->sqln; ++i) {
			lua_pushnumber(L, i+1);
			lua_gettable(L, params);

			var = &stmt->in_sqlda->sqlvar[i];
			set_param(L, stmt, var);

			lua_pop(L,1);  /* param value */
		}
	} else {
		ltop = lua_gettop(L);
		for(i=0; i<stmt->in_sqlda->sqln; ++i, ++params) {
			if(params > ltop) {
				lua_pushnil(L);
			} else {
				lua_pushvalue(L, params);
			}

			var = &stmt->in_sqlda->sqlvar[i];
			set_param(L, stmt, var);

			lua_pop(L,1);  /* param value */
		}
	}
}

/*
** Prepares a SQL statement.
** Lua input:
**   SQL statement
**  [parmeter table]
** Returns
**   statement object ready for setting parameters
**   nil and error message otherwise.
*/
static int conn_prepare (lua_State *L)
{
	conn_data *conn = getconnection(L,1);
	const char *statement = luaL_checkstring(L, 2);

	stmt_data *user_stmt;

	stmt_data stmt;

	memset(&stmt, 0, sizeof(stmt_data));

	stmt.closed = 0;
	stmt.env = conn->env;
	stmt.conn = conn;

	/* create a statement to handle the query */
	isc_dsql_allocate_statement(conn->env->status_vector, &conn->db, &stmt.handle);
	if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
		return return_db_error(L, conn->env->status_vector);
	}

	/* process the SQL ready to run the query */
	isc_dsql_prepare(conn->env->status_vector, &conn->transaction, &stmt.handle, 0,
	                 (char *)statement, conn->dialect, NULL);
	if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
		free_stmt(&stmt);
		return return_db_error(L, conn->env->status_vector);
	}

	/* what type of SQL statement is it? */
	stmt.type = get_statement_type(&stmt);
	if(stmt.type < 0) {
		free_stmt(&stmt);
		return return_db_error(L, stmt.env->status_vector);
	}

	/* an unsupported SQL statement (something like COMMIT) */
	switch(stmt.type) {
	case isc_info_sql_stmt_select:
	case isc_info_sql_stmt_insert:
	case isc_info_sql_stmt_update:
	case isc_info_sql_stmt_delete:
	case isc_info_sql_stmt_ddl:
	case isc_info_sql_stmt_exec_procedure:
		break;
	default:
		free_stmt(&stmt);
		return luasql_faildirect(L, "unsupported SQL statement");
	}

	/* bind the input parameters */
	stmt.in_sqlda = malloc_xsqlda(1);
	isc_dsql_describe_bind(conn->env->status_vector, &stmt.handle, 1,
	                       stmt.in_sqlda);
	if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
		free_stmt(&stmt);
		return return_db_error(L, conn->env->status_vector);
	}
	/* resize the parameter set if needed */
	if (stmt.in_sqlda->sqld > stmt.in_sqlda->sqln) {
		ISC_SHORT n = stmt.in_sqlda->sqld;
		free_xsqlda(stmt.in_sqlda);
		stmt.in_sqlda = malloc_xsqlda(n);
		isc_dsql_describe_bind(conn->env->status_vector, &stmt.handle, 1,
		                       stmt.in_sqlda);
		if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
			free_stmt(&stmt);
			return return_db_error(L, conn->env->status_vector);
		}
	}
	malloc_sqlda_vars(stmt.in_sqlda);

	/* are there any input params */
	if(stmt.in_sqlda->sqld > 0) {
		parse_params(L, &stmt, 3);
		if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
			free_stmt(&stmt);
			return return_db_error(L, conn->env->status_vector);
		}
	}

	/* copy the statement into a new lua userdata object */
	user_stmt = (stmt_data *)lua_newuserdata(L, sizeof(stmt_data));
	luasql_setmeta (L, LUASQL_STATEMENT_FIREBIRD);
	memcpy((void *)user_stmt, (void *)&stmt, sizeof(stmt_data));

	/* add statement to the lock count */
	registerobj(L, 1, conn);
	++conn->lock;

	return 1;
}

static int raw_execute (lua_State *L, int stmt_indx)
{
	int count;
	cur_data cur;
	stmt_data *stmt;

	if(stmt_indx < 0) {
		stmt_indx = lua_gettop(L) + stmt_indx + 1;
	}

	stmt = getstatement(L,stmt_indx);

	/* is there already a cursor open */
	if(stmt->lock > 0) {
		return luasql_faildirect(L, "statement already has an open cursor");
	}

	memset(&cur, 0, sizeof(cur_data));
	cur.closed = 0;
	cur.stmt = stmt;
	cur.env = stmt->env;

	/* size the result, set if needed */
	cur.out_sqlda = malloc_xsqlda(1);
	isc_dsql_describe(cur.env->status_vector, &cur.stmt->handle, 1, cur.out_sqlda);
	if (cur.out_sqlda->sqld > cur.out_sqlda->sqln) {
		ISC_SHORT n = cur.out_sqlda->sqld;
		free_xsqlda(cur.out_sqlda);
		cur.out_sqlda = malloc_xsqlda(n);
		isc_dsql_describe(cur.env->status_vector, &cur.stmt->handle, 1,
		                  cur.out_sqlda);
		if ( CHECK_DB_ERROR(cur.env->status_vector) ) {
			free_cur(&cur);
			return return_db_error(L, cur.env->status_vector);
		}
	}
	malloc_sqlda_vars(cur.out_sqlda);

	/* does the statment return data? allocate a cursor */
	if(cur.out_sqlda->sqld > 0) {
		char cur_name[64];
		snprintf(cur_name, sizeof(cur_name), "dyn_cursor_%p", (void *)stmt);

		/* open the cursor ready for fetch cycles */
		isc_dsql_set_cursor_name(cur.env->status_vector, &cur.stmt->handle,
		                         cur_name, 0);
		if ( CHECK_DB_ERROR(cur.env->status_vector) ) {
			lua_pop(L, 1);	/* the userdata */
			free_cur(&cur);
			return return_db_error(L, cur.env->status_vector);
		}
	}

	/* run the query */
	isc_dsql_execute(stmt->env->status_vector, &stmt->conn->transaction,
	                 &stmt->handle, 1, stmt->in_sqlda);
	if ( CHECK_DB_ERROR(stmt->env->status_vector) ) {
		free_cur(&cur);
		return return_db_error(L, cur.env->status_vector);
	}

	/* what do we return? a cursor or a count */
	if(cur.out_sqlda->sqld > 0) { /* a cursor */
		cur_data *user_cur = (cur_data *)lua_newuserdata(L, sizeof(cur_data));
		luasql_setmeta (L, LUASQL_CURSOR_FIREBIRD);

		/* copy the cursor into a new lua userdata object */
		memcpy((void *)user_cur, (void *)&cur, sizeof(cur_data));

		/* add cursor to the lock count */
		registerobj(L, stmt_indx, user_cur->stmt);
		++user_cur->stmt->lock;
	} else { /* a count */
		/* if autocommit is set, commit change */
		if(cur.stmt->conn->autocommit) {
			isc_commit_retaining(cur.env->status_vector,
			                     &cur.stmt->conn->transaction);
			if ( CHECK_DB_ERROR(cur.env->status_vector) ) {
				free_cur(&cur);
				return return_db_error(L, cur.env->status_vector);
			}
		}

		if( (count = count_rows_affected(cur.env, &cur)) < 0 ) {
			free_cur(&cur);
			return return_db_error(L, cur.env->status_vector);
		}

		luasql_pushinteger(L, count);

		/* totaly finished with the cursor */
		isc_dsql_free_statement(cur.env->status_vector, &cur.stmt->handle,
		                        DSQL_close);
		free_cur(&cur);
	}

	return 1;
}

/*
** Executes a SQL statement.
** Lua input:
**   SQL statement
**  [parameter table]
** Returns
**   cursor object: if there are results or
**   row count: number of rows affected by statement if no results
**   nil and error message otherwise.
*/
static int conn_execute (lua_State *L)
{
	int ret;
	stmt_data *stmt;

	/* prepare the statement */
	if( (ret = conn_prepare(L)) != 1) {
		return ret;
	}

	/* execute and check result */
	if((ret = raw_execute(L, -1)) != 1) {
		return ret;
	}

	/* for neatness, remove stmt from stack */
	stmt = getstatement(L, -(ret+1));
	lua_remove(L, -(ret+1));

	/* this will be an internal, hidden statment */
	stmt->hidden = 1;

	/* if statement doesn't return a cursor, close it */
	if(luaL_testudata (L, -1, LUASQL_CURSOR_FIREBIRD) == NULL) {
		if((ret = stmt_shut(L, stmt)) != 0) {
			return ret;
		}
	}

	return 1;
}

/*
** Commits the current transaction
*/
static int conn_commit(lua_State *L) {
	conn_data *conn = getconnection(L,1);

	isc_commit_retaining(conn->env->status_vector, &conn->transaction);
	if ( CHECK_DB_ERROR(conn->env->status_vector) )
		return return_db_error(L, conn->env->status_vector);

	lua_pushboolean(L, 1);
	return 1;
}

/*
** Rolls back the current transaction
** Lua Returns:
**   1 if rollback is sucsessful
**   nil and error message otherwise.
*/
static int conn_rollback(lua_State *L) {
	conn_data *conn = getconnection(L,1);

	isc_rollback_retaining(conn->env->status_vector, &conn->transaction);
	if ( CHECK_DB_ERROR(conn->env->status_vector) )
		return return_db_error(L, conn->env->status_vector);

	lua_pushboolean(L, 1);
	return 1;
}

/*
** Sets the autocommit state of the connection
** Lua Returns:
**   autocommit state (0:off, 1:on)
**   nil and error message on error.
*/
static int conn_setautocommit(lua_State *L) {
	conn_data *conn = getconnection(L,1);

	if(lua_toboolean(L, 2))
		conn->autocommit = 1;
	else
		conn->autocommit = 0;

	lua_pushboolean(L, 1);
	return 1;
}

/*
** Closes a connection.
** Lua Returns:
**   1 if close was sucsessful, 0 if already closed
**   nil and error message otherwise.
*/
static int conn_close (lua_State *L)
{
	conn_data *conn = (conn_data *)luaL_checkudata(L,1,LUASQL_CONNECTION_FIREBIRD);
	luaL_argcheck (L, conn != NULL, 1, "connection expected");

	/* already closed */
	if(conn->closed != 0) {
		lua_pushboolean(L, 0);
		return 1;
	}

	/* are all related statements closed? */
	if(conn->lock > 0) {
		return luasql_faildirect(L, "there are still open statements/cursors");
	}

	if(conn->autocommit != 0) {
		isc_commit_transaction(conn->env->status_vector, &conn->transaction);
	} else {
		isc_rollback_transaction(conn->env->status_vector, &conn->transaction);
	}
	if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
		return return_db_error(L, conn->env->status_vector);
	}

	isc_detach_database(conn->env->status_vector, &conn->db);
	if ( CHECK_DB_ERROR(conn->env->status_vector) ) {
		return return_db_error(L, conn->env->status_vector);
	}

	conn->closed = 1;
	--conn->env->lock;

	/* check environment can be GC'd */
	if(conn->env->lock == 0) {
		unregisterobj(L, conn->env);
	}

	lua_pushboolean(L, 1);
	return 1;
}

/*
** GCs an connection object
*/
static int conn_gc (lua_State *L)
{
	conn_data *conn = (conn_data *)luaL_checkudata(L,1,LUASQL_CONNECTION_FIREBIRD);

	if(conn->closed == 0) {
		if(conn->autocommit != 0) {
			isc_commit_transaction(conn->env->status_vector,
			                       &conn->transaction);
		} else {
			isc_rollback_transaction(conn->env->status_vector,
			                         &conn->transaction);
		}

		isc_detach_database(conn->env->status_vector, &conn->db);

		conn->closed = 1;
		--conn->env->lock;

		/* check environment can be GC'd */
		if(conn->env->lock == 0) {
			unregisterobj(L, conn->env);
		}
	}

	return 0;
}

/*
** Escapes a given string so that it can't break out of it's delimiting quotes
*/
static int conn_escape(lua_State *L) {
	size_t len;
	const char *from = luaL_checklstring (L, 2, &len);
	char *res = malloc(len*sizeof(char)*2+1);
	char *to = res;

	if(res) {
		while(*from != '\0') {
			*(to++) = *from;
			if(*from == '\'')
				*(to++) = *from;

			from++;
		}
		*to = '\0';

		lua_pushstring(L, res);
		free(res);
		return 1;
	}

	luaL_error(L, "could not allocate escaped string");
	return 0;
}

/*
** Pushes the indexed value onto the lua stack
*/
static void push_column(lua_State *L, int i, cur_data *cur)
{
	int varcharlen;
	struct tm timevar;
	char timestr[256];
	ISC_STATUS blob_stat;
	isc_blob_handle blob_handle = 0;
	ISC_QUAD blob_id;
	luaL_Buffer b;
	char *buffer;
	unsigned short actual_seg_len;

	if( (cur->out_sqlda->sqlvar[i].sqlind != NULL) &&
	          (*(cur->out_sqlda->sqlvar[i].sqlind) != 0) ) {
		/* a null field? */
		lua_pushnil(L);
	} else {
		switch(cur->out_sqlda->sqlvar[i].sqltype & ~1) {
		case SQL_VARYING:
			varcharlen = (int)isc_vax_integer(cur->out_sqlda->sqlvar[i].sqldata, 2);
			lua_pushlstring(L, cur->out_sqlda->sqlvar[i].sqldata+2, varcharlen);
			break;
		case SQL_TEXT:
			lua_pushlstring(L, cur->out_sqlda->sqlvar[i].sqldata,
			                cur->out_sqlda->sqlvar[i].sqllen);
			break;
		case SQL_SHORT:
			luasql_pushinteger(L, *(ISC_SHORT *)(cur->out_sqlda->sqlvar[i].sqldata));
			break;
		case SQL_LONG:
			luasql_pushinteger(L, *(ISC_LONG *)(cur->out_sqlda->sqlvar[i].sqldata));
			break;
		case SQL_INT64:
			luasql_pushinteger(L, *(ISC_INT64 *)(cur->out_sqlda->sqlvar[i].sqldata));
			break;
		case SQL_FLOAT:
			lua_pushnumber(L, *(float *)(cur->out_sqlda->sqlvar[i].sqldata));
			break;
		case SQL_DOUBLE:
			lua_pushnumber(L, *(double *)(cur->out_sqlda->sqlvar[i].sqldata));
			break;
		case SQL_TYPE_TIME:
			isc_decode_sql_time((ISC_TIME *)(cur->out_sqlda->sqlvar[i].sqldata),
			                    &timevar);
			strftime(timestr, 255, "%X", &timevar);
			lua_pushstring(L, timestr);
			break;
		case SQL_TYPE_DATE:
			isc_decode_sql_date((ISC_DATE *)(cur->out_sqlda->sqlvar[i].sqldata),
			                    &timevar);
			strftime(timestr, 255, "%Y-%m-%d", &timevar);
			lua_pushstring(L, timestr);
			break;
		case SQL_TIMESTAMP:
			isc_decode_timestamp((ISC_TIMESTAMP *)(cur->out_sqlda->sqlvar[i].sqldata),
			                     &timevar);
			strftime(timestr, 255, "%Y-%m-%d %X", &timevar);
			lua_pushstring(L, timestr);
			break;
		case SQL_BLOB:
			/* get the BLOB ID and open it */
			memcpy(&blob_id, cur->out_sqlda->sqlvar[i].sqldata, sizeof(ISC_QUAD));
			isc_open_blob2(cur->env->status_vector, &cur->stmt->conn->db,
                              &cur->stmt->conn->transaction,
			               &blob_handle, &blob_id, 0, NULL );
			/* fetch the blob data */
			luaL_buffinit(L, &b);
			buffer = luaL_prepbuffer(&b);

			blob_stat = isc_get_segment(cur->env->status_vector,
			                            &blob_handle, &actual_seg_len,
			                            LUAL_BUFFERSIZE, buffer );
			while(blob_stat == 0 || cur->env->status_vector[1] == isc_segment) {
				luaL_addsize(&b, actual_seg_len);
				buffer = luaL_prepbuffer(&b);
				blob_stat = isc_get_segment(cur->env->status_vector,
				                            &blob_handle, &actual_seg_len,
				                            LUAL_BUFFERSIZE, buffer );
			}

			/* finnished, close the BLOB */
			isc_close_blob(cur->env->status_vector, &blob_handle);
			blob_handle = 0;

			luaL_pushresult(&b);
			break;
		default:
			lua_pushstring(L, "<unsupported data type>");
			break;
		}
	}
}

/*
** Returns a map of parameter IDs to their types
*/
static int stmt_get_params (lua_State *L)
{
	stmt_data *stmt = getstatement(L,1);

	return dump_xsqlda_types(L, stmt->in_sqlda);
}

/*
** Executes the statement
** Lua input:
**   [table of values]
** Returns
**   cursor object: if there are results or
**   row count: number of rows affected by statement if no results
**   nil and error message otherwise.
*/
static int stmt_execute (lua_State *L)
{
	stmt_data *stmt = getstatement(L,1);

	/* are there input params */
	if(stmt->in_sqlda->sqld > 0) {
		parse_params(L, stmt, 2);
		if ( CHECK_DB_ERROR(stmt->conn->env->status_vector) ) {
			return return_db_error(L, stmt->conn->env->status_vector);
		}
	}

	return raw_execute(L, 1);
}

/*
** Closes a statement object
** Lua Returns:
**   true if close was sucsessful, false if already closed
**   nil and error message otherwise.
*/
static int stmt_close (lua_State *L)
{
	stmt_data *stmt = (stmt_data *)luaL_checkudata(L,1,LUASQL_STATEMENT_FIREBIRD);
	luaL_argcheck (L, stmt != NULL, 1, "statement expected");

	if(stmt->lock > 0) {
		return luasql_faildirect(L, "there are still open cursors");
	}

	if(stmt->closed == 0) {
		int res = stmt_shut(L, stmt);
		if(res != 0) {
			return res;
		}

		/* return sucsess */
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushboolean(L, 0);
	return 1;
}

/*
** Frees up memory alloc'd to a statement
*/
static int stmt_gc (lua_State *L)
{
	stmt_data *stmt = (stmt_data *)luaL_checkudata(L,1,LUASQL_STATEMENT_FIREBIRD);
	luaL_argcheck (L, stmt != NULL, 1, "statement expected");

	if(stmt->closed == 0) {
		if(stmt_shut(L, stmt) != 0) {
			return 1;
		}
	}

	return 0;
}

/*
** Returns a row of data from the query
** Lua Returns:
**   list of results or table of results depending on call
**   nil and error message otherwise.
*/
static int cur_fetch (lua_State *L)
{
	ISC_STATUS fetch_stat;
	int i, res;
	cur_data *cur = (cur_data *)luaL_checkudata (L, 1, LUASQL_CURSOR_FIREBIRD);
	const char *opts = luaL_optstring (L, 3, "n");
	int num = strchr(opts, 'n') != NULL;
	int alpha = strchr(opts, 'a') != NULL;

	/* check cursor status */
	luaL_argcheck (L, cur != NULL, 1, "cursor expected");
	if (cur->closed) {
		return 0;
	}

	if ((fetch_stat = isc_dsql_fetch(cur->env->status_vector, &cur->stmt->handle,
	                                 1, cur->out_sqlda)) == 0) {
		if (lua_istable (L, 2)) {
			/* remove the option string */
			lua_settop(L, 2);

			/* loop through the columns */
			for (i = 0; i < cur->out_sqlda->sqld; i++) {
				push_column(L, i, cur);

				if (num) {
					lua_pushnumber(L, i+1);
					lua_pushvalue(L, -2);
					lua_settable(L, 2);
				}

				if (alpha) {
					lua_pushlstring(L, cur->out_sqlda->sqlvar[i].aliasname,
					                cur->out_sqlda->sqlvar[i].aliasname_length);
					lua_pushvalue(L, -2);
					lua_settable(L, 2);
				}

				lua_pop(L, 1);
			}

			/* returning given table */
			res = 1;
		} else {
			for (i = 0; i < cur->out_sqlda->sqld; i++) {
				push_column(L, i, cur);
			}

			/* returning a list of values */
			res = cur->out_sqlda->sqld;
		}

		/* close cursor for procedures/returnings as they (currently) only
		   return one result, and error on subsequent fetches */
		if (cur->stmt->type == isc_info_sql_stmt_exec_procedure) {
			cur_shut(L, cur);
		}

		return res;
	}

	/* isc_dsql_fetch returns 100 if no more rows remain to be retrieved
	   so this can be ignored */
	if (fetch_stat != 100L) {
		return return_db_error(L, cur->env->status_vector);
	}

	/* shut cursor */
	return cur_shut(L, cur);
}

/*
** Returns a table of column names from the query
** Lua Returns:
**   a table of column names
**   nil and error message otherwise.
*/
static int cur_colnames (lua_State *L)
{
	int i;
	XSQLVAR *var;
	cur_data *cur = getcursor(L,1);

	lua_newtable(L);

	for (i=1, var = cur->out_sqlda->sqlvar; i <= cur->out_sqlda->sqld; i++, var++) {
		lua_pushnumber(L, i);
		lua_pushlstring(L, var->aliasname, var->aliasname_length);
		lua_settable(L, -3);
	}

	return 1;
}

/*
** Returns a table of column types from the query
** Lua Returns:
**   a table of column types
**   nil and error message otherwise.
*/
static int cur_coltypes (lua_State *L)
{
	cur_data *cur = getcursor(L,1);

	return dump_xsqlda_types(L, cur->out_sqlda);
}

/*
** Closes a cursor object
** Lua Returns:
**   true if close was sucsessful, false if already closed
**   nil and error message otherwise.
*/
static int cur_close (lua_State *L)
{
	cur_data *cur = (cur_data *)luaL_checkudata(L,1,LUASQL_CURSOR_FIREBIRD);
	luaL_argcheck (L, cur != NULL, 1, "cursor expected");

	if(cur->closed == 0) {
		int res = cur_shut(L, cur);
		if(res != 0) {
			return res;
		}

		/* return sucsess */
		lua_pushboolean(L, 1);
		return 1;
	}

	lua_pushboolean(L, 0);
	return 1;
}

/*
** GCs a cursor object
*/
static int cur_gc (lua_State *L)
{
	cur_data *cur = (cur_data *)luaL_checkudata(L,1,LUASQL_CURSOR_FIREBIRD);
	luaL_argcheck (L, cur != NULL, 1, "cursor expected");

	if(cur->closed == 0) {
		if(cur_shut(L, cur) != 0) {
			return 1;
		}
	}

	return 0;
}

/*
** Creates an Environment and returns it.
*/
static int create_environment (lua_State *L)
{
	env_data *env;

	env = (env_data *)lua_newuserdata (L, sizeof (env_data));
	luasql_setmeta (L, LUASQL_ENVIRONMENT_FIREBIRD);
	memset (env, 0, sizeof (env_data));

	return 1;
}

/*
** Creates and returns a connection object
** Lua Input: source, user, pass
**   source: data source
**   user, pass: data source authentication information
** Lua Returns:
**   connection object if successfull
**   nil and error message otherwise.
*/
static int env_connect (lua_State *L) {
	char *dpb;
	int i;
	static char isc_tpb[] = {	isc_tpb_version3,
								isc_tpb_write		};
	conn_data conn;
	conn_data* res_conn;

	env_data *env = (env_data *) getenvironment (L, 1);
	const char *sourcename = luaL_checkstring (L, 2);
	const char *username = luaL_optstring (L, 3, "");
	const char *password = luaL_optstring (L, 4, "");

	conn.env = env;
	conn.db = 0L;
	conn.transaction = 0L;
	conn.lock = 0;
	conn.autocommit = 0;
	conn.dialect = 3;

	/* Construct a database parameter buffer. */
	dpb = conn.dpb_buffer;
	*dpb++ = isc_dpb_version1;
	*dpb++ = isc_dpb_num_buffers;
	*dpb++ = 1;
	*dpb++ = 90;

	/* add the user name and password */
	*dpb++ = isc_dpb_user_name;
    *dpb++ = (char)strlen(username);
	for(i=0; i<(int)strlen(username); i++)
		*dpb++ = username[i];
	*dpb++ = isc_dpb_password;
    *dpb++ = (char)strlen(password);
	for(i=0; i<(int)strlen(password); i++)
		*dpb++ = password[i];

	/* the length of the dpb */
	conn.dpb_length = (short)(dpb - conn.dpb_buffer);

	/* do the job */
	isc_attach_database(env->status_vector, (short)strlen(sourcename),
	                    (char *)sourcename, &conn.db,
	                    conn.dpb_length, conn.dpb_buffer);

	/* an error? */
	if ( CHECK_DB_ERROR(conn.env->status_vector) ) {
		return return_db_error(L, conn.env->status_vector);
	}

	/* open up the transaction handle */
	isc_start_transaction(env->status_vector, &conn.transaction, 1,
	                      &conn.db, (unsigned short)sizeof(isc_tpb),
	                      isc_tpb );

	/* an error? */
	if ( CHECK_DB_ERROR(conn.env->status_vector) ) {
		return return_db_error(L, conn.env->status_vector);
	}

	/* create the lua object and add the connection to the lock */
	res_conn = (conn_data *)lua_newuserdata(L, sizeof(conn_data));
	luasql_setmeta (L, LUASQL_CONNECTION_FIREBIRD);
	memcpy(res_conn, &conn, sizeof(conn_data));
	res_conn->closed = 0;   /* connect now officially open */

	/* register the connection */
	registerobj(L, 1, env);
	++env->lock;

	return 1;
}

/*
** Closes an environment object
** Lua Returns:
**   1 if close was sucsessful, 0 if already closed
**   nil and error message otherwise.
*/
static int env_close (lua_State *L)
{
	env_data *env = (env_data *)luaL_checkudata (L, 1, LUASQL_ENVIRONMENT_FIREBIRD);
	luaL_argcheck (L, env != NULL, 1, "environment expected");

	/* already closed? */
	if(env->closed == 1) {
		lua_pushboolean(L, 0);
		return 1;
	}

	/* check the lock */
	if(env->lock > 0) {
		return luasql_faildirect(L, "there are still open connections");
	}

	/* unregister */
	unregisterobj(L, env);

	/* mark as closed */
	env->closed = 1;

	lua_pushboolean(L, 1);
	return 1;
}

/*
** GCs an environment object
*/
static int env_gc (lua_State *L) {
	/* nothing to be done for the FB envronment */
	return 0;
}

/*
** Create metatables for each class of object.
*/
static void create_metatables (lua_State *L)
{
	struct luaL_Reg environment_methods[] = {
		{"__gc", env_gc},
		{"close", env_close},
		{"connect", env_connect},
		{NULL, NULL},
	};
	struct luaL_Reg connection_methods[] = {
		{"__gc", conn_gc},
		{"close", conn_close},
		{"prepare", conn_prepare},
		{"execute", conn_execute},
		{"commit", conn_commit},
		{"rollback", conn_rollback},
		{"setautocommit", conn_setautocommit},
		{"escape", conn_escape},
		{NULL, NULL},
	};
	struct luaL_Reg statement_methods[] = {
		{"__gc", stmt_gc},
		{"close", stmt_close},
		{"getparamtypes", stmt_get_params},
		{"execute", stmt_execute},
		{NULL, NULL},
	};
	struct luaL_Reg cursor_methods[] = {
		{"__gc", cur_gc},
		{"close", cur_close},
		{"fetch", cur_fetch},
		{"getcoltypes", cur_coltypes},
		{"getcolnames", cur_colnames},
		{NULL, NULL},
	};
	luasql_createmeta (L, LUASQL_ENVIRONMENT_FIREBIRD, environment_methods);
	luasql_createmeta (L, LUASQL_CONNECTION_FIREBIRD, connection_methods);
	luasql_createmeta (L, LUASQL_STATEMENT_FIREBIRD, statement_methods);
	luasql_createmeta (L, LUASQL_CURSOR_FIREBIRD, cursor_methods);
	lua_pop (L, 4);
}

/*
** Creates the metatables for the objects and registers the
** driver open method.
*/
LUASQL_API int luaopen_luasql_firebird (lua_State *L) {
	struct luaL_Reg driver[] = {
		{"firebird", create_environment},
		{NULL, NULL},
	};
	create_metatables (L);
	lua_newtable (L);
	luaL_setfuncs (L, driver, 0);
	luasql_set_info (L);
	return 1;
} 
