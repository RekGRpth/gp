/* -------------------------------------------------------------------------
 *
 * gather.c
 *		SELECT from a distributed table: all rows to the coordinator
 *
 * The table on the coordinator holds no rows, and the rows are on the
 * segments.  The hook of the paths of a relation puts a custom scan,
 * GpGather, in place of every path to a distributed table; the planner
 * plans the rest, the joins, the aggregates and the sorts, on the
 * coordinator as it would for any table.  GpGather sends a serialized scan to all segments and reads their binary
 * rows into a tuplestore, from which the rows go up the plan.  The conditions
 * of the query and the projection run on the coordinator, so what comes is
 * always correct, if not always cheap.  A replicated table has all its rows on
 * every segment, so only one of them is asked.
 *
 * A clean read goes over the interconnect under the lock of the distributed
 * snapshot.  A segment that the transaction of the coordinator has written to
 * instead answers on its dispatched libpq connection, so it shows those writes.
 * FOR UPDATE and TABLESAMPLE on a distributed table are refused.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/gather.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/value.h"
#include <math.h>
#include "access/cmptype.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "lib/binaryheap.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#include "gp.h"

/* The cost of the trip to the segments, before the first row */
#define GP_GATHER_STARTUP_COST 100.0

/* A row from the segments costs more than one of a table */
#define GP_GATHER_ROW_COST_FACTOR 10.0

static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static bool fragment_plan = false;

/* Let a fragment for a segment have an ordinary scan, not another gather */
bool
gp_gather_fragment(bool enable)
{
	bool		old = fragment_plan;

	fragment_plan = enable;
	return old;
}

typedef struct GpGatherState
{
	CustomScanState css;
	char	   *sql;
	bool		replicated;
	int			only;			/* the segment all the rows live on, 0 if not */
	bool		inherit;
	Tuplestorestate *store;		/* all rows, once read */
	List	   *sortkeys;		/* one list per key: attno, desc, nulls first, type, collation; NIL when the gather does not merge */
	Tuplestorestate **stores;	/* one sorted store per segment */
	int			nstores;
	binaryheap *heap;			/* the segment whose next row is the next answer */
	TupleTableSlot **peeks;		/* the unemitted next row of every store */
} GpGatherState;

static void gp_gather_append_order(StringInfoData *sql, List *sortkeys,
								   Oid relid);
static bool gp_gather_load_one(GpGatherState *state, GpSegment *segment,
							   int i, GpSnapshots *snapshots, const char *db,
							   const char *user, TupleDesc tupdesc,
							   char *plan);
static int	gp_gather_merge_heap_cmp(Datum a, Datum b, void *arg);

static Node *gp_gather_create_state(CustomScan *cscan);

static const CustomScanMethods gp_gather_plan_methods = {
	.CustomName = "GpGather",
	.CreateCustomScanState = gp_gather_create_state,
};

/*
 * Read a clean query over the interconnect.  Dropped columns are explicit
 * typed nulls in the fragment, so its answer has the table's full tupledesc.
 * The statement's commit lock covers every remote scan, and every worker
 * imports the snapshot exported for this statement on its own segment.
 */
