/* -------------------------------------------------------------------------
 *
 * motion.c
 *		Rows of a query from every segment of the cluster
 *
 * A query that joins tables of different colocation groups needs the rows
 * of some of them to move, so that every row meets its partners on one
 * segment.  The movement goes the pull way: every segment, as it runs the
 * query, asks every segment — itself among them — for rows.  A fragment is
 * sent as a serialized PlannedStmt, never as SQL text, and the answer is a
 * stream of binary tuples.  An inner join
 * either asks for the rows that the hash of the join key places on it, however
 * many tables move, or broadcasts a whole table when there is no such key.  An
 * outer join of two tables broadcasts a whole side instead, keeping rows with
 * null keys and rows without a partner.
 *
 * An aggregate whose grouping columns do not cover the distribution key moves
 * its rows the same way, but hashes those grouping columns: every group lands
 * on one segment, which computes the whole aggregate.  An INSERT ... SELECT
 * hashes its source rows by the distribution key of the target table, so
 * every segment inserts only the rows that belong to it.
 *
 * The coordinator leaves gp.peer_query in the query as the description of
 * the rows to move.  Its planner hook replaces that function by GpMotion,
 * a CustomScan with the same serialized fragment and answer columns.  The
 * function remains as a wrapper for compatibility, but a fragment contains
 * the motion node itself rather than a FunctionScan.
 *
 * The addresses of the segments are not kept here: they are asked of the
 * coordinator at every call, so a cluster that changed stays correct.  The
 * segment the rows are for is told to the others by a setting of the
 * connection, for the query itself is one for all of them.  Every segment
 * answers as the user that runs the query, in a READ ONLY transaction that
 * imports the snapshot exported for this statement on that segment.  The lock
 * of the distributed snapshot, held by the coordinator from before the exports
 * to after the last row, keeps all segments on one state of the cluster.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/motion.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/value.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_func.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "gp.h"

/* The cost of the trip to the segments, before the first row */
#define GP_MOTION_STARTUP_COST 100.0

/* A row from the segments costs more than one of a table */
#define GP_MOTION_ROW_COST_FACTOR 10.0

/* The wait event of the connections of the motion */
static uint32 gp_motion_wait_event = 0;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;

typedef struct GpMotionState
{
	CustomScanState css;
	char	   *serialized;
	Tuplestorestate *store;		/* all rows, once read */
} GpMotionState;

static Node *gp_motion_create_state(CustomScan *cscan);
static Plan *gp_motion_plan(PlannerInfo *root, RelOptInfo *rel,
							CustomPath *best_path, List *tlist,
							List *clauses, List *custom_plans);

static const CustomScanMethods gp_motion_plan_methods = {
	.CustomName = "GpMotion",
	.CreateCustomScanState = gp_motion_create_state,
};

static uint32
gp_motion_event(void)
{
	if (gp_motion_wait_event == 0)
		gp_motion_wait_event = WaitEventExtensionNew("GpMotion");
	return gp_motion_wait_event;
}

/*
 * Read the fragment of every peer in a session of its own: the call of
 * gp.fragment_exec carries the same serialized plan the workers answered,
 * and the peer executes it in an ordinary session, which the snapshots of
 * the statement were attached to.  A failure of one peer cancels what the
 * others still run and fails the pull
 */
