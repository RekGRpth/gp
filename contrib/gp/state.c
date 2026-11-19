/* -------------------------------------------------------------------------
 *
 * state.c
 *		The state of the nodes of the cluster, read through SQL
 *
 * The coordinator knows the nodes of the cluster, but not whether they
 * answer: a segment that went away keeps its last registration.  The
 * function asks every node with a short connection of its own, the way a
 * client would, and tells a live node from a dead one.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/state.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/wait_event.h"

#include "gp.h"

/* One row of the answer, probed before the function starts returning */
typedef struct GpNodeStateRow
{
	int			segment_id;
	char		host[GP_HOST_LEN];
	int			port;
	bool		up;
	bool		in_recovery;	/* meaningless when up is false */
	char	   *error;			/* why the node does not answer, NULL when up */
} GpNodeStateRow;

typedef struct GpNodeState
{
	GpNodeStateRow *rows;		/* the rows of the answer */
	int			nrows;			/* how many there are */
	int			next;			/* the one the next call answers */
} GpNodeState;

/* The wait event of the short connections, made on first use */
static uint32
gp_node_state_wait_event(void)
{
	static uint32 wait_event = 0;

	if (wait_event == 0)
		wait_event = WaitEventExtensionNew("GpNodeState");
	return wait_event;
}

/* Probe one node with a short connection, filling its row */
static void
gp_node_state_probe(GpNodeStateRow *row)
{
	const char *keywords[] = {"host", "port", "dbname", "user",
	"application_name", "connect_timeout", NULL};
	const char *values[7];
	char		port[12];
	PGconn	   *conn;
	PGresult   *res;

	snprintf(port, sizeof(port), "%d", row->port);
	values[0] = row->host;
	values[1] = port;
	values[2] = get_database_name(MyDatabaseId);
	values[3] = GetConfigOptionResetString("session_authorization");
	values[4] = "gp node state";
	values[5] = "5";
	values[6] = NULL;

	conn = libpqsrv_connect_params_start(keywords, values, false);
	libpqsrv_connect_complete(conn, gp_node_state_wait_event());
	if (PQstatus(conn) != CONNECTION_OK)
	{
		row->up = false;
		row->error = pchomp(PQerrorMessage(conn));
		libpqsrv_disconnect(conn);
		return;
	}

	if (!PQsendQuery(conn, "SELECT pg_is_in_recovery()"))
	{
		row->up = false;
		row->error = pchomp(PQerrorMessage(conn));
		libpqsrv_disconnect(conn);
		return;
	}
	res = libpqsrv_get_result_last(conn, gp_node_state_wait_event());
	if (res == NULL)
	{
		row->up = false;
		row->error = pchomp(PQerrorMessage(conn));
		libpqsrv_disconnect(conn);
		return;
	}
	if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
	{
		row->up = false;
		row->error = psprintf("unexpected answer: %s",
							  pchomp(PQresultErrorMessage(res)));
		PQclear(res);
		libpqsrv_disconnect(conn);
		return;
	}

	row->up = true;
	row->in_recovery = PQgetvalue(res, 0, 0)[0] == 't';
	PQclear(res);
	libpqsrv_disconnect(conn);
}

/*
 * The state of the nodes of the cluster: one row per registered segment,
 * probed with a short connection each.  The coordinator alone knows the
 * nodes, and the read is open to anyone: the state of the cluster is no
 * secret
 */
PG_FUNCTION_INFO_V1(gp_node_state);

Datum
gp_node_state(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	GpNodeState *state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		GpSegment  *segments;
		int			nsegments;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(6);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "segment_id",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "host",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "port",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "up",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "in_recovery",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "error",
						   TEXTOID, -1, 0);
		TupleDescFinalize(tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		nsegments = gp_segments_get(&segments);

		state = palloc0(sizeof(GpNodeState));
		state->rows = palloc0_array(GpNodeStateRow, nsegments);
		state->nrows = nsegments;

		for (int i = 0; i < nsegments; i++)
		{
			state->rows[i].segment_id = segments[i].segment_id;
			strlcpy(state->rows[i].host, segments[i].host,
					sizeof(state->rows[i].host));
			state->rows[i].port = segments[i].port;
			gp_node_state_probe(&state->rows[i]);
		}

		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = funcctx->user_fctx;

	if (state->next >= state->nrows)
		SRF_RETURN_DONE(funcctx);

	{
		GpNodeStateRow *row = &state->rows[state->next++];
		Datum		values[6];
		bool		nulls[6] = {0};

		values[0] = Int32GetDatum(row->segment_id);
		values[1] = CStringGetTextDatum(row->host);
		values[2] = Int32GetDatum(row->port);
		values[3] = BoolGetDatum(row->up);
		if (row->up)
			values[4] = BoolGetDatum(row->in_recovery);
		else
			nulls[4] = true;
		if (row->error != NULL)
			values[5] = CStringGetTextDatum(row->error);
		else
			nulls[5] = true;

		SRF_RETURN_NEXT(funcctx,
						HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
														  values, nulls)));
	}
}