static bool
gp_gather_load_interconnect(GpGatherState *state)
{
	ScanState  *ss = &state->css.ss;
	Relation	rel = ss->ss_currentRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData cols;
	StringInfoData sql;
	GpSegment  *segments;
	char	   *plan;
	char	   *db;
	GpSnapshots *snapshots;
	const char *user;
	int			nsegments;
	int			only = 0;

	/* The listener does not carry session_user or the session's RLS settings */
	if (GetUserId() != GetSessionUserId() || rel->rd_rel->relrowsecurity ||
		rel->rd_rel->relforcerowsecurity)
		return false;

	initStringInfo(&cols);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		appendStringInfo(&cols, "%s", cols.len > 0 ? ", " : "");
		if (att->attisdropped)
			appendStringInfo(&cols, "NULL::pg_catalog.int4 AS %s",
							 quote_identifier(NameStr(att->attname)));
		else
			appendStringInfoString(&cols, quote_identifier(NameStr(att->attname)));
	}
	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT %s FROM %s%s", cols.data,
					 state->inherit ? "" : "ONLY ",
					 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
											  RelationGetRelationName(rel)));
	if (state->sortkeys != NIL)
		gp_gather_append_order(&sql, state->sortkeys, rel->rd_id);
	plan = gp_interconnect_plan(sql.data);
	db = get_database_name(MyDatabaseId);
	user = GetUserNameFromId(GetUserId(), false);
	nsegments = gp_segments_get(&segments);
	if (state->replicated)
	{
		if (nsegments == 0)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("the cluster has no segments"));
		only = segments[MyProcPid % nsegments].segment_id;
	}
	else if (state->only != 0)
		only = state->only;

	/* A segment whose broker has not reported its port is not ready yet */
	for (int i = 0; i < nsegments; i++)
	{
		if (only != 0 && segments[i].segment_id != only)
			continue;
		if (segments[i].interconnect_port == 0)
			return false;
	}

	snapshots = gp_dtx_statement_snapshots(ss->ps.state);
	if (snapshots == NULL)
		return false;

	/*
	 * The merge reads one sorted segment after another: every store holds
	 * the sorted answer of its segment, and the gather hands out the rows
	 * in the order of the query without sorting anything here
	 */
	if (state->sortkeys != NIL)
	{
		state->nstores = 0;
		state->stores = palloc_array(Tuplestorestate *, nsegments);
		state->peeks = palloc_array(TupleTableSlot *, nsegments);
		for (int i = 0; i < nsegments; i++)
		{
			if (only != 0 && segments[i].segment_id != only)
				continue;
			gp_gather_load_one(state, &segments[i], state->nstores,
							   snapshots, db, user, tupdesc, plan);
			state->peeks[state->nstores] =
				MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);
			if (tuplestore_gettupleslot(state->stores[state->nstores], true,
										true, state->peeks[state->nstores]))
				state->nstores++;
			else
				tuplestore_end(state->stores[state->nstores]);
		}
		if (state->nstores == 0)
			state->stores = NULL;
		else
		{
			state->heap = binaryheap_allocate(state->nstores,
											  gp_gather_merge_heap_cmp, state);
			for (int i = 0; i < state->nstores; i++)
				binaryheap_add(state->heap, Int32GetDatum(i));
		}
		return true;
	}

	state->store = tuplestore_begin_heap(true, false, work_mem);
	{
		GpSegment  *wanted = palloc_array(GpSegment, nsegments);
		int			nwanted = 0;

		for (int i = 0; i < nsegments; i++)
		{
			if (only != 0 && segments[i].segment_id != only)
				continue;
			wanted[nwanted++] = segments[i];
		}
		gp_interconnect_pulls(state->store, tupdesc, db, user, plan, 0,
							  snapshots, wanted, nwanted);
	}
	return true;
}

/*
 * A dispatched connection is still needed to see this transaction's writes.
 */