static void
gp_motion_fetch_libpq(Tuplestorestate *store, TupleDesc tupdesc,
					  const char *serialized, GpSnapshots *snapshots,
					  GpSegment *wanted, int nsegments,
					  const char *db, const char *user)
{
	const char *keywords[] = {"host", "port", "dbname", "user",
	"application_name", "client_encoding", NULL};
	FmgrInfo   *recvfns;
	Oid		   *ioparam;
	PGconn	  **conns;
	bool	   *local;
	char	   *setup = NULL;
	char		port[12];
	StringInfoData sql;
	MemoryContext rowcxt;
	TupleTableSlot *loadslot;
	PGresult   *res;
	int			natts = tupdesc->natts;

	if (nsegments == 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the cluster has no segments"));

	/*
	 * The columns of the answer, as the peer's parser needs them for a
	 * function that returns record
	 */
	initStringInfo(&sql);
	appendStringInfoString(&sql,
						   "SELECT * FROM gp.fragment_exec($1) AS f(");
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		appendStringInfo(&sql, "%s%s %s", i > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)),
						 format_type_extended(att->atttypid, att->atttypmod,
											  FORMAT_TYPE_TYPEMOD_GIVEN |
											  FORMAT_TYPE_ALLOW_INVALID));
	}
	appendStringInfoString(&sql, ")");

	recvfns = palloc0_array(FmgrInfo, natts);
	ioparam = palloc_array(Oid, natts);
	for (int i = 0; i < natts; i++)
	{
		Oid			recvfn;

		if (TupleDescAttr(tupdesc, i)->attisdropped)
			continue;
		getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
							   &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recvfns[i]);
	}

	conns = palloc0_array(PGconn *, nsegments);
	local = palloc0_array(bool, nsegments);
	/*
	 * A session opens with who the rows are for: the filter of a
	 * redistribution reads the setting, the way the workers of the
	 * interconnect read it from their request.  The snapshots of the
	 * statement ride in the same transaction
	 */
	/*
	 * The fragments of one statement are read-only, the way the dispatched
	 * queries are: an immutable function that hides a write must not write
	 */
	if (snapshots != NULL)
		setup = psprintf("BEGIN READ ONLY; SET gp.requester_segment = %d; %s",
						 gp_segment_id,
						 psprintf("SELECT gp.attach_statement_snapshots('%s')",
								  gp_snapshots_serialize(snapshots)));
	else
		setup = psprintf("SET default_transaction_read_only = on; "
						 "SET gp.requester_segment = %d", gp_segment_id);

	PG_TRY();
	{
		for (int i = 0; i < nsegments; i++)
		{
			const char *values[6];

			elog(LOG, "GPDBG20 peer %d connect", wanted[i].segment_id);
			snprintf(port, sizeof(port), "%d", wanted[i].port);
			values[0] = wanted[i].host;
			values[1] = port;
			values[2] = db;
			values[3] = user;
			values[4] = "gp motion";
			values[5] = GetDatabaseEncodingName();
			conns[i] = libpqsrv_connect_params(keywords, values, true,
											   gp_motion_event());
			if (PQstatus(conns[i]) != CONNECTION_OK)
			{
				char	   *err = pchomp(PQerrorMessage(conns[i]));

				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not connect to segment %d at %s:%d",
							   wanted[i].segment_id, wanted[i].host,
							   wanted[i].port),
						errdetail("%s", err));
			}
			{
				local[i] = snapshots != NULL;
				if (!PQsendQuery(conns[i], setup))
					ereport(ERROR,
							errcode(ERRCODE_CONNECTION_FAILURE),
							errmsg("lost the connection to segment %d at %s:%d",
								   wanted[i].segment_id, wanted[i].host,
								   wanted[i].port),
							errdetail_internal("%s",
											   pchomp(PQerrorMessage(conns[i]))));
				while ((res = libpqsrv_get_result(conns[i],
												  gp_motion_event())) != NULL)
				{
					if (PQresultStatus(res) != PGRES_COMMAND_OK &&
						PQresultStatus(res) != PGRES_TUPLES_OK)
						ereport(ERROR,
								errcode(ERRCODE_CONNECTION_FAILURE),
								errmsg("could not set the session of segment %d up",
									   wanted[i].segment_id),
								errdetail_internal("%s",
												   pchomp(PQresultErrorMessage(res))));
					PQclear(res);
				}
			}
			elog(LOG, "GPDBG20 peer %d setup done, sending fragment", wanted[i].segment_id);
			if (!PQsendQueryParams(conns[i], sql.data, 1,
								   NULL, &serialized, NULL, NULL, 1) ||
				!PQsetSingleRowMode(conns[i]))
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("lost the connection to segment %d at %s:%d",
							   wanted[i].segment_id, wanted[i].host,
							   wanted[i].port),
						errdetail_internal("%s",
										   pchomp(PQerrorMessage(conns[i]))));
		}

		elog(LOG, "GPDBG20 all fragments sent, reading rows");
		rowcxt = AllocSetContextCreate(CurrentMemoryContext, "motion row",
									   ALLOCSET_DEFAULT_SIZES);
		loadslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
		for (int i = 0; i < nsegments; i++)
		{
			while ((res = libpqsrv_get_result(conns[i], gp_motion_event())) != NULL)
			{
				MemoryContext oldcontext;

				CHECK_FOR_INTERRUPTS();

				if (PQresultStatus(res) == PGRES_TUPLES_OK)
				{
					PQclear(res);
					continue;
				}
				if (PQresultStatus(res) != PGRES_SINGLE_TUPLE)
					ereport(ERROR,
							errcode(ERRCODE_CONNECTION_FAILURE),
							errmsg("segment %d returned an error",
								   wanted[i].segment_id),
							errdetail_internal("%s",
											   pchomp(PQresultErrorMessage(res))));

				ExecClearTuple(loadslot);
				oldcontext = MemoryContextSwitchTo(rowcxt);
				for (int a = 0; a < natts; a++)
				{
					StringInfoData buf;

					loadslot->tts_isnull[a] = true;
					loadslot->tts_values[a] = (Datum) 0;
					if (PQgetisnull(res, 0, a))
						continue;

					/* libpq ends every value with a zero byte, as the receive functions expect */
					buf.data = PQgetvalue(res, 0, a);
					buf.len = PQgetlength(res, 0, a);
					buf.maxlen = buf.len + 1;
					buf.cursor = 0;
					loadslot->tts_values[a] = ReceiveFunctionCall(&recvfns[a],
																 &buf,
																 ioparam[a],
																 TupleDescAttr(tupdesc, a)->atttypmod);
					loadslot->tts_isnull[a] = false;
				}
				ExecStoreVirtualTuple(loadslot);
				tuplestore_puttupleslot(store, loadslot);
				MemoryContextSwitchTo(oldcontext);
				MemoryContextReset(rowcxt);
				PQclear(res);
			}
		}
		elog(LOG, "GPDBG20 all rows read");
		ExecDropSingleTupleTableSlot(loadslot);
		MemoryContextDelete(rowcxt);
	}
	PG_FINALLY();
	{
		for (int i = 0; i < nsegments; i++)
		{
			PGconn	   *conn = conns[i];

			if (conn == NULL)
				continue;
			if (local[i])
			{
				if (PQsendQuery(conn, "COMMIT"))
					while ((res = libpqsrv_get_result(conn,
													  gp_motion_event())) != NULL)
						PQclear(res);
			}
			libpqsrv_disconnect(conn);
			conns[i] = NULL;
		}
	}
	PG_END_TRY();
}