static void
gp_gather_load_libpq(GpGatherState *state)
{
	ScanState  *ss = &state->css.ss;
	TupleDesc	tupdesc = RelationGetDescr(ss->ss_currentRelation);
	int			natts = tupdesc->natts;
	int		   *colmap = palloc_array(int, natts);
	FmgrInfo   *recv = palloc0_array(FmgrInfo, natts);
	Oid		   *ioparam = palloc_array(Oid, natts);
	int			ncols = 0;
	TupleTableSlot *loadslot;
	MemoryContext rowcxt;
	GpQuery    *query;
	PGresult   *res;
	int			only = 0;

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			recvfn;

		if (att->attisdropped)
			continue;
		getTypeBinaryInputInfo(att->atttypid, &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
		colmap[ncols++] = i;
	}

	state->store = tuplestore_begin_heap(true, false, work_mem);
	loadslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "gather row",
								   ALLOCSET_DEFAULT_SIZES);

	if (state->replicated)
	{
		GpSegment  *segments;
		int			n = gp_segments_get(&segments);

		if (n == 0)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("the cluster has no segments"));
		only = segments[MyProcPid % n].segment_id;
	}
	else if (state->only != 0)
	{
		only = state->only;
	}

	query = gp_query_start(state->sql, only, NULL);
	while ((res = gp_query_next(query)) != NULL)
	{
		MemoryContext oldcontext;

		if (PQnfields(res) != ncols)
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("a segment returned %d columns, but table \"%s\" has %d",
						   PQnfields(res),
						   RelationGetRelationName(ss->ss_currentRelation),
						   ncols));

		ExecClearTuple(loadslot);
		oldcontext = MemoryContextSwitchTo(rowcxt);
		for (int i = 0; i < natts; i++)
		{
			loadslot->tts_values[i] = (Datum) 0;
			loadslot->tts_isnull[i] = true;
		}
		for (int c = 0; c < ncols; c++)
		{
			int			i = colmap[c];
			StringInfoData buf;

			if (PQgetisnull(res, 0, c))
				continue;

			/* libpq ends every value with a zero byte, as the receive functions expect */
			buf.data = PQgetvalue(res, 0, c);
			buf.len = PQgetlength(res, 0, c);
			buf.maxlen = buf.len + 1;
			buf.cursor = 0;
			loadslot->tts_values[i] = ReceiveFunctionCall(&recv[i], &buf,
														  ioparam[i],
														  TupleDescAttr(tupdesc, i)->atttypmod);
			loadslot->tts_isnull[i] = false;
		}
		ExecStoreVirtualTuple(loadslot);
		tuplestore_puttupleslot(state->store, loadslot);
		MemoryContextSwitchTo(oldcontext);
		MemoryContextReset(rowcxt);
		PQclear(res);
	}

	ExecDropSingleTupleTableSlot(loadslot);
	MemoryContextDelete(rowcxt);
}

static void
gp_gather_load(GpGatherState *state)
{
	gp_dtx_statement_start(state->css.ss.ps.state);
	if (XactIsoLevel == XACT_READ_COMMITTED && !gp_dispatch_in_xact() &&
		gp_gather_load_interconnect(state))
		return;
	if (state->sortkeys != NIL)
		/*
		 * The order of the query is promised by the merge of the segments'
		 * sorted answers, and only the interconnect carries it
		 */
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("the sorted gather needs the interconnect"));
	gp_gather_load_libpq(state);
}

static void
gp_gather_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpGatherState *state = (GpGatherState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->sql = strVal(linitial(cscan->custom_private));
	state->replicated = boolVal(lsecond(cscan->custom_private));
	state->inherit = boolVal(lthird(cscan->custom_private));
	state->only = intVal(lfourth(cscan->custom_private));
	state->sortkeys = list_length(cscan->custom_private) > 4
		? list_nth(cscan->custom_private, 4)
		: NIL;
}

/* The comparison of the unemitted next rows of two segments: the smaller
 * one is the answer, the way the query order names it */
static int
gp_gather_merge_cmp(TupleTableSlot *a, TupleTableSlot *b, GpGatherState *state)
{
	ListCell   *lc;

	foreach(lc, state->sortkeys)
	{
		List	   *key = lfirst(lc);
		AttrNumber	attno = intVal(linitial(key));
		bool		isnull[2];
		Datum		values[2];
		int			cmp;

		values[0] = slot_getattr(a, attno, &isnull[0]);
		values[1] = slot_getattr(b, attno, &isnull[1]);
		if (isnull[0] || isnull[1])
		{
			if (isnull[0] && isnull[1])
				cmp = 0;
			else if (boolVal(lthird(key)))
				cmp = isnull[0] ? -1 : 1;
			else
				cmp = isnull[0] ? 1 : -1;
		}
		else
		{
			TypeCacheEntry *tce = lookup_type_cache((Oid) intVal(lfourth(key)),
													TYPECACHE_CMP_PROC_FINFO);

			cmp = DatumGetInt32(FunctionCall2Coll(&tce->cmp_proc_finfo,
												  (Oid) intVal(list_nth(key, 4)),
												  values[0], values[1]));
		}
		if (cmp != 0)
			return boolVal(lsecond(key)) ? -cmp : cmp;
	}
	return 0;
}