/* Ask every segment, this one included, for the rows of the fragment */
static void
gp_motion_fetch(Tuplestorestate *store, TupleDesc tupdesc,
				const char *serialized)
{
	const char *coord_keywords[] = {"dbname", "dbname", "application_name",
	NULL};
	const char *coord_values[3];
	char	   *db;
	GpSnapshots *snapshots = gp_fragment_snapshots();
	PGconn	   *conn;
	PGresult   *segments;

	/* The segments of the cluster, asked of the coordinator */
	db = get_database_name(MyDatabaseId);
	coord_values[0] = gp_coordinator_conninfo();
	coord_values[1] = gp_database;
	coord_values[2] = "gp motion";
	conn = libpqsrv_connect_params(coord_keywords, coord_values, true,
								   gp_motion_event());
	if (PQstatus(conn) != CONNECTION_OK)
	{
		char	   *err = pchomp(PQerrorMessage(conn));

		libpqsrv_disconnect(conn);
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not ask the coordinator for the segments"),
				errdetail("%s", err));
	}
	/*
	 * The registry is the only address of a broker: there is no rule to
	 * fall back to.  A segment registers itself a moment before its broker
	 * reports its port, so that moment is waited out the way an
	 * unreachable broker is, not failed at once
	 */
	segments = NULL;
	for (int attempt = 0;; attempt++)
	{
		bool		reported = true;

		if (segments != NULL)
			PQclear(segments);
		segments = libpqsrv_exec_params(conn,
										"SELECT segment_id, host, port FROM gp.cluster_segments()",
										0, NULL, NULL, NULL, NULL, 0,
										gp_motion_event());
		if (PQresultStatus(segments) != PGRES_TUPLES_OK)
		{
			char	   *err = pchomp(PQresultErrorMessage(segments));

			PQclear(segments);
			libpqsrv_disconnect(conn);
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not read the segments of the cluster"),
					errdetail("%s", err));
		}

		if (reported || attempt >= 59)
			break;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000);
	}

	/* Tests stop a parent before its children share the statement snapshots */
	if (snapshots != NULL)
		INJECTION_POINT("gp-interconnect-parent", NULL);

	{
		int			nsegments = PQntuples(segments);
		GpSegment  *wanted = palloc0_array(GpSegment, nsegments);

		for (int i = 0; i < nsegments; i++)
		{
			wanted[i].segment_id = atoi(PQgetvalue(segments, i, 0));
			strlcpy(wanted[i].host, PQgetvalue(segments, i, 1),
					sizeof(wanted[i].host));
			wanted[i].port = atoi(PQgetvalue(segments, i, 2));

		}
		gp_motion_fetch_libpq(store, tupdesc, serialized, snapshots,
							  wanted, nsegments, db,
							  GetUserNameFromId(GetUserId(), false));
	}

	PQclear(segments);
	libpqsrv_disconnect(conn);
}