/* The heap keeps the indexes of the segments whose next row is pending */
static int
gp_gather_merge_heap_cmp(Datum a, Datum b, void *arg)
{
	GpGatherState *state = (GpGatherState *) arg;
	int			cmp = gp_gather_merge_cmp(state->peeks[DatumGetInt32(a)],
										  state->peeks[DatumGetInt32(b)], state);

	/*
	 * The heap keeps the largest node on top, so the comparison of the
	 * order is inverted: the row that comes first sinks to the top
	 */
	return cmp == 0 ? 0 : -cmp;
}

/* Load one segment into its own sorted store */
static bool
gp_gather_load_one(GpGatherState *state, GpSegment *segment, int i,
				   GpSnapshots *snapshots, const char *db, const char *user,
				   TupleDesc tupdesc, char *plan)
{
	state->stores[i] = tuplestore_begin_heap(true, false, work_mem);
	gp_interconnect_pulls(state->stores[i], tupdesc, db, user, plan, 0,
						  snapshots, segment, 1);
	return true;
}

/*
 * The answer of the gather is the merge of the segments' sorted stores: the
 * next row comes from the segment whose unemitted row is the smallest one
 */
static TupleTableSlot *
gp_gather_next_merged(GpGatherState *state, TupleTableSlot *slot)
{
	while (state->heap != NULL && !binaryheap_empty(state->heap))
	{
		int			i = DatumGetInt32(binaryheap_first(state->heap));

		ExecCopySlot(slot, state->peeks[i]);
		if (!tuplestore_gettupleslot(state->stores[i], true, true,
									 state->peeks[i]))
			binaryheap_remove_first(state->heap);
		else
			binaryheap_replace_first(state->heap, Int32GetDatum(i));
		return slot;
	}
	return NULL;
}

static TupleTableSlot *
gp_gather_next(ScanState *ss)
{
	GpGatherState *state = (GpGatherState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	gp_dtx_statement_start(state->css.ss.ps.state);
	if (state->store == NULL && state->stores == NULL)
		gp_gather_load(state);

	if (state->stores != NULL)
		return gp_gather_next_merged(state, slot);

	/* At the end the slot is empty, which is what ExecScan() looks for */
	(void) tuplestore_gettupleslot(state->store, true, false, slot);
	return slot;
}

static bool
gp_gather_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gp_gather_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gp_gather_next, gp_gather_recheck);
}

static void
gp_gather_end(CustomScanState *node)
{
	GpGatherState *state = (GpGatherState *) node;
	int			i;

	if (state->store != NULL)
		tuplestore_end(state->store);
	state->store = NULL;
	if (state->stores != NULL)
	{
		for (i = 0; i < state->nstores; i++)
		{
			tuplestore_end(state->stores[i]);
			ExecDropSingleTupleTableSlot(state->peeks[i]);
		}
		pfree(state->stores);
		pfree(state->peeks);
		state->stores = NULL;
		state->peeks = NULL;
	}
	if (state->heap != NULL)
	{
		binaryheap_free(state->heap);
		state->heap = NULL;
	}
	state->peeks = NULL;
}

/* The rows are all here, so a rescan reads them again from the stores */
static void
gp_gather_rescan(CustomScanState *node)
{
	GpGatherState *state = (GpGatherState *) node;
	int			i;

	if (state->store != NULL)
		tuplestore_rescan(state->store);
	if (state->stores != NULL)
	{
		binaryheap_reset(state->heap);
		for (i = 0; i < state->nstores; i++)
		{
			tuplestore_rescan(state->stores[i]);
			if (tuplestore_gettupleslot(state->stores[i], true, true,
										state->peeks[i]))
				binaryheap_add(state->heap, Int32GetDatum(i));
		}
	}
	ExecScanReScan(&node->ss);
}