static TupleTableSlot *
gp_motion_next(ScanState *ss)
{
	GpMotionState *state = (GpMotionState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (state->store == NULL)
	{
		MemoryContext oldcontext;

		if (gp_role != GP_ROLE_SEGMENT)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("GpMotion runs on a segment only"));

		oldcontext = MemoryContextSwitchTo(ss->ps.state->es_query_cxt);
		state->store = tuplestore_begin_heap(true, false, work_mem);
		gp_motion_fetch(state->store, ss->ss_ScanTupleSlot->tts_tupleDescriptor,
						state->serialized);
		MemoryContextSwitchTo(oldcontext);
	}

	/* At the end the slot is empty, which is what ExecScan() looks for */
	(void) tuplestore_gettupleslot(state->store, true, false, slot);
	return slot;
}

static bool
gp_motion_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gp_motion_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gp_motion_next, gp_motion_recheck);
}

static void
gp_motion_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpMotionState *state = (GpMotionState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->serialized = strVal(linitial(cscan->custom_private));
}

static void
gp_motion_end(CustomScanState *node)
{
	GpMotionState *state = (GpMotionState *) node;

	if (state->store != NULL)
		tuplestore_end(state->store);
	state->store = NULL;
}

/* The rows are all here, so a rescan reads them again from the store */
static void
gp_motion_rescan(CustomScanState *node)
{
	GpMotionState *state = (GpMotionState *) node;

	if (state->store != NULL)
		tuplestore_rescan(state->store);
	ExecScanReScan(&node->ss);
}

static const CustomExecMethods gp_motion_exec_methods = {
	.CustomName = "GpMotion",
	.BeginCustomScan = gp_motion_begin,
	.ExecCustomScan = gp_motion_exec,
	.EndCustomScan = gp_motion_end,
	.ReScanCustomScan = gp_motion_rescan,
};

static Node *
gp_motion_create_state(CustomScan *cscan)
{
	GpMotionState *state = (GpMotionState *) newNode(sizeof(GpMotionState),
													 T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_motion_exec_methods;
	/* A tuplestore hands out minimal tuples */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

/* Make the plan of the path; setrefs maps its answer onto INDEX_VAR */
static Plan *
gp_motion_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	RangeTblFunction *rtf = linitial(rte->functions);
	List	   *scan_tlist = NIL;

	/* The source returns every column of the function, not only the columns
	 * that the plan above this one needs */
	for (int i = 0; i < rtf->funccolcount; i++)
	{
		Var		   *var = makeVar(rel->relid, i + 1,
								 list_nth_oid(rtf->funccoltypes, i),
								 list_nth_int(rtf->funccoltypmods, i),
								 list_nth_oid(rtf->funccolcollations, i), 0);
		const char *name = strVal(list_nth(rtf->funccolnames, i));

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) var, i + 1,
											 pstrdup(name), false));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = best_path->custom_private;
	cscan->methods = &gp_motion_plan_methods;
	return &cscan->scan.plan;
}

static const CustomPathMethods gp_motion_path_methods = {
	.CustomName = "GpMotion",
	.PlanCustomPath = gp_motion_plan,
};

/* The serialized argument of the planner's gp.peer_query marker */
static char *
gp_motion_serialized(RangeTblEntry *rte)
{
	RangeTblFunction *rtf;
	FuncExpr   *func;
	Const	   *arg;
	Oid			argtype = TEXTOID;
	Oid			peer_query_oid;

	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality ||
		list_length(rte->functions) != 1)
		return NULL;
	rtf = linitial(rte->functions);
	if (!IsA(rtf->funcexpr, FuncExpr))
		return NULL;
	func = (FuncExpr *) rtf->funcexpr;
	peer_query_oid = LookupFuncName(list_make2(makeString("gp"),
											 makeString("peer_query")),
									1, &argtype, true);
	if (!OidIsValid(peer_query_oid) || func->funcid != peer_query_oid ||
		list_length(func->args) != 1)
		return NULL;
	arg = (Const *) linitial(func->args);
	if (!IsA(arg, Const) || arg->consttype != TEXTOID || arg->constisnull)
		return NULL;
	return TextDatumGetCString(arg->constvalue);
}