static void
gp_gather_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpGatherState *state = (GpGatherState *) node;

	if (es->verbose)
		ExplainPropertyText("Remote SQL", state->sql, es);
}

static const CustomExecMethods gp_gather_exec_methods = {
	.CustomName = "GpGather",
	.BeginCustomScan = gp_gather_begin,
	.ExecCustomScan = gp_gather_exec,
	.EndCustomScan = gp_gather_end,
	.ReScanCustomScan = gp_gather_rescan,
	.ExplainCustomScan = gp_gather_explain,
};

static Node *
gp_gather_create_state(CustomScan *cscan)
{
	GpGatherState *state = (GpGatherState *) newNode(sizeof(GpGatherState),
													 T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_gather_exec_methods;
	/* A tuplestore hands out minimal tuples */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

/*
 * Make a plan of the path: the conditions and the projection stay with the
 * node, the scan tuple is a row of the table
 */
static Plan *
gp_gather_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_private = best_path->custom_private;
	cscan->methods = &gp_gather_plan_methods;
	return &cscan->scan.plan;
}

static const CustomPathMethods gp_gather_path_methods = {
	.CustomName = "GpGather",
	.PlanCustomPath = gp_gather_plan,
};

/* Whether a plan node is the gather of a distributed table */
bool
gp_is_gather(Plan *plan)
{
	return IsA(plan, CustomScan) &&
		((CustomScan *) plan)->methods == &gp_gather_plan_methods;
}

/*
 * The query that the segments run, and their columns, as the node reads
 * them
 */
static char *
gp_gather_sql(RangeTblEntry *rte)
{
	Relation	rel = table_open(rte->relid, NoLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData cols;
	char	   *sql;

	initStringInfo(&cols);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		appendStringInfo(&cols, "%s%s", cols.len > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
	}
	sql = psprintf("SELECT %s FROM %s%s", cols.data,
				   rte->inh ? "" : "ONLY ",
				   quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
											  RelationGetRelationName(rel)));
	table_close(rel, NoLock);
	return sql;
}

/* The ORDER BY clause of the sortkeys, to append to the gather SQL */
static void
gp_gather_append_order(StringInfoData *sql, List *sortkeys, Oid relid)
{
	ListCell   *lc;

	foreach(lc, sortkeys)
	{
		List	   *key = lfirst(lc);
		AttrNumber	attno = intVal(linitial(key));

		appendStringInfo(sql, "%s%s %s %s", lc == list_head(sortkeys) ?
						 " ORDER BY " : ", ",
						 quote_identifier(get_attname(relid, attno, false)),
						 boolVal(lsecond(key)) ? "DESC" : "ASC",
						 boolVal(lthird(key)) ? "NULLS FIRST" : "NULLS LAST");
	}
}

/*
 * The sortkeys of the query order, when every one of them is a plain column
 * of this table: the segments sort them, and the gather merges their sorted
 * answers.  NIL when the gather cannot promise the order
 */
static List *
gp_gather_sortkeys(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *keys = NIL;
	ListCell   *lc;

	foreach(lc, root->query_pathkeys)
	{
		PathKey    *pk = lfirst(lc);
		EquivalenceClass *ec = pk->pk_eclass;
		EquivalenceMember *em = NULL;
		ListCell   *lc2;
		Var		   *var;

		if (ec->ec_has_volatile)
			return NIL;
		foreach(lc2, ec->ec_members)
		{
			em = lfirst(lc2);

			if (!em->em_is_child && IsA(em->em_expr, Var) &&
				((Var *) em->em_expr)->varno == rel->relid)
				break;
			em = NULL;
		}
		if (em == NULL)
			return NIL;
		var = (Var *) em->em_expr;
		if (var->varattno <= 0)
			return NIL;
		keys = lappend(keys, list_make5(makeInteger(var->varattno),
										makeBoolean(pk->pk_cmptype ==
													COMPARE_GT),
										makeBoolean(pk->pk_nulls_first),
										makeInteger((int) var->vartype),
										makeInteger((int) exprCollation((Node *) var))));
	}
	return keys;
}

/*
 * The only path to a distributed table is GpGather.  A replicated table
 * keeps a copy on the coordinator as well, so it is read from it alone,
 * without a word to the segments
 */
static void
gp_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					RangeTblEntry *rte)
{
	GpPolicy	policy;
	CustomPath *cpath;
	Relation	relation;
	int			only = 0;

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);

	if (!IsUnderPostmaster || fragment_plan || rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION &&
		 rte->relkind != RELKIND_PARTITIONED_TABLE))
		return;

	/*
	 * A flattened UNION ALL leaf is an other member of an append relation
	 * whose parent has no relation oid: the planner pulled the leaf up into
	 * the query, and its scan runs where the query runs.  A partitioned
	 * parent builds its paths from such members, so a member must be read
	 * through the gather the same way the whole table is.  Members of real
	 * inheritance or partitioning (parent oid set) keep the usual paths.
	 */
	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
	{
		bool		setop_member = false;
		ListCell   *lc;

		foreach(lc, root->append_rel_list)
		{
			AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);

			if (appinfo->child_relid == rti)
			{
				setop_member = appinfo->parent_reloid == InvalidOid;
				break;
			}
		}
		if (!setop_member)
			return;
	}
	else if (rel->reloptkind != RELOPT_BASEREL)
		return;

	if (!gp_policy_get(rte->relid, &policy))
		return;

	/* The copy of a replicated table is here, and the paths are the usual */
	if (policy.replicated)
		return;

	/*
	 * The table itself is empty here.  ANALYZE imports the sizes of the
	 * cluster into pg_class, but the ordinary heap estimate still looks at
	 * that empty file.  Recompute the sizes before making the only path.
	 */
	relation = table_open(rte->relid, NoLock);
	if (relation->rd_rel->reltuples >= 0)
	{
		rel->pages = relation->rd_rel->relpages;
		rel->tuples = relation->rd_rel->reltuples;
		set_baserel_size_estimates(root, rel);
	}
	table_close(relation, NoLock);

	if (rte->tablesample != NULL)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("TABLESAMPLE is not supported on a distributed table"));
	foreach_node(PlanRowMark, rc, root->rowMarks)
	{
		if (rc->rti == rti)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("row locks are not supported on a distributed table"));
	}

	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/*
	 * A partitioned table is read as a whole: the planner must not build its
	 * paths again from those of the partitions, which are empty here
	 */
	if (rel->reloptkind == RELOPT_BASEREL)
	{
		rel->part_scheme = NULL;
		rel->nparts = 0;
	}
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = GP_GATHER_STARTUP_COST +
		rel->baserestrictcost.startup;
	cpath->path.total_cost = cpath->path.startup_cost +
		rel->tuples * cpu_tuple_cost * GP_GATHER_ROW_COST_FACTOR +
		rel->baserestrictcost.per_tuple * rel->tuples;
	cpath->path.pathkeys = NIL;
	cpath->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cpath->custom_paths = NIL;
	{
		/*
		 * A table pinned to a segment keeps every row on it: the gather
		 * reads that segment alone
		 */
		if (!policy.replicated && policy.nbuckets > 0)
		{
			int			b = 1;

			while (b < policy.nbuckets &&
				   policy.map[b] == policy.map[0])
				b++;
			if (b == policy.nbuckets)
				only = policy.map[0];
		}
		cpath->custom_private = list_make4(makeString(gp_gather_sql(rte)),
										   makeBoolean(policy.replicated),
										   makeBoolean(rte->inh),
										   makeInteger(only));
	}
	cpath->methods = &gp_gather_path_methods;

	add_path(rel, &cpath->path);

	/*
	 * The sorted gather: the segments sort their rows by the order of the
	 * query, and the gather merges their sorted answers without sorting the
	 * whole here.  It costs the segments' sort on top of the plain gather;
	 * the planner takes it when the sort above the gather costs more.  Only
	 * the plain query of one table takes it: the rescan the plans of a join
	 * ask of their inputs rewinds the merge, that machinery is not built
	 * yet, and an aggregate, a DISTINCT or a window of the query pushes its
	 * own work to the segments, where the sort of the gather is noise.  The
	 * interconnect carries the merge, so a gather whose rows must come over
	 * libpq keeps the plain path
	 */
	if (root->simple_rel_array_size == 2 &&
		root->parse->commandType == CMD_SELECT &&
		!root->parse->hasAggs && root->parse->groupClause == NIL &&
		root->parse->distinctClause == NIL &&
		!root->parse->hasWindowFuncs && root->parse->setOperations == NULL)
	{
		List	   *sortkeys = gp_gather_sortkeys(root, rel);

		if (sortkeys != NIL)
		{
			StringInfoData suffix;
			CustomPath *spath;
			int64		remote_limit = 0;

			initStringInfo(&suffix);
			gp_gather_append_order(&suffix, sortkeys, rte->relid);

			/*
			 * The query asks for the first rows of the order: the segments
			 * can cut their answers to the offset plus the count, and the
			 * merge works on a few rows per segment instead of the whole
			 * table.  Only literal counts take this: an expression of the
			 * LIMIT clause is not evaluated yet
			 */
			if (root->parse->limitCount != NULL &&
				IsA(root->parse->limitCount, Const))
			{
				Const	   *limit = (Const *) root->parse->limitCount;

				if (!limit->constisnull &&
					(limit->consttype == INT4OID ||
					 limit->consttype == INT8OID))
				{
					remote_limit = limit->consttype == INT4OID
						? (int64) DatumGetInt32(limit->constvalue)
						: DatumGetInt64(limit->constvalue);
					if (root->parse->limitOffset != NULL)
					{
						if (IsA(root->parse->limitOffset, Const))
						{
							Const	   *offset = (Const *) root->parse->limitOffset;

							if (!offset->constisnull &&
								(offset->consttype == INT4OID ||
								 offset->consttype == INT8OID))
								remote_limit += offset->consttype == INT4OID
									? (int64) DatumGetInt32(offset->constvalue)
									: DatumGetInt64(offset->constvalue);
						}
						else
							remote_limit = 0;
					}
					if (remote_limit < 0)
						remote_limit = 0;
				}
			}
			if (remote_limit > 0)
				appendStringInfo(&suffix, " LIMIT %lld",
								 (long long) remote_limit);

			spath = makeNode(CustomPath);
			spath->path = cpath->path;
			spath->path.pathkeys = root->query_pathkeys;
			spath->path.total_cost += 3.0 * rel->tuples *
				log(rel->tuples + 2.0) * cpu_operator_cost;
			if (remote_limit > 0 && spath->path.rows > (double) remote_limit)
			{
				double		factor = spath->path.total_cost /
					Max(spath->path.rows, 1.0);

				spath->path.rows = (double) remote_limit;
				spath->path.total_cost = spath->path.startup_cost +
					spath->path.rows * factor;
			}
			spath->custom_private = list_make5(makeString(psprintf("%s%s",
																	gp_gather_sql(rte),
																	suffix.data)),
											   makeBoolean(policy.replicated),
											   makeBoolean(rte->inh),
											   makeInteger(only),
											   sortkeys);
			spath->methods = &gp_gather_path_methods;
			add_path(rel, &spath->path);
		}
	}
}

/*
 * Install the hook on the coordinator; called from _PG_init()
 */
void
gp_gather_init(void)
{
	RegisterCustomScanMethods(&gp_gather_plan_methods);
	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gp_set_rel_pathlist;
}