/* Replace the function marker by a real motion path in a fragment */
static void
gp_motion_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
						   RangeTblEntry *rte)
{
	CustomPath *cpath;
	char	   *serialized;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (!IsUnderPostmaster || rel->reloptkind != RELOPT_BASEREL)
		return;
	serialized = gp_motion_serialized(rte);
	if (serialized == NULL)
		return;

	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = GP_MOTION_STARTUP_COST +
		rel->baserestrictcost.startup;
	cpath->path.total_cost = cpath->path.startup_cost +
		rel->tuples * cpu_tuple_cost * GP_MOTION_ROW_COST_FACTOR +
		rel->baserestrictcost.per_tuple * rel->tuples;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = list_make1(makeString(serialized));
	cpath->methods = &gp_motion_path_methods;

	add_path(rel, &cpath->path);
}

/*
 * The SQL wrapper remains available, but the planner turns its calls in a
 * fragment into GpMotion instead of executing a FunctionScan
 */
PG_FUNCTION_INFO_V1(gp_peer_query);

Datum
gp_peer_query(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *store;
	TupleDesc	tupdesc;
	char	   *sql;
	MemoryContext oldcontext;

	/* The planner of the coordinator writes the calls; none runs by hand */
	if (gp_role != GP_ROLE_SEGMENT)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.peer_query runs on a segment only"));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("set-valued function called in context that cannot accept a set"));
	if (rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.peer_query needs the columns of its answer"));
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("the query of the rows is required"));

	/* The result must survive the per-tuple context of the function call */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	store = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(oldcontext);
	sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	gp_motion_fetch(store, tupdesc, sql);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = store;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}

/*
 * Execute the serialized fragment of a query in this session and answer its
 * rows.  The pull of a motion asks every peer for its fragment with this
 * call, in a session of its own: the guards are the ones the workers kept, and the executor checks
 * the permissions of the caller as for any query
 */
PG_FUNCTION_INFO_V1(gp_fragment_exec);

Datum
gp_fragment_exec(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PlannedStmt *stmt;
	QueryDesc   *query;
	DestReceiver *dest;
	Tuplestorestate *store;
	TupleDesc	tupdesc;
	MemoryContext oldcontext;

	/* The pulls of the peers write the calls; none runs by hand */
	if (gp_role != GP_ROLE_SEGMENT)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.fragment_exec runs on a segment only"));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("set-valued function called in context that cannot accept a set"));
	if (rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("gp.fragment_exec needs the columns of its answer"));
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("the fragment is required"));

	INJECTION_POINT("gp-interconnect-fragment", NULL);

	stmt = (PlannedStmt *) stringToNode(text_to_cstring(PG_GETARG_TEXT_PP(0)));
	if (!IsA(stmt, PlannedStmt) || stmt->commandType != CMD_SELECT)
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("an invalid plan fragment"));
	foreach_node(RangeTblEntry, rte, stmt->rtable)
	{
		if (rte->rtekind == RTE_RELATION)
			LockRelationOid(rte->relid,
							rte->rellockmode != NoLock ? rte->rellockmode : AccessShareLock);
	}

	/* The result must survive the per-tuple context of the function call */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	store = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(oldcontext);

	dest = CreateTuplestoreDestReceiver();
	SetTuplestoreDestReceiverParams(dest, store,
									rsinfo->econtext->ecxt_per_query_memory,
									true, NULL, NULL);
	query = CreateQueryDesc(stmt, "gp.fragment_exec", GetActiveSnapshot(),
							NULL, dest, NULL, NULL, 0);
	ExecutorStart(query, 0);
	INJECTION_POINT("gp-interconnect-reader", NULL);
	ExecutorRun(query, ForwardScanDirection, 0L);
	ExecutorFinish(query);
	ExecutorEnd(query);
	FreeQueryDesc(query);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = store;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}

/* Install the executor everywhere and the planner hook on the coordinator */
void
gp_motion_init(void)
{
	RegisterCustomScanMethods(&gp_motion_plan_methods);
	if (gp_role == GP_ROLE_COORDINATOR)
	{
		prev_set_rel_pathlist = set_rel_pathlist_hook;
		set_rel_pathlist_hook = gp_motion_set_rel_pathlist;
	}
}

/* Whether a plan node is a motion between the segments */
bool
gp_is_motion(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &gp_motion_plan_methods;
}
