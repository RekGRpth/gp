/* -------------------------------------------------------------------------
 *
 * slice.c
 *		Aggregates of a distributed table, computed on the segments
 *
 * GpGather brings every row of a distributed table to the coordinator,
 * and the aggregate above it runs there.  When the query groups by the
 * whole key of the distribution, every row of a group is on one segment,
 * so the segment can compute the group to the end, and the coordinator
 * only collects the finished groups.  A replicated table has all its rows
 * on every segment, so one segment answers the whole aggregate.  GpSlice
 * is the scan that runs such an aggregate on the segments and reads their
 * rows; the sort and the limit of the query stay on the coordinator.
 *
 * The aggregate is replaced after the planner has run, by a scan that
 * returns exactly the target list of the aggregate.  Queries whose target
 * list has junk entries — a grouping or a sort key that is not selected —
 * are left to the planner, as are joins, subqueries, windows and grouping
 * sets.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/slice.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "commands/defrem.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_aggregate.h"
#include "catalog/namespace.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_class.h"
#include "parser/parse_func.h"
#include "parser/parse_agg.h"
#include "parser/parse_oper.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "gp.h"

typedef struct GpSliceState
{
	CustomScanState css;
	char	   *sql;
	bool		replicated;
	bool		motion;
	bool		autonomous;
	Tuplestorestate *store;		/* all rows, once read */
} GpSliceState;

static Node *gp_slice_create_state(CustomScan *cscan);

static const CustomScanMethods gp_slice_plan_methods = {
	.CustomName = "GpSlice",
	.CreateCustomScanState = gp_slice_create_state,
};

/* Where the rows of a plan subtree are computed, and by which columns they hash */
typedef enum GpLocusKind
{
	GP_LOCUS_COORDINATOR,		/* no distributed locus */
	GP_LOCUS_SEGMENTS,			/* a part of the answer on every segment */
	GP_LOCUS_ONE_SEGMENT,		/* the whole answer on one segment */
} GpLocusKind;

typedef struct GpLocus
{
	GpLocusKind kind;
	bool		replicated;		/* a copy is available on every segment */
	int			colocation_id;
	int			nkeys;
	AttrNumber *keys;			/* positions in the output of the subtree */
	Oid		   *keytypes;
	Oid		   *keycollations;
} GpLocus;

static GpLocus
gp_locus_make(GpLocusKind kind)
{
	GpLocus	locus = { .kind = kind };

	return locus;
}

static GpLocus
gp_locus_policy(const GpPolicy *policy)
{
	GpLocus	locus;

	if (policy->replicated)
	{
		locus = gp_locus_make(GP_LOCUS_ONE_SEGMENT);
		locus.replicated = true;
		return locus;
	}
	locus = gp_locus_make(GP_LOCUS_SEGMENTS);
	locus.colocation_id = policy->colocation_id;
	locus.nkeys = policy->nkeys;
	locus.keys = palloc_array(AttrNumber, policy->nkeys);
	memcpy(locus.keys, policy->keys, policy->nkeys * sizeof(AttrNumber));
	return locus;
}

/* Unknown hashing, but the rows are still split over the segments */
static GpLocus
gp_locus_unhash(GpLocus locus)
{
	locus.colocation_id = 0;
	locus.nkeys = 0;
	locus.keys = NULL;
	locus.keytypes = NULL;
	locus.keycollations = NULL;
	return locus;
}

/*
 * Map all the hash keys through a projection.  Losing one key of a compound
 * key, or computing an expression over it, makes the hashing unknown: hashing
 * by the remaining columns would not prove colocation.
 */
static GpLocus
gp_locus_project(Plan *plan, GpLocus child)
{
	AttrNumber *keys;
	Oid		   *types;
	Oid		   *collations;

	if (child.kind != GP_LOCUS_SEGMENTS || child.nkeys == 0)
		return child;
	keys = palloc_array(AttrNumber, child.nkeys);
	types = palloc_array(Oid, child.nkeys);
	collations = palloc_array(Oid, child.nkeys);
	for (int k = 0; k < child.nkeys; k++)
	{
		bool		found = false;
		ListCell   *lc;

		foreach(lc, plan->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Var		   *var;

			if (!IsA(tle->expr, Var))
				continue;
			var = (Var *) tle->expr;
			if (var->varattno == child.keys[k])
			{
				keys[k] = tle->resno;
				types[k] = var->vartype;
				collations[k] = var->varcollid;
				found = true;
				break;
			}
		}
		if (!found)
		{
			pfree(keys);
			pfree(types);
			pfree(collations);
			return gp_locus_unhash(child);
		}
	}
	child.keys = keys;
	child.keytypes = types;
	child.keycollations = collations;
	return child;
}

/* Sort and limit keep the output; Result and SubqueryScan may project it */
static GpLocus
gp_locus_unary(Plan *plan, GpLocus child)
{
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, IncrementalSort))
		return child;
	if (IsA(plan, Result) || IsA(plan, SubqueryScan))
		return gp_locus_project(plan, child);
	return gp_locus_make(GP_LOCUS_COORDINATOR);
}

/* Whether every hash key is present in a grouping/partition column list */
static bool
gp_locus_columns(GpLocus locus, Plan *below, int numCols,
				 AttrNumber *colIdx)
{
	if (locus.kind != GP_LOCUS_SEGMENTS || locus.nkeys == 0)
		return false;
	while (below != NULL && (IsA(below, Sort) || IsA(below, IncrementalSort)))
		below = below->lefttree;
	for (int k = 0; k < locus.nkeys; k++)
	{
		bool		grouped = false;

		for (int i = 0; i < numCols; i++)
		{
			TargetEntry *tle = list_nth(below->targetlist, colIdx[i] - 1);
			Var		   *var;

			if (!IsA(tle->expr, Var))
				return false;
			var = (Var *) tle->expr;
			if (var->varattno == locus.keys[k])
			{
				grouped = true;
				break;
			}
		}
		if (!grouped)
			return false;
	}
	return true;
}

/* Whether every hash key is a grouping column of the aggregate */
static bool
gp_locus_grouped(GpLocus locus, Agg *agg, Plan *below)
{
	return gp_locus_columns(locus, below, agg->numCols, agg->grpColIdx);
}

static GpLocus gp_locus_plan(Plan *plan, List *rtable, List *subplans);

static bool
gp_locus_join_var(Node *node, Index varno, AttrNumber attno)
{
	Var		   *var;

	if (!IsA(node, Var))
		return false;
	var = (Var *) node;
	return var->varno == varno && var->varattno == attno;
}

/* Whether a join clause equates this pair of distribution columns */
static bool
gp_locus_join_pair(Node *qual, AttrNumber outer, AttrNumber inner)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;

	if (!IsA(qual, OpExpr))
		return false;
	op = (OpExpr *) qual;
	if (list_length(op->args) != 2 || get_opname(op->opno) == NULL ||
		strcmp(get_opname(op->opno), "=") != 0)
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	return (gp_locus_join_var(left, OUTER_VAR, outer) &&
			gp_locus_join_var(right, INNER_VAR, inner)) ||
		(gp_locus_join_var(right, OUTER_VAR, outer) &&
		 gp_locus_join_var(left, INNER_VAR, inner));
}

/* All key columns of two colocated inputs must meet by equality */
static bool
gp_locus_join_keys(Join *join, GpLocus outer, GpLocus inner)
{
	bool	   *used;

	if (outer.kind != GP_LOCUS_SEGMENTS || inner.kind != GP_LOCUS_SEGMENTS ||
		outer.nkeys == 0 || outer.nkeys != inner.nkeys ||
		outer.colocation_id == 0 ||
		outer.colocation_id != inner.colocation_id)
		return false;
	used = palloc0_array(bool, inner.nkeys);
	for (int k = 0; k < outer.nkeys; k++)
	{
		bool		found = false;
		ListCell   *lc;

		foreach(lc, join->joinqual)
		{
			Node	   *qual = lfirst(lc);

			for (int j = 0; j < inner.nkeys; j++)
				if (!used[j] && gp_locus_join_pair(qual, outer.keys[k],
												 inner.keys[j]))
				{
					used[j] = true;
					found = true;
					break;
				}
			if (found)
				break;
		}
		if (!found)
		{
			pfree(used);
			return false;
		}
	}
	pfree(used);
	return true;
}

/* The locus of a binary join of two already computed inputs */
static GpLocus
gp_locus_join(Join *join, GpLocus outer, GpLocus inner)
{
	GpLocus	result;

	if (outer.kind == GP_LOCUS_COORDINATOR || inner.kind == GP_LOCUS_COORDINATOR)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (outer.kind == GP_LOCUS_ONE_SEGMENT && inner.kind == GP_LOCUS_ONE_SEGMENT)
	{
		result = gp_locus_make(GP_LOCUS_ONE_SEGMENT);
		result.replicated = outer.replicated && inner.replicated;
		return result;
	}
	if (outer.kind == GP_LOCUS_SEGMENTS && inner.kind == GP_LOCUS_SEGMENTS &&
		gp_locus_join_keys(join, outer, inner))
		/* A colocated outer join keeps every partner on its own segment */
		return gp_locus_project(&join->plan, outer);
	if (join->jointype == JOIN_FULL)
		/* Without colocation both inputs must meet on one chosen segment */
		return gp_locus_make(GP_LOCUS_ONE_SEGMENT);
	if (outer.kind == GP_LOCUS_ONE_SEGMENT)
	{
		if (!outer.replicated)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		return gp_locus_project(&join->plan, inner);
	}
	if (inner.kind == GP_LOCUS_ONE_SEGMENT)
	{
		if (!inner.replicated)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		return gp_locus_project(&join->plan, outer);
	}
	return gp_locus_make(GP_LOCUS_COORDINATOR);
}

/* All branches of an Append must have the same distribution to keep it */
static GpLocus
gp_locus_append(Append *append, List *rtable, List *subplans)
{
	GpLocus	result = gp_locus_make(GP_LOCUS_COORDINATOR);
	bool		first = true;

	foreach_ptr(Plan, child_plan, append->appendplans)
	{
		GpLocus	child = gp_locus_plan(child_plan, rtable, subplans);

		if (child.kind == GP_LOCUS_COORDINATOR)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		if (first)
		{
			result = child;
			first = false;
			continue;
		}
		if (result.kind != child.kind || result.replicated != child.replicated ||
			result.colocation_id != child.colocation_id ||
			result.nkeys != child.nkeys)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		for (int i = 0; i < result.nkeys; i++)
		{
			if (result.keys[i] != child.keys[i] ||
				(result.keytypes != NULL && child.keytypes != NULL &&
				 result.keytypes[i] != child.keytypes[i]) ||
				(result.keycollations != NULL && child.keycollations != NULL &&
				 result.keycollations[i] != child.keycollations[i]))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
		}
	}
	if (first)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	return gp_locus_project(&append->plan, result);
}

/* Set operations require every equal output row on the same segment */
static GpLocus
gp_locus_setop(SetOp *setop, List *rtable, List *subplans)
{
	GpLocus	outer = gp_locus_plan(setop->plan.lefttree, rtable, subplans);
	GpLocus	inner = gp_locus_plan(setop->plan.righttree, rtable, subplans);
	GpLocus	result;

	if (outer.kind != inner.kind)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (outer.kind == GP_LOCUS_ONE_SEGMENT)
	{
		if (!outer.replicated || !inner.replicated)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		result = gp_locus_make(GP_LOCUS_ONE_SEGMENT);
		result.replicated = true;
		return result;
	}
	if (outer.kind != GP_LOCUS_SEGMENTS || outer.nkeys == 0 ||
		outer.nkeys != inner.nkeys ||
		outer.colocation_id != inner.colocation_id)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	for (int i = 0; i < outer.nkeys; i++)
		if (outer.keys[i] != inner.keys[i] ||
			(outer.keytypes != NULL && inner.keytypes != NULL &&
			 outer.keytypes[i] != inner.keytypes[i]) ||
			(outer.keycollations != NULL && inner.keycollations != NULL &&
			 outer.keycollations[i] != inner.keycollations[i]))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	return gp_locus_project(&setop->plan, outer);
}

/* Compute the locus bottom-up without changing the plan */
static GpLocus
gp_locus_plan(Plan *plan, List *rtable, List *subplans)
{
	GpLocus	child;

	if (plan == NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (gp_is_gather(plan) || IsA(plan, SeqScan))
	{
		RangeTblEntry *rte;
		GpPolicy	policy;

		if (((Scan *) plan)->scanrelid == 0 ||
			((Scan *) plan)->scanrelid > list_length(rtable))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		rte = rt_fetch(((Scan *) plan)->scanrelid, rtable);

		if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		return gp_locus_policy(&policy);
	}
	if (IsA(plan, Gather))
		/* GpSlice is distributed itself; its full answer must not multiply here */
		return gp_locus_plan(plan->lefttree, rtable, subplans);
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, IncrementalSort) ||
		IsA(plan, Result))
	{
		child = gp_locus_plan(plan->lefttree, rtable, subplans);
		return gp_locus_unary(plan, child);
	}
	if (IsA(plan, SubqueryScan))
	{
		child = gp_locus_plan(((SubqueryScan *) plan)->subplan, rtable, subplans);
		return gp_locus_unary(plan, child);
	}
	if (IsA(plan, CteScan))
	{
		CteScan   *cte = (CteScan *) plan;

		if (subplans == NULL)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		child = gp_locus_plan(list_nth(subplans, cte->ctePlanId - 1),
							  rtable, subplans);
		return gp_locus_project(&cte->scan.plan, child);
	}
	if (IsA(plan, Append))
		return gp_locus_append((Append *) plan, rtable, subplans);
	if (IsA(plan, SetOp))
		return gp_locus_setop((SetOp *) plan, rtable, subplans);
	if (IsA(plan, Unique))
	{
		Unique	   *unique = (Unique *) plan;

		child = gp_locus_plan(unique->plan.lefttree, rtable, subplans);
		if (child.kind == GP_LOCUS_ONE_SEGMENT ||
			gp_locus_columns(child, unique->plan.lefttree,
							 unique->numCols, unique->uniqColIdx))
			return gp_locus_project(&unique->plan, child);
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	if (IsA(plan, WindowAgg))
	{
		WindowAgg  *window = (WindowAgg *) plan;

		child = gp_locus_plan(window->plan.lefttree, rtable, subplans);
		if (child.kind == GP_LOCUS_ONE_SEGMENT ||
			gp_locus_columns(child, window->plan.lefttree,
							 window->partNumCols, window->partColIdx))
			return gp_locus_project(&window->plan, child);
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	if (IsA(plan, Agg))
	{
		Agg		   *agg = (Agg *) plan;
		Plan	   *below = agg->plan.lefttree;

		while (below != NULL && IsA(below, Sort))
			below = below->lefttree;
		child = gp_locus_plan(agg->plan.lefttree, rtable, subplans);
		if (child.kind == GP_LOCUS_ONE_SEGMENT ||
			gp_locus_grouped(child, agg, below))
			return gp_locus_project(&agg->plan, child);
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	if (IsA(plan, NestLoop) || IsA(plan, HashJoin) || IsA(plan, MergeJoin))
	{
		Join	   *join = (Join *) plan;
		GpLocus	outer = gp_locus_plan(join->plan.lefttree, rtable, subplans);
		GpLocus	inner = gp_locus_plan(join->plan.righttree, rtable, subplans);

		return gp_locus_join(join, outer, inner);
	}
	return gp_locus_make(GP_LOCUS_COORDINATOR);
}

/*
 * Copy a query after the planner, or refuse it when it cannot be deparsed.
 * A pulled-up or removed subquery keeps its slot with subquery = NULL, and the
 * join aliases of the flattened tree belong to the planner, not to ruleutils.
 * Such a tree is good for analysis, but not for pg_get_querydef: keep the
 * coordinator's plan rather than descend into a stale subquery.
 */
static Query *
gp_slice_query_copy(Query *parse)
{
	foreach_node(RangeTblEntry, rte, parse->rtable)
	{
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery == NULL)
			return NULL;
		if (rte->rtekind == RTE_JOIN &&
			list_length(rte->joinaliasvars) != list_length(rte->eref->colnames))
			return NULL;
	}
	return copyObject(parse);
}

static bool gp_slice_union_leaf(Query *query);
static CustomScan *gp_slice_make(Plan *src, Query *original, bool replicated,
								 bool motion, List *sortClause);
typedef struct GpJoinTable GpJoinTable;
static bool gp_slice_join_rows(Query *parse, GpJoinTable *table, double *rows);
static bool gp_slice_agg_locus(Query *original, Agg *agg, Plan *below,
							   bool *replicated, bool *motion, GpLocus *locus);
static bool gp_slice_cte_agg_replace(Query *ctequery, PlannedStmt *stmt,
									 int cteplanid, Plan *parent, Agg *agg,
									 bool replicated, bool motion);
static bool gp_slice_base_column(Plan *plan, TargetEntry *tle, Index *rti,
								 AttrNumber *attno);
static bool gp_slice_join_motion_affordable(Query *parse, List *tables,
											List *conds);
static bool gp_slice_join_partial(Query *original, PlannedStmt *stmt,
								  Plan *plan, Plan *parent, bool sort_above,
								  bool motion);
static bool gp_slice_join_redistributed_agg(Query *original, PlannedStmt *stmt,
											Plan *plan, Plan *parent,
											bool sort_above, List *tables);

/* A Sort added after setrefs refers to its child as OUTER_VAR */
static Node *
gp_slice_sort_vars(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *var = (Var *) node;

		var->varno = OUTER_VAR;
		var->varnosyn = OUTER_VAR;
	}
	return expression_tree_mutator(node, gp_slice_sort_vars, context);
}

/* Keep the original visibility of junk columns in an added Sort */
static void
gp_slice_sort_target(Query *original, Sort *sort)
{
	ListCell   *lc;

	sort->plan.targetlist = (List *) gp_slice_sort_vars(
		(Node *) copyObject(sort->plan.targetlist), NULL);
	foreach(lc, sort->plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *orig = get_tle_by_resno(original->targetList, tle->resno);

		if (orig != NULL)
			tle->resjunk = orig->resjunk;
	}
}

/*
 * The request worker does not inherit the settings of the session.  Only an
 * immutable fragment can run there without changing the answer.  The motion
 * function itself is the exception: its serialized argument was planned by
 * the coordinator, and the function is just the transport of that fragment.
 */
static bool
gp_slice_mutable_checker(Oid func_id, void *context)
{
	Oid			peer_query_oid = *((Oid *) context);

	return func_id != peer_query_oid &&
		func_volatile(func_id) != PROVOLATILE_IMMUTABLE;
}

static bool
gp_slice_mutable_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (check_functions_in_node(node, gp_slice_mutable_checker, context))
		return true;

	/* These nodes need the rules of contain_mutable_functions() too.  Be
	 * conservative rather than duplicate its detailed JSON and XML rules. */
	if (IsA(node, SQLValueFunction) || IsA(node, NextValueExpr) ||
		IsA(node, JsonConstructorExpr) || IsA(node, JsonExpr) ||
		IsA(node, XmlExpr))
		return true;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node, gp_slice_mutable_walker,
							 context, 0);
	return expression_tree_walker(node, gp_slice_mutable_walker, context);
}

/* Whether an autonomous request preserves the semantics of this session */
static bool
gp_slice_autonomous(Query *query)
{
	Oid			peer_query_oid;

	if (GetUserId() != GetSessionUserId())
		return false;

	foreach_node(RangeTblEntry, rte, query->rtable)
	{
		HeapTuple	tuple;
		Form_pg_class rel;

		if (rte->rtekind != RTE_RELATION)
			continue;
		/* Children can have their own RLS policies; keep this case on libpq */
		if (rte->inh)
			return false;
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", rte->relid);
		rel = (Form_pg_class) GETSTRUCT(tuple);
		if (rel->relrowsecurity || rel->relforcerowsecurity)
		{
			ReleaseSysCache(tuple);
			return false;
		}
		ReleaseSysCache(tuple);
	}

	peer_query_oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
								  CStringGetDatum("gp.peer_query(text)")));
	return !gp_slice_mutable_walker((Node *) query, &peer_query_oid);
}

/*
 * Read all rows of the aggregate from the segments into the tuplestore.
 * The columns of the result are those of the target list of the node.
 */
static void
gp_slice_load_libpq(GpSliceState *state)
{
	ScanState  *ss = &state->css.ss;
	TupleDesc	tupdesc = ss->ss_ScanTupleSlot->tts_tupleDescriptor;
	int			natts = tupdesc->natts;
	FmgrInfo   *recv = palloc0_array(FmgrInfo, natts);
	Oid		   *ioparam = palloc_array(Oid, natts);
	TupleTableSlot *loadslot;
	MemoryContext rowcxt;
	GpQuery    *query;
	PGresult   *res;
	int			only = 0;

	if (state->motion && gp_dispatch_in_xact())
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("a motion query cannot see writes made earlier in the same transaction"),
				errhint("Run the query before writing, or prepare it after the write."));

	for (int i = 0; i < natts; i++)
	{
		Oid			recvfn;

		getTypeBinaryInputInfo(TupleDescAttr(tupdesc, i)->atttypid,
							   &recvfn, &ioparam[i]);
		fmgr_info(recvfn, &recv[i]);
	}

	state->store = tuplestore_begin_heap(true, false, work_mem);
	gp_dtx_statement_start(ss->ps.state);
	loadslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "slice row",
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

	/* A fragment with motions reads its peers through interconnect workers
	 * that import the statement snapshots; attach the map to the fragment
	 * itself, so its own scans see the same distributed state */
	query = gp_query_start(state->sql, only,
						   state->motion ?
						   gp_dtx_statement_snapshots(ss->ps.state) : NULL);
	PG_TRY();
	{
	while ((res = gp_query_next(query)) != NULL)
	{
		MemoryContext oldcontext;

		if (PQnfields(res) != natts)
			ereport(ERROR,
					errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("a segment returned %d columns, but the aggregate has %d",
						   PQnfields(res), natts));

		ExecClearTuple(loadslot);
		oldcontext = MemoryContextSwitchTo(rowcxt);
		for (int i = 0; i < natts; i++)
		{
			StringInfoData buf;

			loadslot->tts_isnull[i] = true;
			loadslot->tts_values[i] = (Datum) 0;
			if (PQgetisnull(res, 0, i))
				continue;

			/* libpq ends every value with a zero byte, as the receive functions expect */
			buf.data = PQgetvalue(res, 0, i);
			buf.len = PQgetlength(res, 0, i);
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
	PG_FINALLY();
	{
		gp_query_end(query);
	}
	PG_END_TRY();
}

/* Run a clean immutable fragment in autonomous interconnect workers */
static bool
gp_slice_load_interconnect(GpSliceState *state)
{
	ScanState  *ss = &state->css.ss;
	TupleDesc	tupdesc = ss->ss_ScanTupleSlot->tts_tupleDescriptor;
	GpSegment  *segments;
	char	   *plan;
	char	   *db;
	GpSnapshots *snapshots;
	const char *user;
	int			nsegments;
	int			only = 0;

	if (!state->autonomous || GetUserId() != GetSessionUserId())
		return false;

	plan = gp_interconnect_plan(state->sql);
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
	state->store = tuplestore_begin_heap(true, false, work_mem);
	for (int i = 0; i < nsegments; i++)
	{
		if (only != 0 && segments[i].segment_id != only)
			continue;
		if (!gp_interconnect_query_try(state->store, tupdesc, db, user, plan,
								   segments[i].host,
								   segments[i].interconnect_port, 0,
								   snapshots))
		{
			/* Earlier workers may have returned rows already */
			tuplestore_end(state->store);
			state->store = NULL;
			return false;
		}
	}
	return true;
}

static void
gp_slice_load(GpSliceState *state)
{
	gp_dispatches_bump();
	if (XactIsoLevel == XACT_READ_COMMITTED && !gp_dispatch_in_xact() &&
		gp_slice_load_interconnect(state))
		return;
	gp_slice_load_libpq(state);
}

static void
gp_slice_begin(CustomScanState *node, EState *estate, int eflags)
{
	GpSliceState *state = (GpSliceState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	state->sql = strVal(linitial(cscan->custom_private));
	state->replicated = boolVal(lsecond(cscan->custom_private));
	state->motion = boolVal(lthird(cscan->custom_private));
	state->autonomous = boolVal(lfourth(cscan->custom_private));
}

static TupleTableSlot *
gp_slice_next(ScanState *ss)
{
	GpSliceState *state = (GpSliceState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	gp_dtx_statement_start(ss->ps.state);
	if (state->store == NULL)
		gp_slice_load(state);

	/* At the end the slot is empty, which is what ExecScan() looks for */
	(void) tuplestore_gettupleslot(state->store, true, false, slot);
	return slot;
}

static bool
gp_slice_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gp_slice_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, gp_slice_next, gp_slice_recheck);
}

static void
gp_slice_end(CustomScanState *node)
{
	GpSliceState *state = (GpSliceState *) node;

	if (state->store != NULL)
		tuplestore_end(state->store);
	state->store = NULL;
}

/* The rows are all here, so a rescan reads them again from the store */
static void
gp_slice_rescan(CustomScanState *node)
{
	GpSliceState *state = (GpSliceState *) node;

	if (state->store != NULL)
		tuplestore_rescan(state->store);
	ExecScanReScan(&node->ss);
}

static void
gp_slice_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GpSliceState *state = (GpSliceState *) node;

	if (es->verbose)
		ExplainPropertyText("Remote SQL", state->sql, es);
}

static const CustomExecMethods gp_slice_exec_methods = {
	.CustomName = "GpSlice",
	.BeginCustomScan = gp_slice_begin,
	.ExecCustomScan = gp_slice_exec,
	.EndCustomScan = gp_slice_end,
	.ReScanCustomScan = gp_slice_rescan,
	.ExplainCustomScan = gp_slice_explain,
};

static Node *
gp_slice_create_state(CustomScan *cscan)
{
	GpSliceState *state = (GpSliceState *) newNode(sizeof(GpSliceState),
												   T_CustomScanState);

	state->css.flags = cscan->flags;
	state->css.methods = &gp_slice_exec_methods;
	/* A tuplestore hands out minimal tuples */
	state->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) state;
}

/*
 * Whether the query is an aggregate that the segments can compute whole:
 * one distributed table, nothing else.  For a table distributed by hash,
 * the grouping must include the whole key of the distribution, so that
 * every group is on one segment.  A copy of the query is returned for the
 * SQL of the segments, or NULL when the query is not such an aggregate.
 */
Query *
gp_slice_agg_query(Query *parse)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;
	Index		rti;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasAggs)
		return NULL;
	if (parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return NULL;

	/* One table, and the FROM is that table alone */
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return NULL;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	rti = ref->rtindex;
	rte = rt_fetch(rti, parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
		return NULL;

	/* A replicated table is answered by one segment as a whole */
	if (policy.replicated)
		return gp_slice_query_copy(parse);

	/* A table by hash without a grouping cannot be answered by one segment */
	if (parse->groupClause == NIL)
		return NULL;

	return gp_slice_query_copy(parse);
}

/*
 * Replace the aggregate of the plan with a scan that runs it on the
 * segments, when the plan is the aggregate over the gather of the one
 * table.  The sort and the limit above the aggregate are left in place.
 */
bool
gp_slice_agg_plan(Query *original, PlannedStmt *stmt)
{
	/* Returns whether the plan was rewritten */
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	Agg		   *agg;
	Plan	   *below;
	CustomScan *cscan;
	bool		replicated = false;
	bool		motion = false;
	GpLocus	locus;

	/* Find the aggregate, below whatever sorts and limits sit on top */
	while (plan != NULL && !IsA(plan, Agg))
	{
		if (plan->righttree != NULL)
			return false;
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;
	agg = (Agg *) plan;

	/*
	 * A sorted aggregate answers the ORDER BY of the query by itself, with
	 * no sort on top: its replacement would not answer it.  A sort on top
	 * sorts the groups of the segments as it sorted the groups of the gather
	 */
	if (original->sortClause != NIL && !sort_above)
		return false;

	/* Below the aggregate there is the gather of the table and nothing else */
	below = agg->plan.lefttree;
	while (below != NULL && (IsA(below, Sort) || IsA(below, IncrementalSort)))
		below = below->lefttree;
	if (below == NULL || !gp_is_gather(below))
		return false;

	locus = gp_locus_plan(&agg->plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
	{
		/* A grouping that is not colocated can still move by its columns */
		if (!gp_slice_agg_locus(original, agg, below, &replicated, &motion, &locus))
			return false;
	}
	else
		replicated = locus.kind == GP_LOCUS_ONE_SEGMENT;

	/* The columns the segments return are the target list of the aggregate */
	if (list_length(agg->plan.targetlist) !=
		list_length(original->targetList))
		return false;

	cscan = gp_slice_make(&agg->plan, original, replicated, motion,
						  original->sortClause);

	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;

	return true;

}

/* One nonrecursive CTE over one distributed table */
Query *
gp_slice_cte_query(Query *parse)
{
	CommonTableExpr *cte;
	RangeTblEntry *rte;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->hasAggs || parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->groupClause != NIL ||
		parse->groupingSets != NIL || parse->distinctClause != NIL ||
		parse->rowMarks != NIL || list_length(parse->cteList) != 1)
		return NULL;
	cte = linitial_node(CommonTableExpr, parse->cteList);
	if (cte->cterecursive || !gp_slice_union_leaf((Query *) cte->ctequery))
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_CTE)
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace a CteScan by the whole query, including its CTE */
bool
gp_slice_cte_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || !IsA(plan, CteScan))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

/*
 * Whether every CteScan of the statement reads the same subplan, and its
 * number.  The scans sit in the plan tree and in the subplans themselves,
 * for the InitPlan above a CteScan keeps the scan of its own.
 */
static bool
gp_slice_cte_agg_scan(Plan *plan, int *cteplanid)
{
	if (plan == NULL)
		return true;
	if (IsA(plan, CteScan))
	{
		if (*cteplanid != 0 && *cteplanid != ((CteScan *) plan)->ctePlanId)
			return false;
		*cteplanid = ((CteScan *) plan)->ctePlanId;
	}
	if (!gp_slice_cte_agg_scan(plan->lefttree, cteplanid) ||
		!gp_slice_cte_agg_scan(plan->righttree, cteplanid))
		return false;
	if (IsA(plan, SubqueryScan))
		return gp_slice_cte_agg_scan(((SubqueryScan *) plan)->subplan, cteplanid);
	if (IsA(plan, Append))
	{
		foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
			if (!gp_slice_cte_agg_scan(child, cteplanid))
				return false;
	}
	if (IsA(plan, MergeAppend))
	{
		foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
			if (!gp_slice_cte_agg_scan(child, cteplanid))
				return false;
	}
	return true;
}

/*
 * The query of a nonrecursive CTE is an aggregate of one table, which the
 * segments can answer whole.  The outer query — with its join, its sublinks
 * and its aggregates — stays on the coordinator above the scans of the CTE.
 * A copy of the CTE query is returned for the SQL of the segments, or NULL
 * when the CTE is not such an aggregate.
 */
Query *
gp_slice_cte_agg_query(Query *parse)
{
	CommonTableExpr *cte;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		list_length(parse->cteList) != 1)
		return NULL;
	cte = linitial_node(CommonTableExpr, parse->cteList);
	if (cte->cterecursive)
		return NULL;
	return gp_slice_agg_query(castNode(Query, cte->ctequery));
}

/*
 * Replace the aggregate of a CTE with a scan that runs it on the segments,
 * in the subplan that the CteScan nodes of the statement read.  The outer
 * query keeps its plan: it sees the same rows of the CTE as before.  A CTE
 * whose grouping covers the key of the distribution is answered where the
 * table is; over another grouping the rows of the table move by the
 * grouping columns, so every group is whole on one segment.
 */
bool
gp_slice_cte_agg_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *subplan;
	Plan	   *parent = NULL;
	Plan	   *plan;
	Plan	   *below;
	Agg		   *agg;
	int			cteplanid = 0;
	bool		replicated = false;
	bool		motion = false;
	GpLocus		locus;

	/* Every CteScan of the statement must name the one subplan of the CTE */
	if (!gp_slice_cte_agg_scan(stmt->planTree, &cteplanid))
		return false;
	foreach_ptr(Plan, sub, stmt->subplans)
		if (!gp_slice_cte_agg_scan(sub, &cteplanid))
			return false;
	if (cteplanid <= 0 || cteplanid > list_length(stmt->subplans))
		return false;

	/*
	 * The ORDER BY of the CTE leaves a sort inside the subplan, above the
	 * aggregate, and may add its keys to the columns of the segments.  The
	 * replacement is not made for that case: such a CTE stays on the
	 * coordinator.
	 */
	if (original->sortClause != NIL)
		return false;

	/* Find the aggregate of the subplan, below its sorts and limits */
	subplan = list_nth(stmt->subplans, cteplanid - 1);
	plan = subplan;
	while (plan != NULL && !IsA(plan, Agg))
	{
		if (plan->righttree != NULL)
			return false;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;
	agg = (Agg *) plan;

	/*
	 * A sorted aggregation hands the order of its groups up: the planner
	 * may plan a merge join above the scan of the CTE in the faith that the
	 * scan comes out sorted, and the answer of the segments arrives in no
	 * order.  Keep such a CTE on the coordinator
	 */
	if (agg->aggstrategy == AGG_SORTED || agg->aggstrategy == AGG_MIXED)
		return false;

	/* Below the aggregate there is the gather of the table and nothing else */
	below = agg->plan.lefttree;
	while (below != NULL && (IsA(below, Sort) || IsA(below, IncrementalSort)))
		below = below->lefttree;
	if (below == NULL || !gp_is_gather(below))
		return false;

	locus = gp_locus_plan(&agg->plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
	{
		/* A grouping that is not colocated can still move by its columns */
		if (!gp_slice_agg_locus(original, agg, below, &replicated, &motion, &locus))
			return false;
	}
	else
		replicated = locus.kind == GP_LOCUS_ONE_SEGMENT;

	/* The segments answer the whole CTE: one table, one aggregate */
	return gp_slice_cte_agg_replace(original, stmt, cteplanid, parent, agg,
									replicated, motion);
}

/*
 * Replace the aggregate of the CTE, in the subplan with the given number,
 * with a scan that runs it on the segments.  The query of the CTE carries
 * the pulls of its motion already: the caller has rewritten its table
 */
static bool
gp_slice_cte_agg_replace(Query *ctequery, PlannedStmt *stmt, int cteplanid,
						 Plan *parent, Agg *agg, bool replicated, bool motion)
{
	ListCell   *lc;
	Index		varno;
	bool		found = false;
	CustomScan *cscan;

	/* The columns the segments return are the target list of the CTE */
	if (list_length(agg->plan.targetlist) !=
		list_length(ctequery->targetList))
		return false;

	cscan = gp_slice_make(&agg->plan, ctequery, replicated, motion,
						  ctequery->sortClause);

	/*
	 * gp_slice_make keeps the target list of the query as the descriptor of
	 * what the segments return.  That list names its columns by the numbers
	 * of the rtable of the CTE, and a grouping column may name the grouping
	 * itself; EXPLAIN deparses the descriptor against the rtable of the
	 * whole statement.  Name every column as the column of the CTE that it
	 * is.
	 */
	varno = 0;
	foreach_node(RangeTblEntry, rte, stmt->rtable)
	{
		varno++;
		if (rte->rtekind == RTE_CTE)
		{
			found = true;
			break;
		}
	}
	if (!found)
		return false;
	foreach(lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *var = makeVar(varno, tle->resno,
									 exprType((Node *) tle->expr),
									 exprTypmod((Node *) tle->expr),
									 exprCollation((Node *) tle->expr), 0);

		tle->expr = (Expr *) var;
	}

	if (parent == NULL)
		lfirst(list_nth_cell(stmt->subplans, cteplanid - 1)) = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;

	return true;
}

/* A window query over one distributed table, with every partition local */
Query *
gp_slice_window_query(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasWindowFuncs || parse->hasAggs || parse->hasSubLinks ||
		parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace a WindowAgg whose partitions all fit their distribution key */
bool
gp_slice_window_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, IncrementalSort)))
	{
		if (IsA(plan, IncrementalSort))
		{
			Sort	   *full = makeNode(Sort);

			*full = ((IncrementalSort *) plan)->sort;
			full->plan.type = T_Sort;

			/* A distributed answer has no presorted input for IncrementalSort */
			full->plan.lefttree = plan->lefttree;
			if (parent != NULL)
				parent->lefttree = (Plan *) full;
			parent = (Plan *) full;
			plan = plan->lefttree;
			sort_above = true;
			continue;
		}
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || !IsA(plan, WindowAgg))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

/* A DISTINCT query is local when its output rows share one distribution key */
Query *
gp_slice_distinct_query(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->distinctClause == NIL || parse->hasWindowFuncs ||
		parse->hasSubLinks || parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace a Unique or a distinct aggregate whose keys are all local */
bool
gp_slice_distinct_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, Unique)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

static bool
gp_slice_grouping_key(Query *parse, const GpPolicy *policy, GroupingSet *gs)
{
	ListCell   *lc;

	if (gs->kind == GROUPING_SET_SETS)
	{
		/* GROUPING SETS ((a), (a)) has one extra list wrapper */
		foreach(lc, gs->content)
		{
			if (!gp_slice_grouping_key(parse, policy,
									   lfirst_node(GroupingSet, lc)))
				return false;
		}
		return true;
	}
	if (gs->kind != GROUPING_SET_SIMPLE || gs->content == NIL)
		return false;
	for (int k = 0; k < policy->nkeys; k++)
	{
		bool		found = false;

		foreach(lc, gs->content)
		{
			Index		ref = lfirst_int(lc);
			TargetEntry *tle = get_sortgroupref_tle(ref, parse->targetList);
			Var		   *var;

			if (!IsA(tle->expr, Var))
				return false;
			var = (Var *) tle->expr;
			if (var->varattno == policy->keys[k])
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}

/* Every grouping set must contain the whole distribution key and be nonempty */
static bool
gp_slice_grouping_keys(Query *parse, const GpPolicy *policy)
{
	ListCell   *lc;

	foreach(lc, parse->groupingSets)
	{
		if (!gp_slice_grouping_key(parse, policy,
								   lfirst_node(GroupingSet, lc)))
			return false;
	}
	return true;
}

/* A GROUPING SETS query whose every set keeps its groups on one segment */
Query *
gp_slice_grouping_query(Query *parse)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasAggs || parse->groupingSets == NIL ||
		parse->hasWindowFuncs || parse->hasSubLinks || parse->hasTargetSRFs ||
		parse->hasRowSecurity || parse->setOperations != NULL ||
		parse->cteList != NIL || parse->distinctClause != NIL ||
		parse->rowMarks != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return NULL;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex,
					 parse->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &policy) ||
		!gp_slice_grouping_keys(parse, &policy))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Replace an aggregate whose every grouping set covers the distribution key */
bool
gp_slice_grouping_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	Agg		   *agg;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || !IsA(plan, Agg))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;
	agg = (Agg *) plan;
	locus = gp_locus_plan(&agg->plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR)
		return false;
	if (list_length(agg->plan.targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(&agg->plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	stmt->parallelModeNeeded = false;
	return true;
}

/* A UNION ALL leaf reads one distributed table and nothing else */
static bool
gp_slice_union_leaf(Query *query)
{
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (query->commandType != CMD_SELECT || query->setOperations != NULL ||
		query->hasAggs || query->hasWindowFuncs || query->hasSubLinks ||
		query->hasTargetSRFs || query->hasRowSecurity || query->cteList != NIL ||
		query->groupClause != NIL || query->groupingSets != NIL ||
		query->distinctClause != NIL || query->rowMarks != NIL ||
		query->sortClause != NIL || query->limitCount != NULL ||
		query->limitOffset != NULL)
		return false;
	if (!IsA(query->jointree, FromExpr) ||
		list_length(query->jointree->fromlist) != 1 ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef) ||
		query->jointree->quals != NULL)
		return false;
	rte = rt_fetch(linitial_node(RangeTblRef, query->jointree->fromlist)->rtindex,
					 query->rtable);
	return rte->rtekind == RTE_RELATION && gp_policy_get(rte->relid, &policy);
}

/* Every operation of the tree must be UNION ALL over such a leaf */
static bool
gp_slice_union_tree(Query *parse, Node *node)
{
	SetOperationStmt *setop;
	RangeTblEntry *rte;

	if (IsA(node, RangeTblRef))
	{
		rte = rt_fetch(((RangeTblRef *) node)->rtindex, parse->rtable);
		return rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL &&
			gp_slice_union_leaf(rte->subquery);
	}
	if (!IsA(node, SetOperationStmt))
		return false;
	setop = (SetOperationStmt *) node;
	return (setop->op == SETOP_UNION || setop->op == SETOP_INTERSECT ||
			setop->op == SETOP_EXCEPT) &&
		gp_slice_union_tree(parse, setop->larg) &&
		gp_slice_union_tree(parse, setop->rarg);
}

Query *
gp_slice_union_query(Query *parse)
{
	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->setOperations == NULL || parse->hasAggs ||
		parse->hasWindowFuncs || parse->hasSubLinks || parse->hasTargetSRFs ||
		parse->hasRowSecurity || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!gp_slice_union_tree(parse, parse->setOperations))
		return NULL;
	return gp_slice_query_copy(parse);
}

/* Collect whether the tree mixes replicated and hashed leaves */
static void
gp_slice_union_mixed(Query *parse, Node *node, bool *saw_replicated,
					 bool *saw_hashed)
{
	SetOperationStmt *setop;
	RangeTblEntry *rte;
	GpPolicy	policy;

	if (IsA(node, RangeTblRef))
	{
		rte = rt_fetch(((RangeTblRef *) node)->rtindex, parse->rtable);
		if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
			return;
		rte = rt_fetch(linitial_node(RangeTblRef,
									 rte->subquery->jointree->fromlist)->rtindex,
						 rte->subquery->rtable);
		if (!gp_policy_get(rte->relid, &policy))
			return;
		if (policy.replicated)
			*saw_replicated = true;
		else
			*saw_hashed = true;
		return;
	}
	if (!IsA(node, SetOperationStmt))
		return;
	setop = (SetOperationStmt *) node;
	gp_slice_union_mixed(parse, setop->larg, saw_replicated, saw_hashed);
	gp_slice_union_mixed(parse, setop->rarg, saw_replicated, saw_hashed);
}

/* Replace a compatible Append by one scan of its whole UNION ALL */
bool
gp_slice_union_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	bool		distinct = !((SetOperationStmt *) original->setOperations)->all;
	GpLocus	locus;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, Result) || IsA(plan, Gather) ||
							(distinct && (IsA(plan, Unique) || IsA(plan, Agg)))))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		/* A parallel Gather is not kept above a distributed union */
		if (!IsA(plan, Gather))
			parent = plan;
		plan = plan->lefttree;
	}
	if (IsA(plan, SetOp))
	{
		/* HashSetOp has two inputs, not an Append: replace the whole node */
		locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
		if (locus.kind == GP_LOCUS_COORDINATOR)
			return false;
		cscan = gp_slice_make(plan, original,
							  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
							  original->sortClause);
		if (parent == NULL)
			stmt->planTree = &cscan->scan.plan;
		else
			parent->lefttree = &cscan->scan.plan;
		stmt->parallelModeNeeded = false;
		return true;
	}
	if (plan == NULL || !IsA(plan, Append))
		return false;
	if (original->sortClause != NIL && !sort_above)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (locus.kind == GP_LOCUS_COORDINATOR && !distinct)
	{
		bool		saw_replicated = false;
		bool		saw_hashed = false;

		/* Different hash maps have no common locus, but no duplicates either */
		gp_slice_union_mixed(original, original->setOperations,
							 &saw_replicated, &saw_hashed);
		if (saw_replicated && saw_hashed)
			return false;
	}
	if (locus.kind == GP_LOCUS_COORDINATOR && distinct)
		/* DISTINCT needs one hashed place for every equal output row */
		return false;
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;
	cscan = gp_slice_make(plan, original,
						  locus.kind == GP_LOCUS_ONE_SEGMENT, false,
						  original->sortClause);
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	/* No PostgreSQL parallel workers remain above the distributed union */
	stmt->parallelModeNeeded = false;
	return true;
}

/*
 * A scan that runs a query on the segments: its target list is that of the
 * plan it replaces, and its SQL is the query without its sort and limit
 */
static CustomScan *
gp_slice_make(Plan *src, Query *original, bool replicated, bool motion,
			  List *sortClause)
{
	Query	   *remote = copyObject(original);
	CustomScan *cscan = makeNode(CustomScan);
	List	   *srctlist = src->targetlist;
	List	   *origtlist = original->targetList;
	List	   *tlist = NIL;
	ListCell   *lc;
	int			pos = 0;

	/*
	 * A junk column is a grouping or a sort key that is not selected.  With
	 * no ORDER BY it is not needed by anything above, so it is dropped from
	 * both lists.  With an ORDER BY it is needed by the sort, so it goes to
	 * the segments as a column of its own.
	 */
	if (sortClause == NIL)
	{
		List	   *srcnonjunk = NIL;
		List	   *orignojunk = NIL;

		foreach(lc, srctlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (!tle->resjunk)
				srcnonjunk = lappend(srcnonjunk, tle);
		}
		foreach(lc, origtlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (!tle->resjunk)
				orignojunk = lappend(orignojunk, tle);
		}
		srctlist = srcnonjunk;
		origtlist = orignojunk;
	}
	if (sortClause != NIL)
	{
		foreach(lc, remote->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			tle->resjunk = false;
		}
	}
	remote->sortClause = NIL;
	remote->limitCount = NULL;
	remote->limitOffset = NULL;

	foreach(lc, srctlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *out;
		Var		   *var;

		pos++;
		var = makeVar(INDEX_VAR, pos,
					  exprType((Node *) tle->expr),
					  exprTypmod((Node *) tle->expr),
					  exprCollation((Node *) tle->expr), 0);
		out = makeTargetEntry((Expr *) var, tle->resno,
							  tle->resname ? pstrdup(tle->resname) : NULL,
							  false);
		out->ressortgroupref = tle->ressortgroupref;
		tlist = lappend(tlist, out);
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.startup_cost = src->startup_cost;
	cscan->scan.plan.total_cost = src->total_cost;
	cscan->scan.plan.plan_rows = src->plan_rows;
	cscan->scan.plan.plan_width = src->plan_width;
	cscan->custom_scan_tlist = copyObject(origtlist);
	cscan->custom_private = list_make4(makeString(pg_get_querydef(remote, false)),
									   makeBoolean(replicated),
									   makeBoolean(motion),
									   makeBoolean(gp_slice_autonomous(remote)));
	cscan->methods = &gp_slice_plan_methods;
	return cscan;
}

typedef struct GpJoinTable
{
	Index		rti;
	Query	   *query;			/* query level that owns rti */
	int			depth;			/* nesting level of that query */
	GpPolicy	policy;
} GpJoinTable;

/*
 * Whether the condition equates the two given columns of the two tables,
 * at the top of an AND or below it, with each Var at its own query level
 */
static bool
gp_slice_join_var_owned(Var *var, Query *condquery, int cond_depth,
						List *ancestors, GpJoinTable *table, AttrNumber key)
{
	if (var->varno != table->rti || var->varattno != key)
		return false;
	if (var->varlevelsup == 0)
		return table->query == condquery;
	if (table->query == condquery ||
		var->varlevelsup != cond_depth - table->depth ||
		var->varlevelsup > list_length(ancestors))
		return false;
	return list_nth(ancestors, cond_depth - var->varlevelsup) == table->query;
}

/* Whether any of the join conditions equates the two columns */

static bool
gp_slice_join_pair(Node *qual, Query *condquery, int cond_depth,
				   List *ancestors, GpJoinTable *table1, GpJoinTable *table2,
				   AttrNumber k1, AttrNumber k2)
{
	OpExpr	   *op;
	Var		   *var1;
	Var		   *var2;

	if (qual == NULL)
		return false;
	if (IsA(qual, BoolExpr) && ((BoolExpr *) qual)->boolop == AND_EXPR)
	{
		ListCell   *lc;

		foreach(lc, ((BoolExpr *) qual)->args)
		{
			if (gp_slice_join_pair(lfirst(lc), condquery, cond_depth, ancestors,
								   table1, table2, k1, k2))
				return true;
		}
		return false;
	}
	if (!IsA(qual, OpExpr))
		return false;
	op = (OpExpr *) qual;
	if (list_length(op->args) != 2 ||
		get_opname(op->opno) == NULL ||
		strcmp(get_opname(op->opno), "=") != 0)
		return false;
	if (!IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return false;
	var1 = (Var *) linitial(op->args);
	var2 = (Var *) lsecond(op->args);

	return (gp_slice_join_var_owned(var1, condquery, cond_depth, ancestors, table1, k1) &&
			gp_slice_join_var_owned(var2, condquery, cond_depth, ancestors, table2, k2)) ||
		(gp_slice_join_var_owned(var2, condquery, cond_depth, ancestors, table1, k1) &&
		 gp_slice_join_var_owned(var1, condquery, cond_depth, ancestors, table2, k2));
}

/* A join condition and the query level that owns its RT indexes */
typedef struct GpJoinCond
{
	Query	   *query;
	Node	   *qual;
	int			depth;
	List	   *ancestors;		/* query levels above this condition */
} GpJoinCond;

/* Whether any of the join conditions equates the two columns */
static bool
gp_slice_join_pair_list(List *conds, GpJoinTable *table1,
						GpJoinTable *table2, AttrNumber k1, AttrNumber k2)
{
	ListCell   *lc;

	foreach(lc, conds)
	{
		GpJoinCond *cond = lfirst(lc);

		/* Do not confuse equal RT indexes of unrelated query levels */
		if (cond->query != table1->query && cond->query != table2->query)
			continue;
		if (gp_slice_join_pair(cond->qual, cond->query, cond->depth,
							   cond->ancestors, table1, table2, k1, k2))
			return true;
	}
	return false;
}

static bool gp_slice_join_from_depth(Query *parse, int depth, List *ancestors,
									 List **tables, List **conds);

/*
 * Collect the base tables of a join tree and all its join conditions;
 * false when a leaf is not a base table or a join type is not supported
 */
static bool
gp_slice_join_collect(Node *node, Query *query, int depth, List *ancestors,
					  List **tables, List **conds)
{
	JoinExpr   *join;

	if (IsA(node, RangeTblRef))
	{
		Index		rti = ((RangeTblRef *) node)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, query->rtable);
		GpJoinTable *table;

		if (rte->rtekind == RTE_SUBQUERY)
		{
			List	   *nested;

			/* Keep the ownership and the ancestry of one derived FROM level */
			if (depth > 0 || rte->subquery == NULL)
				return false;
			nested = lappend(list_copy(ancestors), query);
			return gp_slice_join_from_depth(rte->subquery, depth + 1,
											nested, tables, conds);
		}
		table = palloc0(sizeof(GpJoinTable));
		table->rti = rti;
		table->query = query;
		table->depth = depth;
		*tables = lappend(*tables, table);
		return true;
	}
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	if (join->jointype != JOIN_INNER && join->jointype != JOIN_LEFT &&
		join->jointype != JOIN_RIGHT && join->jointype != JOIN_FULL)
		return false;
	if (join->quals != NULL)
	{
		GpJoinCond *cond = palloc0(sizeof(GpJoinCond));

		cond->query = query;
		cond->qual = join->quals;
		cond->depth = depth;
		cond->ancestors = ancestors;
		*conds = lappend(*conds, cond);
	}
	return gp_slice_join_collect(join->larg, query, depth, ancestors, tables, conds) &&
		gp_slice_join_collect(join->rarg, query, depth, ancestors, tables, conds);
}

/* A comma-separated FROM list is a set of inner joins too */
static bool
gp_slice_join_from_depth(Query *parse, int depth, List *ancestors,
						 List **tables, List **conds)
{
	ListCell   *lc;

	if (!IsA(parse->jointree, FromExpr))
		return false;
	foreach(lc, parse->jointree->fromlist)
	{
		if (!gp_slice_join_collect(lfirst(lc), parse, depth, ancestors, tables, conds))
			return false;
	}
	if (parse->jointree->quals != NULL)
	{
		GpJoinCond *cond = palloc0(sizeof(GpJoinCond));

		cond->query = parse;
		cond->qual = parse->jointree->quals;
		cond->depth = depth;
		cond->ancestors = ancestors;
		*conds = lappend(*conds, cond);
	}
	return true;
}

static bool
gp_slice_join_from(Query *parse, List **tables, List **conds)
{
	return gp_slice_join_from_depth(parse, 0, NIL, tables, conds);
}

typedef struct GpJoinSubqueries
{
	List	  **tables;
	List	  **conds;
	List	   *ancestors;
	int			depth;
	bool		ok;
} GpJoinSubqueries;

/* Collect every correlated level, keeping the owner of each RT index */
static bool
gp_slice_join_sublink_walker(Node *node, GpJoinSubqueries *state)
{
	SubLink    *sublink;
	Query	   *subquery;

	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		GpJoinSubqueries nested = *state;

		/* A subquery in FROM is a level too, not only a SubLink is one */
		nested.depth = state->depth + 1;
		nested.ancestors = lappend(list_copy(state->ancestors), node);
		return query_tree_walker((Query *) node, gp_slice_join_sublink_walker,
							 &nested, 0);
	}
	if (!IsA(node, SubLink))
		return expression_tree_walker(node, gp_slice_join_sublink_walker, state);
	sublink = (SubLink *) node;
	if (!IsA(sublink->subselect, Query))
	{
		state->ok = false;
		return true;
	}
	subquery = (Query *) sublink->subselect;
	if (!gp_slice_join_from_depth(subquery, state->depth, state->ancestors,
								  state->tables, state->conds))
	{
		state->ok = false;
		return true;
	}
	{
		GpJoinSubqueries nested = *state;

		nested.depth = state->depth + 1;
		nested.ancestors = lappend(list_copy(state->ancestors), subquery);
		return query_tree_walker(subquery, gp_slice_join_sublink_walker,
								 &nested, 0);
	}
}

static bool
gp_slice_join_sublinks(Query *parse, List **tables, List **conds)
{
	GpJoinSubqueries state =
	{
		.tables = tables,
		.conds = conds,
		.ancestors = list_make1(parse),
		.depth = 1,
		.ok = true
	};
	ListCell   *lc;

	(void) query_tree_walker(parse, gp_slice_join_sublink_walker, &state, 0);

	/*
	 * The sublinks of a derived table of the fromlist drive the join of the
	 * subquery below the way the sublinks of the query drive its own.  The
	 * walker of the query misses the subqueries of the range table, so each
	 * of them is walked as the level one below
	 */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		GpJoinSubqueries nested = state;

		if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
			continue;
		nested.depth = state.depth + 1;
		nested.ancestors = lappend(list_copy(state.ancestors), rte->subquery);
		(void) query_tree_walker(rte->subquery, gp_slice_join_sublink_walker,
								 &nested, 0);
	}
	return state.ok;
}

/* Load the policies needed to build a motion, independently of its locus */
static bool
gp_slice_join_load_policies(Query *parse, List *tables)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		RangeTblEntry *rte = rt_fetch(table->rti, table->query->rtable);

		if (rte->rtekind != RTE_RELATION ||
			!gp_policy_get(rte->relid, &table->policy))
			return false;
	}
	return true;
}

/*
 * Load the policies of the tables of a join and check that every row of
 * the join is on one segment: all hashed tables are of one colocation
 * group, and every one of them is joined to the others by the whole key,
 * while a replicated table is on every segment already
 */
static bool
gp_slice_join_colocated(Query *parse, List *tables, List *conds)
{
	ListCell   *lc;
	int			colocation_id = 0;
	int			nkeys = 0;
	bool		any_hash = false;
	List	   *connected = NIL;
	List	   *pending = NIL;

	if (!gp_slice_join_load_policies(parse, tables))
		return false;

	/* One colocation group for all hashed tables, if they are colocated */
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table->policy.replicated)
			continue;
		any_hash = true;
		if (colocation_id == 0)
		{
			colocation_id = table->policy.colocation_id;
			nkeys = table->policy.nkeys;
		}
		if (table->policy.colocation_id != colocation_id ||
			table->policy.nkeys != nkeys || colocation_id == 0)
			return false;
	}
	if (!any_hash)
		return true;

	/* Grow the connected set of hashed tables by key pairs of the joins */
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
			pending = lappend(pending, table);
	}
	if (pending != NIL)
	{
		GpJoinTable *first = linitial(pending);

		connected = list_make1(first);
		pending = list_delete(pending, first);
	}
	for (;;)
	{
		bool		grew = false;
		ListCell   *lp;

		foreach(lp, pending)
		{
			GpJoinTable *table = lfirst(lp);
			ListCell   *lu;

			foreach(lu, connected)
			{
				GpJoinTable *other = lfirst(lu);
				bool		all_keys = true;

				for (int i = 0; i < nkeys; i++)
				{
					if (!gp_slice_join_pair_list(conds, table, other,
												 table->policy.keys[i],
												 other->policy.keys[i]))
					{
						all_keys = false;
						break;
					}
				}
				if (all_keys)
				{
					connected = lappend(connected, table);
					pending = list_delete(pending, table);
					grew = true;
					break;
				}
			}
			if (grew)
				break;
		}
		if (!grew)
			return pending == NIL;
	}
}

/*
 * Whether an inner join of hashed tables of different colocation groups is
 * connected by the whole keys of all of them.  The first table stays; every
 * table of another group moves by the hash of its key under its map.  The
 * connected join graph proves that every row meets all its partners on one
 * segment, even when the key equalities form a chain rather than name the
 * first table directly.  The table that stays and the tables that move are
 * returned
 */
static bool
gp_slice_join_redistributable(Query *parse, List *tables, List *conds,
							  GpJoinTable **stay, List **moves)
{
	GpJoinTable *first;
	Relation	rel1;
	List	   *connected;
	List	   *pending = NIL;
	ListCell   *lc;
	int			nkeys;
	bool		any_move = false;

	*moves = NIL;
	if (list_length(tables) < 2)
		return false;
	first = linitial(tables);
	if (first->policy.replicated || first->policy.colocation_id == 0 ||
		first->policy.nkeys == 0)
		return false;
	nkeys = first->policy.nkeys;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table->policy.replicated || table->policy.colocation_id == 0 ||
			table->policy.nkeys != nkeys)
			return false;
		if (table != first)
		{
			pending = lappend(pending, table);
			if (table->policy.colocation_id != first->policy.colocation_id)
				any_move = true;
		}
	}
	if (!any_move)
		return false;

	/* The key columns must hash to one place, so they are of one type */
	rel1 = table_open(rt_fetch(first->rti, first->query->rtable)->relid,
					  AccessShareLock);
	foreach(lc, pending)
	{
		GpJoinTable *table = lfirst(lc);
		Relation	rel2 = table_open(rt_fetch(table->rti, table->query->rtable)->relid,
									AccessShareLock);
		bool		ok = true;

		for (int i = 0; i < nkeys; i++)
		{
			if (TupleDescAttr(RelationGetDescr(rel1), first->policy.keys[i] - 1)->atttypid !=
				TupleDescAttr(RelationGetDescr(rel2), table->policy.keys[i] - 1)->atttypid)
			{
				ok = false;
				break;
			}
		}
		table_close(rel2, AccessShareLock);
		if (!ok)
		{
			table_close(rel1, AccessShareLock);
			return false;
		}
	}
	table_close(rel1, AccessShareLock);

	/* Grow the connected set by key pairs, regardless of colocation group */
	connected = list_make1(first);
	for (;;)
	{
		bool		grew = false;
		ListCell   *lp;

		foreach(lp, pending)
		{
			GpJoinTable *table = lfirst(lp);
			ListCell   *lu;

			foreach(lu, connected)
			{
				GpJoinTable *other = lfirst(lu);
				bool		all_keys = true;

				for (int i = 0; i < nkeys; i++)
				{
					if (!gp_slice_join_pair_list(conds, table, other,
											 table->policy.keys[i],
											 other->policy.keys[i]))
					{
						all_keys = false;
						break;
					}
				}
				if (all_keys)
				{
					connected = lappend(connected, table);
					pending = list_delete(pending, table);
					grew = true;
					break;
				}
			}
			if (grew)
				break;
		}
		if (!grew)
		{
			if (pending != NIL)
				return false;
			break;
		}
	}

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table->policy.colocation_id != first->policy.colocation_id)
			*moves = lappend(*moves, table);
	}
	*stay = first;
	return true;
}

/* Whether any join of the tree is a full one */
static bool
gp_slice_join_has_full(Node *node)
{
	JoinExpr   *join;

	if (node == NULL || !IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	if (join->jointype == JOIN_FULL)
		return true;
	return gp_slice_join_has_full(join->larg) ||
		gp_slice_join_has_full(join->rarg);
}

/* Whether a derived input joins its tables by a full join */
static bool
gp_slice_join_derived_full(Query *parse)
{
	foreach_node(RangeTblEntry, rte, parse->rtable)
	{
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL &&
			IsA(rte->subquery->jointree, FromExpr) &&
			gp_slice_join_has_full((Node *) rte->subquery->jointree->fromlist))
			return true;
	}
	return false;
}

/* Whether the Var belongs to the table at this query level */
static bool
gp_slice_join_var_of(Var *var, GpJoinTable *table, Query *query)
{
	return var->varlevelsup == 0 && var->varattno > 0 &&
		var->varno == table->rti && table->query == query;
}

/*
 * Whether a condition equates a column of the stay table with a column of
 * the move table, and return that column of the move table
 */
static bool
gp_slice_join_move_key(Node *node, Query *query, GpJoinTable *stay,
					   GpJoinTable *move, AttrNumber *move_attno)
{
	OpExpr	   *op;
	Var		   *var1;
	Var		   *var2;
	ListCell   *lc;

	if (node == NULL)
		return false;
	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == AND_EXPR)
	{
		foreach(lc, ((BoolExpr *) node)->args)
			if (gp_slice_join_move_key(lfirst(lc), query, stay, move,
									   move_attno))
				return true;
		return false;
	}
	if (!IsA(node, OpExpr))
		return false;
	op = (OpExpr *) node;
	if (list_length(op->args) != 2 ||
		get_opname(op->opno) == NULL ||
		strcmp(get_opname(op->opno), "=") != 0 ||
		!IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return false;
	var1 = (Var *) linitial(op->args);
	var2 = (Var *) lsecond(op->args);
	if (gp_slice_join_var_of(var1, stay, query) &&
		gp_slice_join_var_of(var2, move, query))
	{
		*move_attno = var2->varattno;
		return true;
	}
	if (gp_slice_join_var_of(var2, stay, query) &&
		gp_slice_join_var_of(var1, move, query))
	{
		*move_attno = var1->varattno;
		return true;
	}
	return false;
}

/* A colocated component of a join graph, ignoring all other tables */
static List *
gp_slice_join_component(List *tables, List *conds, GpJoinTable *root)
{
	List	   *connected = list_make1(root);
	List	   *pending = NIL;
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table != root && !table->policy.replicated)
			pending = lappend(pending, table);
	}
	for (;;)
	{
		bool		grew = false;

		foreach(lc, pending)
		{
			GpJoinTable *table = lfirst(lc);
			ListCell   *other_lc;

			foreach(other_lc, connected)
			{
				GpJoinTable *other = lfirst(other_lc);
				bool		all_keys = table->policy.nkeys == other->policy.nkeys &&
					table->policy.colocation_id == other->policy.colocation_id;

				for (int i = 0; all_keys && i < table->policy.nkeys; i++)
				{
					if (!gp_slice_join_pair_list(conds, table, other,
												 table->policy.keys[i],
												 other->policy.keys[i]))
						all_keys = false;
				}
				if (all_keys)
				{
					connected = lappend(connected, table);
					pending = list_delete(pending, table);
					grew = true;
					break;
				}
			}
			if (grew)
				break;
		}
		if (!grew)
			break;
	}
	return connected;
}

/* Choose a stationary component and broadcast every other hashed table */
static bool
gp_slice_join_broadcast_component(Query *original, List *tables, List *conds,
								  GpJoinTable **stay, List **moves)
{
	List	   *best = NIL;
	List	   *best_moves = NIL;
	double		best_rows = 0;
	int			best_count = 0;
	bool		best_known = false;
	ListCell   *lc;

	*moves = NIL;
	foreach(lc, tables)
	{
		GpJoinTable *root = lfirst(lc);
		List	   *component;
		List	   *other_moves = NIL;
		ListCell   *it;
		double		rows = 0;
		bool		known = true;
		int			count;

		if (root->policy.replicated)
			continue;
		component = gp_slice_join_component(tables, conds, root);
		foreach(it, tables)
		{
			GpJoinTable *table = lfirst(it);
			double		table_rows;

			if (table->policy.replicated || list_member_ptr(component, table))
				continue;
			other_moves = lappend(other_moves, table);
			if (known && gp_slice_join_rows(original, table, &table_rows))
				rows += table_rows;
			else
				known = false;
		}
		if (other_moves == NIL)
			continue;
		count = list_length(other_moves);
		if (best == NIL ||
			(known && (!best_known || rows < best_rows)) ||
			(!known && !best_known && count < best_count))
		{
			best = component;
			best_moves = other_moves;
			best_rows = rows;
			best_count = count;
			best_known = known;
		}
	}
	if (best == NIL)
		return false;
	*stay = linitial(best);
	*moves = best_moves;
	return true;
}

/* Whether two hashed tables can join by broadcasting one of them */
static bool
gp_slice_join_broadcastable(List *tables)
{
	GpJoinTable *t1;
	GpJoinTable *t2;

	if (list_length(tables) != 2)
		return false;
	t1 = linitial(tables);
	t2 = lsecond(tables);
	return !t1->policy.replicated && !t2->policy.replicated &&
		t1->policy.colocation_id != 0 && t2->policy.colocation_id != 0;
}

/* The imported row count of a table, if ANALYZE has provided one */
static bool
gp_slice_join_rows(Query *parse, GpJoinTable *table, double *rows)
{
	Relation	rel = table_open(rt_fetch(table->rti, table->query->rtable)->relid,
							 NoLock);

	if (rel->rd_rel->reltuples < 0)
	{
		table_close(rel, NoLock);
		return false;
	}
	*rows = rel->rd_rel->reltuples;
	table_close(rel, NoLock);
	return true;
}

/*
 * The kind of the first join of the query: an inner join, or which side of
 * it is kept even when it has no partner
 */
static JoinType
gp_slice_join_type_of(Query *parse)
{
	Node	   *first;

	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1)
		return JOIN_INNER;
	first = linitial(parse->jointree->fromlist);
	if (!IsA(first, JoinExpr))
		return JOIN_INNER;
	return ((JoinExpr *) first)->jointype;
}

/*
 * Rewrite the table that moves into a pull of its rows from every segment:
 * the segment runs the query, and the function asks every segment — itself
 * among them — for the rows.  An inner join needs only the rows that the
 * hash of the join key places on the asking segment.  An outer join broadcasts
 * the moving side instead, so that rows with null keys and rows without a
 * partner are kept.  The map of the table that stays goes into the query as
 * an array, and the segment the rows are for comes in a setting of the
 * connection, so the serialized plan is one for all the segments
 */
static bool
gp_slice_redistribute_rte_keys(Query *remote, GpJoinTable *stay, GpJoinTable *move,
						 bool broadcast, int nkeys, const AttrNumber *keyatts,
							 const Bitmapset *needed,
							 bool default_collation)
{
	RangeTblEntry *rte;
	Relation	rel;
	TupleDesc	tupdesc;
	StringInfoData sql;
	StringInfoData cols;
	StringInfoData map;
	StringInfoData keys;
	List	   *coltypes = NIL;
	List	   *coltypmods = NIL;
	List	   *colnames = NIL;
	List	   *colcolls = NIL;
	RangeTblFunction *rtf;
	FuncExpr   *func;
	Const	   *arg;
	Oid			peer_query_oid;

	/* Equal RT indexes of different query levels name different tables */
	if (move->query != NULL)
		remote = move->query;
	rte = rt_fetch(move->rti, remote->rtable);
	rel = table_open(rte->relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	initStringInfo(&sql);
	initStringInfo(&cols);
	initStringInfo(&keys);
	initStringInfo(&map);
	appendStringInfoString(&map, "ARRAY[");
	for (int i = 0; i < stay->policy.nbuckets; i++)
		appendStringInfo(&map, "%s%d", i > 0 ? "," : "",
						 stay->policy.map[i]);
	appendStringInfoString(&map, "]");
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
		{
			table_close(rel, AccessShareLock);
			return false;
		}
		if (needed != NULL && !bms_is_member(att->attnum, needed))
			appendStringInfo(&cols, "%sNULL AS %s", cols.len > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
		else
			appendStringInfo(&cols, "%s%s", cols.len > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)));
		coltypes = lappend_oid(coltypes, att->atttypid);
		coltypmods = lappend_int(coltypmods, att->atttypmod);
		colnames = lappend(colnames, makeString(pstrdup(NameStr(att->attname))));
		colcolls = lappend_oid(colcolls, att->attcollation);
	}
	for (int i = 0; i < nkeys; i++)
		appendStringInfo(&keys, "%s%s%s", keys.len > 0 ? ", " : "",
						 quote_identifier(NameStr(TupleDescAttr(tupdesc,
																keyatts[i] - 1)->attname)),
						 default_collation &&
						 TupleDescAttr(tupdesc, keyatts[i] - 1)->attcollation != InvalidOid ?
						 " COLLATE pg_catalog.default" : "");

	if (broadcast)
	{
		appendStringInfo(&sql, "SELECT %s FROM %s%s",
						 cols.data,
						 rte->inh ? "" : "ONLY ",
						 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
													RelationGetRelationName(rel)));
	}
	else
	{
		appendStringInfo(&sql, "SELECT %s FROM %s%s WHERE (%s)[gp.bucket(%d%s%s) + 1] OPERATOR(pg_catalog.=) current_setting('gp.requester_segment')::int4",
						 cols.data,
						 rte->inh ? "" : "ONLY ",
						 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
													RelationGetRelationName(rel)),
						 map.data, stay->policy.nbuckets,
						 keys.len > 0 ? ", " : "", keys.data);
	}
	table_close(rel, AccessShareLock);

	peer_query_oid = DatumGetObjectId(DirectFunctionCall1(regprocedurein,
														  CStringGetDatum("gp.peer_query(text)")));
	arg = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
					PointerGetDatum(cstring_to_text(gp_interconnect_plan(sql.data))), false, false);
	func = makeNode(FuncExpr);
	func->funcid = peer_query_oid;
	func->funcresulttype = RECORDOID;
	func->funcretset = true;
	func->funcvariadic = false;
	func->funcformat = COERCE_EXPLICIT_CALL;
	func->funccollid = InvalidOid;
	func->inputcollid = DEFAULT_COLLATION_OID;
	func->args = list_make1(arg);

	rtf = makeNode(RangeTblFunction);
	rtf->funccolcount = list_length(coltypes);
	rtf->funccoltypes = coltypes;
	rtf->funccoltypmods = coltypmods;
	rtf->funccolnames = colnames;
	rtf->funccolcollations = colcolls;
	rtf->funcexpr = (Node *) func;

	rte->rtekind = RTE_FUNCTION;
	rte->functions = list_make1(rtf);
	return true;
}

/* Redistribute a join side by its distribution key */
static bool
gp_slice_redistribute_rte(Query *remote, GpJoinTable *stay, GpJoinTable *move,
						 bool broadcast)
{
	return gp_slice_redistribute_rte_keys(remote, stay, move, broadcast,
										 move->policy.nkeys, move->policy.keys, NULL,
										 false);
}

/* Whether gp.bucket() can hash a grouping column of this type */
static bool
gp_slice_hashable(Oid typid)
{
	Oid			opclass = GetDefaultOpClass(typid, HASH_AM_OID);
	Oid			opcintype;
	Oid			proc;

	if (!OidIsValid(opclass))
		return false;
	opcintype = get_opclass_input_type(opclass);
	proc = get_opfamily_proc(get_opclass_family(opclass), opcintype, opcintype,
							 HASHEXTENDED_PROC);
	return OidIsValid(proc);
}

/* Whether the tree contains a SubLink, descending into queries */
static bool
gp_slice_cte_join_has_sublink(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
		return true;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, gp_slice_cte_join_has_sublink,
								 context, 0);
	return expression_tree_walker(node, gp_slice_cte_join_has_sublink, context);
}

/* The number of the SubLinks of the tree */
static bool
gp_slice_cte_join_sublink_count(Node *node, void *context)
{
	int		   *count = context;

	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		(*count)++;
		return true;
	}
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, gp_slice_cte_join_sublink_count,
								 context, 0);
	return expression_tree_walker(node, gp_slice_cte_join_sublink_count, context);
}

/* The subselect of a sublink is an aggregate over the CTE of the query alone */
static bool
gp_slice_cte_join_sublink_ok(SubLink *sub, Query *parse)
{
	Query	   *q;
	RangeTblEntry *rte;
	CommonTableExpr *cte;

	if (sub->subLinkType != EXPR_SUBLINK || sub->testexpr != NULL ||
		!IsA(sub->subselect, Query))
		return false;
	q = (Query *) sub->subselect;
	if (q->commandType != CMD_SELECT || !q->hasAggs ||
		q->hasWindowFuncs || q->hasSubLinks || q->hasTargetSRFs ||
		q->hasRowSecurity || q->setOperations != NULL ||
		q->cteList != NIL || q->groupClause != NIL || q->groupingSets != NIL ||
		q->distinctClause != NIL || q->rowMarks != NIL ||
		q->sortClause != NIL || q->limitCount != NULL || q->limitOffset != NULL ||
		q->havingQual != NULL)
		return false;
	if (!IsA(q->jointree, FromExpr) ||
		list_length(((FromExpr *) q->jointree)->fromlist) != 1 ||
		!IsA(linitial(((FromExpr *) q->jointree)->fromlist), RangeTblRef) ||
		q->jointree->quals != NULL)
		return false;
	rte = rt_fetch(linitial_node(RangeTblRef,
								 ((FromExpr *) q->jointree)->fromlist)->rtindex,
				   q->rtable);
	if (rte->rtekind != RTE_CTE || list_length(parse->cteList) != 1)
		return false;
	cte = linitial_node(CommonTableExpr, parse->cteList);
	if (strcmp(rte->ctename, cte->ctename) != 0)
		return false;
	foreach_node(TargetEntry, tle, q->targetList)
		if (!tle->resjunk && !IsA(tle->expr, Aggref))
			return false;
	return true;
}

/*
 * Whether a branch of the quals keeps the join whole: either it has no
 * sublink at all, or it equates a column of the CTE with a plain
 * aggregate over the CTE alone
 */
static bool
gp_slice_cte_join_branch_ok(Node *node, Query *parse, Index cterti)
{
	ListCell   *lc;

	if (node == NULL)
		return true;
	if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == AND_EXPR)
	{
		foreach(lc, ((BoolExpr *) node)->args)
			if (!gp_slice_cte_join_branch_ok(lfirst(lc), parse, cterti))
				return false;
		return true;
	}
	if (!IsA(node, OpExpr))
		return !gp_slice_cte_join_has_sublink(node, NULL);
	if (!gp_slice_cte_join_has_sublink(node, NULL))
		return true;
	if (list_length(((OpExpr *) node)->args) != 2)
		return false;
	{
		SubLink   *sub = NULL;
		int			nvars = 0;

		foreach(lc, ((OpExpr *) node)->args)
		{
			Node	   *arg = lfirst(lc);

			if (IsA(arg, SubLink))
			{
				if (sub != NULL)
					return false;
				sub = (SubLink *) arg;
			}
			else if (IsA(arg, Var) && ((Var *) arg)->varno == cterti &&
					 ((Var *) arg)->varlevelsup == 0)
			{
				if (nvars > 0)
					return false;
				nvars++;
			}
			else
				return false;
		}
		return sub != NULL && gp_slice_cte_join_sublink_ok(sub, parse);
	}
}

/*
 * Every sublink of the query sits in the quals of the join tree alone, as
 * a comparison with an aggregate over the CTE: such a sublink becomes an
 * InitPlan, whose answer the query above keeps in a Param
 */
static bool
gp_slice_cte_join_sublinks_ok(Query *parse, Index cterti)
{
	FromExpr   *from;
	ListCell   *lc;
	int			nall = 0;
	int			nquals = 0;

	if (!parse->hasSubLinks)
		return true;
	if (!IsA(parse->jointree, FromExpr))
		return false;
	from = (FromExpr *) parse->jointree;
	if (from->quals == NULL ||
		!gp_slice_cte_join_branch_ok(from->quals, parse, cterti))
		return false;
	foreach(lc, from->fromlist)
		if (IsA(lfirst(lc), JoinExpr) &&
			((JoinExpr *) lfirst(lc))->quals != NULL &&
			gp_slice_cte_join_has_sublink(((JoinExpr *) lfirst(lc))->quals,
										  NULL))
			return false;
	(void) gp_slice_cte_join_sublink_count((Node *) parse, &nall);
	(void) gp_slice_cte_join_sublink_count(from->quals, &nquals);
	return nall == nquals;
}

/* Drop the sublink branches of the quals: the answer above replaces them */
static Node *
gp_slice_cte_join_strip(Node *quals)
{
	List	   *args = NIL;
	ListCell   *lc;

	if (quals == NULL)
		return NULL;
	if (IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR)
	{
		bool		changed = false;

		foreach(lc, ((BoolExpr *) quals)->args)
		{
			Node	   *arg = lfirst(lc);

			if (gp_slice_cte_join_has_sublink(arg, NULL))
				changed = true;
			else
				args = lappend(args, arg);
		}
		if (!changed)
			return quals;
		if (args == NIL)
			return NULL;
		if (list_length(args) == 1)
			return linitial(args);
		return (Node *) makeBoolExpr(AND_EXPR, args, -1);
	}
	if (gp_slice_cte_join_has_sublink(quals, NULL))
		return NULL;
	return quals;
}

/* The number of the Params of an expression tree */
static bool
gp_slice_cte_join_param_count_exprs(Node *node, void *context)
{
	int		   *count = context;

	if (node == NULL)
		return false;
	if (IsA(node, Param))
		(*count)++;
	return expression_tree_walker(node, gp_slice_cte_join_param_count_exprs,
								  context);
}

/* The number of the Params of the expressions of a plan tree */
static void
gp_slice_cte_join_param_count(Plan *plan, int *count)
{
	if (plan == NULL)
		return;
	(void) gp_slice_cte_join_param_count_exprs((Node *) plan->targetlist, count);
	(void) gp_slice_cte_join_param_count_exprs((Node *) plan->qual, count);
	gp_slice_cte_join_param_count(plan->lefttree, count);
	gp_slice_cte_join_param_count(plan->righttree, count);
	if (IsA(plan, SubqueryScan))
		gp_slice_cte_join_param_count(((SubqueryScan *) plan)->subplan, count);
	if (IsA(plan, Append))
	{
		foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
			gp_slice_cte_join_param_count(child, count);
	}
	if (IsA(plan, MergeAppend))
	{
		foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
			gp_slice_cte_join_param_count(child, count);
	}
}

/* Whether an expression tree keeps a SubPlan: a correlated sublink */
static bool
gp_slice_cte_join_has_subplan_exprs(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubPlan))
		return true;
	return expression_tree_walker(node, gp_slice_cte_join_has_subplan_exprs,
								  context);
}

/* Whether the plan tree keeps a correlated subplan */
static bool
gp_slice_cte_join_has_subplan(Plan *plan)
{
	if (plan == NULL)
		return false;
	if (gp_slice_cte_join_has_subplan_exprs((Node *) plan->targetlist, NULL) ||
		gp_slice_cte_join_has_subplan_exprs((Node *) plan->qual, NULL))
		return true;
	if (gp_slice_cte_join_has_subplan(plan->lefttree) ||
		gp_slice_cte_join_has_subplan(plan->righttree))
		return true;
	if (IsA(plan, SubqueryScan) &&
		gp_slice_cte_join_has_subplan(((SubqueryScan *) plan)->subplan))
		return true;
	if (IsA(plan, Append))
	{
		foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
			if (gp_slice_cte_join_has_subplan(child))
				return true;
	}
	if (IsA(plan, MergeAppend))
	{
		foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
			if (gp_slice_cte_join_has_subplan(child))
				return true;
	}
	return false;
}

/* The variables of the scan of the CTE name the columns of the slice */
typedef struct GpCteJoinQual
{
	Plan	   *join;			/* the plan of the join, for the search */
	Index		cterti;			/* the number of the CTE in the statement */
	bool		ok;
} GpCteJoinQual;

static Node *
gp_slice_cte_join_qual_var(Node *node, void *context)
{
	GpCteJoinQual *ctx = context;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		ListCell   *lc;

		if (var->varno != ctx->cterti || var->varlevelsup != 0)
			return node;
		foreach(lc, ctx->join->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Index		rti;
			AttrNumber	attno;

			if (gp_slice_base_column(ctx->join, tle, &rti, &attno) &&
				rti == var->varno && attno == var->varattno)
				return (Node *) makeVar(INDEX_VAR, tle->resno,
										var->vartype, var->vartypmod,
										var->varcollid, 0);
		}
		ctx->ok = false;
		return node;
	}
	return expression_tree_mutator(node, gp_slice_cte_join_qual_var, context);
}

/* The two leaves of the FROM of a join of one CTE and one table */
typedef struct GpCteJoinSides
{
	Index		cterti;			/* the RT index of the CTE */
	Index		tableti;		/* the RT index of the table */
} GpCteJoinSides;

/* A grouping column of the CTE equated with a column of the table */
typedef struct GpCteJoinPair
{
	Index		resno;			/* the position of the column in the CTE */
	AttrNumber	groupattno;		/* the column of the table of the CTE */
	AttrNumber	tableattno;		/* the column of the table of the join */
	Oid			type;			/* the common type of the two columns */
} GpCteJoinPair;

/*
 * The column of the table of the CTE that one output column of the CTE
 * comes from.  A grouping column of this PostgreSQL names the grouping
 * itself, whose expressions carry the columns of the table
 */
static Var *
gp_slice_cte_join_group_var(Query *ctequery, TargetEntry *tle)
{
	Var		   *var;
	RangeTblEntry *rte;

	if (!IsA(tle->expr, Var) || ((Var *) tle->expr)->varlevelsup != 0)
		return NULL;
	var = (Var *) tle->expr;
	if (var->varattno <= 0 || var->varno > list_length(ctequery->rtable))
		return NULL;
	rte = rt_fetch(var->varno, ctequery->rtable);
	if (rte->rtekind == RTE_GROUP)
	{
		Node	   *expr;

		if (var->varattno > list_length(rte->groupexprs))
			return NULL;
		expr = list_nth(rte->groupexprs, var->varattno - 1);
		if (!IsA(expr, Var))
			return NULL;
		var = (Var *) expr;
	}
	if (var->varlevelsup != 0 || var->varattno <= 0 ||
		var->varno > list_length(ctequery->rtable))
		return NULL;
	rte = rt_fetch(var->varno, ctequery->rtable);
	if (rte->rtekind != RTE_RELATION)
		return NULL;
	return var;
}

/*
 * Collect the equalities of a CTE column with a column of the table, at the
 * top of an AND or below it.  Every equality of other kinds stays out
 */
static void
gp_slice_cte_join_pairs(Node *qual, GpCteJoinSides *sides, Query *ctequery,
						List **pairs)
{
	OpExpr	   *op;
	Var		   *ctevar;
	Var		   *tablevar;
	TargetEntry *tle;
	Var		   *groupvar;
	GpCteJoinPair *pair;

	if (qual == NULL)
		return;
	if (IsA(qual, BoolExpr) && ((BoolExpr *) qual)->boolop == AND_EXPR)
	{
		ListCell   *lc;

		foreach(lc, ((BoolExpr *) qual)->args)
			gp_slice_cte_join_pairs(lfirst(lc), sides, ctequery, pairs);
		return;
	}
	if (!IsA(qual, OpExpr))
		return;
	op = (OpExpr *) qual;
	if (list_length(op->args) != 2 || get_opname(op->opno) == NULL ||
		strcmp(get_opname(op->opno), "=") != 0 ||
		!IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Var))
		return;
	if (((Var *) linitial(op->args))->varno == sides->cterti &&
		((Var *) lsecond(op->args))->varno == sides->tableti)
	{
		ctevar = (Var *) linitial(op->args);
		tablevar = (Var *) lsecond(op->args);
	}
	else if (((Var *) lsecond(op->args))->varno == sides->cterti &&
			 ((Var *) linitial(op->args))->varno == sides->tableti)
	{
		ctevar = (Var *) lsecond(op->args);
		tablevar = (Var *) linitial(op->args);
	}
	else
		return;
	if (ctevar->varlevelsup != 0 || tablevar->varlevelsup != 0 ||
		ctevar->varattno <= 0 || tablevar->varattno <= 0)
		return;
	tle = get_tle_by_resno(ctequery->targetList, ctevar->varattno);
	if (tle == NULL)
		return;
	groupvar = gp_slice_cte_join_group_var(ctequery, tle);
	if (groupvar == NULL || groupvar->vartype != tablevar->vartype)
		return;
	pair = palloc0(sizeof(GpCteJoinPair));
	pair->resno = tle->resno;
	pair->groupattno = groupvar->varattno;
	pair->tableattno = tablevar->varattno;
	pair->type = tablevar->vartype;
	*pairs = lappend(*pairs, pair);
}

/*
 * Whether every grouping column of the CTE is a column of its answer
 * equated with a column of the table, and none of the pairs repeats a
 * column of the table.  The keys of the table side and the grouping
 * columns of the table of the CTE, both in the order of the grouping, go
 * out for the pulls of the two sides by the hash of their columns
 */
static bool
gp_slice_cte_join_keys(Query *ctequery, List *pairs, AttrNumber **tablekeys,
					   AttrNumber **groupkeys)
{
	AttrNumber *keyatts;
	AttrNumber *groupatts;
	int			nkeys = list_length(ctequery->groupClause);
	int			i = 0;

	if (nkeys == 0 || list_length(pairs) < nkeys)
		return false;
	keyatts = palloc_array(AttrNumber, nkeys);
	groupatts = palloc_array(AttrNumber, nkeys);
	foreach_node(SortGroupClause, sc, ctequery->groupClause)
	{
		TargetEntry *tle = get_sortgroupclause_tle(sc, ctequery->targetList);
		Var		   *groupvar;
		GpCteJoinPair *found = NULL;
		ListCell   *lc2;

		if (tle->resjunk)
			return false;
		groupvar = gp_slice_cte_join_group_var(ctequery, tle);
		if (groupvar == NULL || !gp_slice_hashable(groupvar->vartype))
			return false;
		foreach(lc2, pairs)
		{
			GpCteJoinPair *pair = lfirst(lc2);

			if (pair->resno == tle->resno)
			{
				found = pair;
				break;
			}
		}
		if (found == NULL || !gp_slice_hashable(found->type) ||
			found->groupattno != groupvar->varattno)
			return false;
		for (int j = 0; j < i; j++)
			if (keyatts[j] == found->tableattno)
				return false;
		keyatts[i] = found->tableattno;
		groupatts[i] = groupvar->varattno;
		i++;
	}
	if (tablekeys != NULL)
		*tablekeys = keyatts;
	if (groupkeys != NULL)
		*groupkeys = groupatts;
	return true;
}

/* The two leaves of the FROM: one CTE and one table, joined or comma-joined */
static bool
gp_slice_cte_join_leaves(Query *parse, GpCteJoinSides *sides)
{
	FromExpr   *from = (FromExpr *) parse->jointree;
	List	   *leaves = NIL;
	ListCell   *lc;

	sides->cterti = 0;
	sides->tableti = 0;
	if (!IsA(parse->jointree, FromExpr))
		return false;
	if (list_length(from->fromlist) == 1 && IsA(linitial(from->fromlist), JoinExpr))
	{
		JoinExpr   *join = linitial_node(JoinExpr, from->fromlist);

		if (!IsA(join->larg, RangeTblRef) || !IsA(join->rarg, RangeTblRef))
			return false;
		leaves = list_make2(join->larg, join->rarg);
	}
	else if (list_length(from->fromlist) == 2)
		leaves = from->fromlist;
	else
		return false;
	foreach(lc, leaves)
	{
		Index		rti = lfirst_node(RangeTblRef, lc)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, parse->rtable);

		if (rte->rtekind == RTE_CTE)
		{
			if (sides->cterti != 0)
				return false;
			sides->cterti = rti;
		}
		else if (rte->rtekind == RTE_RELATION)
		{
			if (sides->tableti != 0)
				return false;
			sides->tableti = rti;
		}
		else
			return false;
	}
	return sides->cterti != 0 && sides->tableti != 0;
}

/* Whether the relation keeps row security or inheritance children */
static bool
gp_slice_cte_join_protected(RangeTblEntry *rte)
{
	HeapTuple	tuple;
	Form_pg_class rel;

	if (rte->inh)
		return true;
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", rte->relid);
	rel = (Form_pg_class) GETSTRUCT(tuple);
	if (rel->relrowsecurity || rel->relforcerowsecurity)
	{
		ReleaseSysCache(tuple);
		return true;
	}
	ReleaseSysCache(tuple);
	return false;
}

/* The relations of the query and of its CTE keep RLS or children */
static bool
gp_slice_cte_join_rls(Query *ctequery, Query *parse)
{
	foreach_node(RangeTblEntry, rte, parse->rtable)
		if (rte->rtekind == RTE_RELATION && gp_slice_cte_join_protected(rte))
			return true;
	foreach_node(RangeTblEntry, rte, ctequery->rtable)
		if (rte->rtekind == RTE_RELATION && gp_slice_cte_join_protected(rte))
			return true;
	return false;
}

/*
 * The join of one aggregate CTE with one hashed table, where every grouping
 * column of the CTE is equated with a column of the table.  The rows of the
 * table move by the hash of those columns under the map of the table of the
 * CTE, and the rows of the table of the CTE move by its grouping columns,
 * so every pair of the join meets on one segment.  A copy of the query is
 * returned for the SQL of the segments, or NULL when it is not such a join
 */
Query *
gp_slice_cte_join_query(Query *parse)
{
	CommonTableExpr *cte;
	Query	   *ctequery;
	RangeTblEntry *rte;
	GpCteJoinSides sides;
	GpPolicy	policy;
	GpPolicy	ctepolicy;
	List	   *pairs = NIL;
	ListCell   *lc;
	bool		covering;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		list_length(parse->cteList) != 1 ||
		parse->hasWindowFuncs || parse->hasAggs ||
		parse->hasTargetSRFs || parse->hasRowSecurity ||
		parse->setOperations != NULL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return NULL;
	if (!gp_slice_cte_join_leaves(parse, &sides))
		return NULL;
	cte = linitial_node(CommonTableExpr, parse->cteList);
	if (cte->cterecursive)
		return NULL;
	ctequery = castNode(Query, cte->ctequery);
	if (ctequery->sortClause != NIL || gp_slice_agg_query(ctequery) == NULL)
		return NULL;

	/* The table of the CTE keeps its rows apart, as the motion needs */
	rte = rt_fetch(linitial_node(RangeTblRef,
								 ((FromExpr *) ctequery->jointree)->fromlist)->rtindex,
				   ctequery->rtable);
	if (rte->rtekind != RTE_RELATION || !gp_policy_get(rte->relid, &ctepolicy) ||
		ctepolicy.replicated)
		return NULL;

	/* The table of the join is hashed, as its rows move by the join */
	rte = rt_fetch(sides.tableti, parse->rtable);
	if (!gp_policy_get(rte->relid, &policy) || policy.replicated)
		return NULL;

	/*
	 * A CTE whose grouping covers the key of its table needs no motion and
	 * keeps the locus of the subplan: leave it to the subplan path
	 */
	covering = true;
	for (int i = 0; i < ctepolicy.nkeys && covering; i++)
	{
		bool		grouped = false;

		foreach_node(SortGroupClause, sc, ctequery->groupClause)
		{
			TargetEntry *tle = get_sortgroupclause_tle(sc, ctequery->targetList);
			Var		   *groupvar;

			if (tle->resjunk)
				continue;
			groupvar = gp_slice_cte_join_group_var(ctequery, tle);
			if (groupvar != NULL && groupvar->varattno == ctepolicy.keys[i])
			{
				grouped = true;
				break;
			}
		}
		covering = grouped;
	}
	if (covering)
		return NULL;

	gp_slice_cte_join_pairs((Node *) ((FromExpr *) parse->jointree)->quals,
							&sides, ctequery, &pairs);
	foreach(lc, ((FromExpr *) parse->jointree)->fromlist)
		if (IsA(lfirst(lc), JoinExpr))
			gp_slice_cte_join_pairs(((JoinExpr *) lfirst(lc))->quals, &sides,
									ctequery, &pairs);
	if (!gp_slice_cte_join_keys(ctequery, pairs, NULL, NULL))
		return NULL;
	if (!gp_slice_cte_join_sublinks_ok(parse, sides.cterti))
		return NULL;
	if (gp_dispatch_in_xact())
		return NULL;
	return gp_slice_query_copy(parse);
}

/*
 * Replace the plan of a join of one aggregate CTE with one table by a scan
 * that runs it on the segments: the CTE is computed there by its grouping
 * columns, and the rows of the table move by the equated columns, so every
 * pair of the join meets on one segment.  A kept side of an outer join
 * stays where its rows are; the null-supplying side of a kept CTE is
 * broadcast to every segment.  The sort and the limit of the query stay
 * above the scan
 */
bool
gp_slice_cte_join_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	Plan	   *subplan;
	Plan	   *below;
	Plan	   *ctescan = NULL;
	Plan	   *tableplan = NULL;
	Join	   *join;
	Agg		   *agg;
	Query	   *ctequery;
	RangeTblEntry *rte;
	CommonTableExpr *cte;
	GpCteJoinSides sides;
	Index		planrti;
	Index		cteglobal = 0;
	int			nentries;
	int			cteplanid;
	int			nparams = 0;
	int			nqual = 0;
	List	   *ctequal;
	List	   *ctequalm = NIL;
	GpCteJoinQual ctx;
	GpJoinTable	ctetable = {0};
	GpJoinTable	jointable = {0};
	GpPolicy	ctepolicy;
	GpPolicy	policy;
	List	   *pairs = NIL;
	ListCell   *lc;
	AttrNumber *tablekeys;
	AttrNumber *groupkeys;
	int			nkeys;
	bool		sort_above = false;
	bool		cte_kept;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL || (!IsA(plan, NestLoop) && !IsA(plan, HashJoin) &&
						 !IsA(plan, MergeJoin)))
	return false;
	join = (Join *) plan;
	if (join->jointype != JOIN_INNER && join->jointype != JOIN_LEFT &&
		join->jointype != JOIN_RIGHT)
	return false;

	/* Non-aggregate plans without a final Sort keep their ordering here */
	if (original->sortClause != NIL && !sort_above)
	return false;

	/*
	 * One side is the scan of the CTE, the other is the gather of the
	 * table.  The build side of a hash join hangs under its Hash
	 */
	{
		Plan	   *left = join->plan.lefttree;
		Plan	   *right = join->plan.righttree;

		if (IsA(left, Hash))
			left = left->lefttree;
		if (IsA(right, Hash))
			right = right->lefttree;
		if (IsA(left, CteScan) && gp_is_gather(right))
		{
			ctescan = left;
			tableplan = right;
			cte_kept = join->jointype == JOIN_LEFT;
		}
		else if (IsA(right, CteScan) && gp_is_gather(left))
		{
			ctescan = right;
			tableplan = left;
			cte_kept = join->jointype == JOIN_RIGHT;
		}
		else
		return false;
	}

	/* The gather scans the table of the query, not another relation */
	rte = rt_fetch(((Scan *) tableplan)->scanrelid, stmt->rtable);
	planrti = 0;
	nentries = 0;
	foreach_node(RangeTblEntry, entry, original->rtable)
	{
		nentries++;
		if (entry->rtekind == RTE_RELATION && entry->relid == rte->relid)
		{
			planrti = nentries;
			break;
		}
	}
	if (planrti == 0)
	return false;

	/* The subplan of the CTE is the aggregate over the gather, as before */
	cteplanid = ((CteScan *) ctescan)->ctePlanId;
	if (cteplanid <= 0 || cteplanid > list_length(stmt->subplans))
	return false;
	subplan = list_nth(stmt->subplans, cteplanid - 1);
	plan = subplan;
	while (plan != NULL && !IsA(plan, Agg))
	{
		if (plan->righttree != NULL)
		return false;
		plan = plan->lefttree;
	}
	if (plan == NULL)
	return false;
	agg = (Agg *) plan;
	below = agg->plan.lefttree;
	while (below != NULL && (IsA(below, Sort) || IsA(below, IncrementalSort)))
		below = below->lefttree;
	if (below == NULL || !gp_is_gather(below))
	return false;

	/*
	 * The quals of the scan of the CTE hold the Params of the init plans,
	 * and their columns are the columns of the answer of the CTE
	 */
	ctequal = ((CteScan *) ctescan)->scan.plan.qual;
	if (ctequal != NIL)
	{
		TargetEntry *tle = linitial(((CteScan *) ctescan)->scan.plan.targetlist);

		if (tle == NULL || !IsA(tle->expr, Var))
		return false;
		cteglobal = ((Var *) tle->expr)->varno;
	}

	/* The analysis of the query, the same the eligibility went through */
	cte = linitial_node(CommonTableExpr, original->cteList);
	ctequery = castNode(Query, cte->ctequery);
	if (!gp_slice_cte_join_leaves(original, &sides))
	return false;
	if (sides.tableti != planrti)
	return false;
	rte = rt_fetch(sides.tableti, original->rtable);
	if (!gp_policy_get(rte->relid, &policy))
	return false;
	ctetable.rti = linitial_node(RangeTblRef,
								 ((FromExpr *) ctequery->jointree)->fromlist)->rtindex;
	rte = rt_fetch(ctetable.rti, ctequery->rtable);
	if (rte->rtekind != RTE_RELATION ||
		!gp_policy_get(rte->relid, &ctepolicy))
	return false;
	ctetable.query = ctequery;
	ctetable.policy = ctepolicy;
	jointable.rti = sides.tableti;
	jointable.query = original;
	jointable.policy = policy;

	gp_slice_cte_join_pairs((Node *) ((FromExpr *) original->jointree)->quals,
							&sides, ctequery, &pairs);
	foreach(lc, ((FromExpr *) original->jointree)->fromlist)
		if (IsA(lfirst(lc), JoinExpr))
			gp_slice_cte_join_pairs(((JoinExpr *) lfirst(lc))->quals, &sides,
									ctequery, &pairs);
	if (!gp_slice_cte_join_keys(ctequery, pairs, &tablekeys, &groupkeys))
	return false;
	nkeys = list_length(ctequery->groupClause);

	/*
	 * The grouping columns of the plan name the columns of the table of the
	 * CTE: the same columns the analysis found, in the same order
	 */
	for (int i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = list_nth(below->targetlist, agg->grpColIdx[i] - 1);
		Var		   *var;

		if (!IsA(tle->expr, Var) || ((Var *) tle->expr)->varattno <= 0)
		return false;
		var = (Var *) tle->expr;

		/* The plan groups by the same column the analysis found */
		if (var->varattno != groupkeys[i])
		return false;
	}

	/*
	 * Every sublink of the query became an InitPlan above the scan of the
	 * CTE.  The Params of the init plans sit in the quals of that scan
	 * alone, and their filter moves onto the replacement: the share of the
	 * CTE that the segments answer knows nothing of the whole
	 */
	if (original->hasSubLinks)
	{
		if (gp_slice_cte_join_has_subplan(stmt->planTree))
		return false;
		foreach_node(SubPlan, ip, stmt->planTree->initPlan)
		{
			Plan	   *sub;

			/*
			 * The list keeps a record of the CTE itself: its subplan is
			 * materialized at the start, and the answer of the segments
			 * replaces it below
			 */
			if (ip->plan_id == cteplanid)
				continue;
			if (ip->plan_id <= 0 || ip->plan_id > list_length(stmt->subplans))
				return false;
			sub = list_nth(stmt->subplans, ip->plan_id - 1);
			if (!IsA(sub, Agg))
				return false;
			if (((Agg *) sub)->plan.lefttree == NULL ||
				!IsA(((Agg *) sub)->plan.lefttree, CteScan) ||
				((CteScan *) ((Agg *) sub)->plan.lefttree)->ctePlanId != cteplanid)
				return false;
		}
		gp_slice_cte_join_param_count(stmt->planTree, &nparams);
		gp_slice_cte_join_param_count_exprs((Node *) ctequal, &nqual);
		if (nqual == 0 || nparams != nqual)
		return false;

		/* The columns the filter names must come out of the join */
		ctx.join = &join->plan;
		ctx.cterti = cteglobal;
		ctx.ok = true;
		ctequalm = (List *) gp_slice_cte_join_qual_var((Node *) copyObject(ctequal),
													   &ctx);
		if (!ctx.ok)
		return false;

		/* The segments answer the join without the sublinks of the query */
		original->jointree->quals = gp_slice_cte_join_strip(
			original->jointree->quals);
		if (gp_slice_cte_join_has_sublink((Node *) original, NULL))
		return false;
	}

	/*
	 * The table of the CTE moves by its grouping columns under the map of
	 * the side that keeps its rows.  An inner join keeps the CTE and moves
	 * the rows of the table by the hash of the equated columns under the
	 * same map, so every pair meets on one segment
	 */
	if (join->jointype == JOIN_INNER || cte_kept)
	{
		if (!gp_slice_redistribute_rte_keys(original, &ctetable, &ctetable,
											false, nkeys, groupkeys,
											NULL, false))
		return false;
		if (join->jointype == JOIN_INNER)
		{
			/*
			 * The rows of the table move by the hash of the equated
			 * columns under the same map, so every pair meets
			 */
			if (!gp_slice_redistribute_rte_keys(original, &ctetable, &jointable,
												false, nkeys, tablekeys,
												NULL, false))
			return false;
		}
		else
		{
			/*
			 * A kept CTE holds every unmatched group of its own: the
			 * null-supplying table broadcasts to every segment
			 */
			if (!gp_slice_redistribute_rte_keys(original, &ctetable, &jointable,
												true, policy.nkeys, policy.keys,
												NULL, false))
			return false;
		}
	}
	else
	{
		/*
		 * The table keeps its rows, and the CTE meets it where it stands.
		 * That is only possible when the key of the distribution of the
		 * table is the equated columns themselves.  Over any other key
		 * the rows of the table move by the equated columns instead, the
		 * way an inner join moves them: a kept row then stands beside its
		 * partner, or alone, with the columns of the CTE null
		 */
		bool		stationary = policy.nkeys == nkeys;

		for (int i = 0; stationary && i < nkeys; i++)
			stationary = policy.keys[i] == tablekeys[i];
		if (stationary)
		{
			if (!gp_slice_redistribute_rte_keys(original, &jointable, &ctetable,
												false, nkeys, groupkeys,
												NULL, false))
			return false;
		}
		else
		{
			if (!gp_slice_redistribute_rte_keys(original, &ctetable, &ctetable,
												false, nkeys, groupkeys,
												NULL, false))
			return false;
			if (!gp_slice_redistribute_rte_keys(original, &ctetable, &jointable,
												false, nkeys, tablekeys,
												NULL, false))
			return false;
		}
	}

	/* The columns the segments return are the target list of the join */
	if (list_length(join->plan.targetlist) != list_length(original->targetList))
	return false;

	{
		CustomScan *cscan = gp_slice_make(&join->plan, original, false, true,
										  original->sortClause);

		/*
		 * The tables of the CTE hide from the walk of gp_slice_autonomous:
		 * keep such a slice on libpq, whose connections see the policies
		 */
		if (gp_slice_cte_join_rls(ctequery, original))
			lfourth(cscan->custom_private) = makeBoolean(false);

		/*
		 * The filter of the init plans carries onto the replacement: the
		 * answer of the join holds every pair, and the query above keeps
		 * only the pairs of the whole answer of the CTE
		 */
		if (ctequalm != NIL)
			cscan->scan.plan.qual = ctequalm;

		/*
		 * The init plans hang on the node of the join: carry them onto
		 * the replacement, whose filter reads their answers
		 */
		cscan->scan.plan.initPlan = join->plan.initPlan;
		join->plan.initPlan = NIL;

		if (parent == NULL)
			stmt->planTree = &cscan->scan.plan;
		else
			parent->lefttree = &cscan->scan.plan;
	}

	/*
	 * The scan of the CTE reads the subplan: answer it from the segments
	 * as well, whose query of the CTE carries the pull already
	 */
	if (original->hasSubLinks &&
		!gp_slice_cte_agg_replace(ctequery, stmt, cteplanid, NULL, agg,
								  false, true))
	return false;

	stmt->parallelModeNeeded = false;
	return true;
}


/*
 * Whether an aggregate of one table can run on the segments.  A replicated
 * table is answered by one of them.  A hash table answers locally when the
 * grouping covers its distribution key.  Otherwise its rows move by the
 * grouping columns, provided they are plain columns of the table: every group
 * then lands on one segment, which computes the whole aggregate
 */
static bool
gp_slice_agg_locus(Query *original, Agg *agg, Plan *below,
				   bool *replicated, bool *motion, GpLocus *locus)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;
	GpJoinTable *table;
	AttrNumber *keyatts;

	*locus = gp_locus_make(GP_LOCUS_COORDINATOR);
	*replicated = false;
	*motion = false;
	if (!IsA(original->jointree, FromExpr) ||
		list_length(original->jointree->fromlist) != 1 ||
		!IsA(linitial(original->jointree->fromlist), RangeTblRef))
		return false;
	ref = linitial_node(RangeTblRef, original->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, original->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	table = palloc0(sizeof(GpJoinTable));
	table->rti = ref->rtindex;
	if (!gp_policy_get(rte->relid, &table->policy))
		return false;
	*locus = gp_locus_policy(&table->policy);
	if (table->policy.replicated)
	{
		*replicated = true;
		return true;
	}

	if (gp_locus_grouped(*locus, agg, below))
	{
		*locus = gp_locus_project(&agg->plan, *locus);
		return true;
	}
	if (agg->numCols == 0 || gp_dispatch_in_xact())
		return false;

	/* Move every row by all the grouping columns instead */
	keyatts = palloc_array(AttrNumber, agg->numCols);
	for (int i = 0; i < agg->numCols; i++)
	{
		TargetEntry *tle = list_nth(below->targetlist, agg->grpColIdx[i] - 1);
		Var		   *var;

		if (!IsA(tle->expr, Var))
			return false;
		var = (Var *) tle->expr;
		if (var->varattno <= 0 || !gp_slice_hashable(var->vartype))
			return false;
		keyatts[i] = var->varattno;
	}
	if (!gp_slice_redistribute_rte_keys(original, table, table, false,
										agg->numCols, keyatts, NULL, false))
		return false;
	locus->colocation_id = table->policy.colocation_id;
	locus->nkeys = agg->numCols;
	locus->keys = keyatts;
	*locus = gp_locus_project(&agg->plan, *locus);
	*motion = true;
	return true;
}

/*
 * Rewrite the source of INSERT INTO a hashed table SELECT direct columns of
 * one hashed table into a pull by the target's distribution key.  Every
 * segment then inserts only the rows that belong to it.  Expressions and
 * defaults stay on the coordinator: functions and sequences must keep their
 * current meaning, and peer connections cannot see earlier writes
 */
bool
gp_slice_insert_motion(Query *parse, bool *motion)
{
	RangeTblEntry *target;
	RangeTblEntry *source;
	RangeTblRef *ref;
	Relation	target_rel;
	Relation	source_rel;
	Query	   *motion_query = parse;
	Index		outer_rti;
	GpJoinTable stay = {0};
	GpJoinTable move = {0};
	GpLocus	source_locus;
	GpLocus	target_locus;
	AttrNumber *keyatts;
	AttrNumber *sourceatts;
	Bitmapset  *needed = NULL;
	int			natts;

	if (!IsUnderPostmaster || parse->commandType != CMD_INSERT ||
		gp_dispatch_in_xact())
		return false;
	if (parse->resultRelation <= 0 || parse->hasSubLinks ||
		parse->hasAggs || parse->hasWindowFuncs || parse->hasTargetSRFs ||
		parse->hasRowSecurity || parse->setOperations != NULL ||
		parse->cteList != NIL || parse->groupClause != NIL ||
		parse->groupingSets != NIL || parse->distinctClause != NIL ||
		parse->sortClause != NIL || parse->limitCount != NULL ||
		parse->limitOffset != NULL || parse->rowMarks != NIL)
		return false;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
		parse->jointree->quals != NULL)
		return false;

	target = rt_fetch(parse->resultRelation, parse->rtable);
	if (target->rtekind != RTE_RELATION ||
		!gp_policy_get(target->relid, &stay.policy))
		return false;
	if (stay.policy.replicated)
		return false;
	stay.rti = parse->resultRelation;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	outer_rti = ref->rtindex;
	source = rt_fetch(outer_rti, parse->rtable);
	if (source->rtekind == RTE_SUBQUERY)
	{
		motion_query = source->subquery;
		if (motion_query == NULL || motion_query->hasSubLinks ||
			motion_query->hasAggs || motion_query->hasWindowFuncs ||
			motion_query->hasTargetSRFs || motion_query->hasRowSecurity ||
			motion_query->setOperations != NULL || motion_query->cteList != NIL ||
			motion_query->groupClause != NIL || motion_query->groupingSets != NIL ||
			motion_query->distinctClause != NIL || motion_query->sortClause != NIL ||
			motion_query->limitCount != NULL || motion_query->limitOffset != NULL ||
			motion_query->rowMarks != NIL)
			return false;
		if (!IsA(motion_query->jointree, FromExpr) ||
			list_length(motion_query->jointree->fromlist) != 1 ||
			!IsA(linitial(motion_query->jointree->fromlist), RangeTblRef) ||
			motion_query->jointree->quals != NULL)
			return false;
		ref = linitial_node(RangeTblRef, motion_query->jointree->fromlist);
		source = rt_fetch(ref->rtindex, motion_query->rtable);
	}
	if (source->rtekind != RTE_RELATION ||
		!gp_policy_get(source->relid, &move.policy) || move.policy.replicated)
		return false;
	move.rti = ref->rtindex;
	source_locus = gp_locus_policy(&move.policy);
	target_locus = gp_locus_policy(&stay.policy);

	target_rel = table_open(target->relid, AccessShareLock);
	source_rel = table_open(source->relid, AccessShareLock);
	natts = RelationGetDescr(target_rel)->natts;
	if (list_length(parse->targetList) != natts)
		goto unsupported;
	keyatts = palloc_array(AttrNumber, stay.policy.nkeys);
	sourceatts = palloc_array(AttrNumber, natts);
	if (motion_query != parse)
	{
		foreach_node(TargetEntry, inner_tle, motion_query->targetList)
		{
			Var		   *inner_var;

			if (inner_tle->resjunk || !IsA(inner_tle->expr, Var))
				goto unsupported;
			inner_var = (Var *) inner_tle->expr;
			if (inner_var->varno != move.rti || inner_var->varattno <= 0)
				goto unsupported;
		}
	}
	for (int attno = 1; attno <= natts; attno++)
	{
		TargetEntry *tle = get_tle_by_resno(parse->targetList, attno);
		Var		   *var;

		if (tle == NULL || tle->resjunk || !IsA(tle->expr, Var))
			goto unsupported;
		var = (Var *) tle->expr;
		if (var->varno != outer_rti || var->varattno <= 0)
			goto unsupported;
		sourceatts[attno - 1] = var->varattno;
		if (motion_query != parse)
		{
			TargetEntry *inner_tle = get_tle_by_resno(motion_query->targetList,
													  var->varattno);
			Var		   *inner_var;

			if (inner_tle == NULL || inner_tle->resjunk ||
				!IsA(inner_tle->expr, Var))
				goto unsupported;
			inner_var = (Var *) inner_tle->expr;
			if (inner_var->varno != move.rti || inner_var->varattno <= 0)
				goto unsupported;
			sourceatts[attno - 1] = inner_var->varattno;
		}
		needed = bms_add_member(needed, sourceatts[attno - 1]);
	}
	for (int i = 0; i < stay.policy.nkeys; i++)
	{
		if (get_atttype(target->relid, stay.policy.keys[i]) !=
			get_atttype(source->relid, sourceatts[stay.policy.keys[i] - 1]))
			goto unsupported;
		keyatts[i] = sourceatts[stay.policy.keys[i] - 1];
	}
	/* The source is already colocated with the target only by every key */
	*motion = false;
	if (source_locus.colocation_id != target_locus.colocation_id ||
		source_locus.nkeys != target_locus.nkeys)
		*motion = true;
	for (int i = 0; i < stay.policy.nkeys && !*motion; i++)
		if (sourceatts[stay.policy.keys[i] - 1] != source_locus.keys[i])
			*motion = true;
	table_close(target_rel, AccessShareLock);
	table_close(source_rel, AccessShareLock);

	if (!*motion)
		return true;
	return gp_slice_redistribute_rte_keys(motion_query, &stay, &move, false,
										  stay.policy.nkeys, keyatts, needed, true);

unsupported:
	table_close(target_rel, AccessShareLock);
	table_close(source_rel, AccessShareLock);
	return false;
}

/*
 * Whether every join of the tree is inner.  An outer join below an inner
 * root also makes an N-way motion unsafe
 */
static bool
gp_slice_join_all_inner(Node *node)
{
	JoinExpr   *join;

	if (IsA(node, RangeTblRef))
		return true;
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	return join->jointype == JOIN_INNER &&
		gp_slice_join_all_inner(join->larg) &&
		gp_slice_join_all_inner(join->rarg);
}

static bool
gp_slice_join_all_inner_from(Query *parse)
{
	ListCell   *lc;

	foreach(lc, parse->jointree->fromlist)
	{
		if (!gp_slice_join_all_inner(lfirst(lc)))
			return false;
	}
	return true;
}

/*
 * Whether every join of the fromlist tree has the jointype: the level is
 * a chain of one kind of an outer join, so one side of its rule decides
 * the motions of the whole level
 */
static bool
gp_slice_join_all_same_type(Node *node, JoinType *type)
{
	JoinExpr   *join;

	if (IsA(node, RangeTblRef))
		return true;
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	return join->jointype == *type &&
		gp_slice_join_all_same_type(join->larg, type) &&
		gp_slice_join_all_same_type(join->rarg, type);
}

/*
 * Whether the motions of a derived join are worth their launches.  A join
 * whose tables are all colocated moves nothing, and joins of six tables or
 * fewer keep the size of their answer from the interconnect launches.  A
 * bigger join that moves tables launches a worker of every moved table on
 * every segment, and one launch costs the time of a local join of about a
 * hundred thousand rows (measured: a tiny eight-table join took 98 seconds
 * distributed against 0.15 seconds local).  Tables of an unknown size count
 * as unaffordable, for the fallback always gives a right answer.
 */
#define GP_MOTION_LAUNCH_ROWS 100000.0

static bool
gp_slice_join_motion_affordable(Query *parse, List *tables, List *conds)
{
	double		local_rows = 0;
	int			nmoved = 0;
	int			nsegments;
	GpSegment  *segments;
	ListCell   *lc;

	if (list_length(tables) <= 6)
		return true;
	if (gp_slice_join_colocated(parse, tables, conds))
		return true;

	nsegments = gp_segments_get(&segments);
	pfree(segments);
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		double		table_rows;

		if (table->policy.replicated)
			continue;
		nmoved++;
		if (!gp_slice_join_rows(parse, table, &table_rows))
			return false;
		local_rows += table_rows;
	}
	if (nmoved == 0 || nsegments == 0)
		return true;
	return local_rows >=
		(double) nmoved * (double) nsegments * GP_MOTION_LAUNCH_ROWS;
}

static bool
gp_slice_join_derived(Query *parse)
{
	foreach_node(RangeTblEntry, rte, parse->rtable)
		if (rte->rtekind == RTE_SUBQUERY)
			return true;
	return false;
}

static bool
gp_slice_join_all_inner_levels(List *tables)
{
	List	   *queries = NIL;
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!list_member_ptr(queries, table->query))
			queries = lappend(queries, table->query);
	}
	foreach(lc, queries)
	{
		if (!gp_slice_join_all_inner_from(lfirst(lc)))
			return false;
	}
	return true;
}

/*
 * The stationary side of a join the sublinks drive alone: the fromlist
 * keeps one table of its own, and the sublinks are semi, anti or scalar
 * over other tables.  The rows of the answer are the rows of that table,
 * so it partitions the answer and must stay, while every table of a
 * sublink must be whole on every segment.  A derived table of the fromlist
 * keeps the table of its own fromlist below
 */
static GpJoinTable *
gp_slice_join_kept_side(Query *original, List *tables)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;
	ListCell   *lc;

	if (!IsA(original->jointree, FromExpr) ||
		list_length(original->jointree->fromlist) != 1 ||
		!IsA(linitial(original->jointree->fromlist), RangeTblRef))
		return NULL;
	ref = linitial_node(RangeTblRef, original->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, original->rtable);
	while (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
	{
		Query	   *subquery = rte->subquery;

		if (!IsA(subquery->jointree, FromExpr) ||
			list_length(subquery->jointree->fromlist) != 1 ||
			!IsA(linitial(subquery->jointree->fromlist), RangeTblRef))
			return NULL;
		ref = linitial_node(RangeTblRef, subquery->jointree->fromlist);
		rte = rt_fetch(ref->rtindex, subquery->rtable);
	}
	if (rte->rtekind != RTE_RELATION)
		return NULL;
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (table->rti == ref->rtindex && !table->policy.replicated &&
			table->policy.colocation_id != 0)
			return table;
	}
	return NULL;
}

/*
 * Move the sides of a join of colocation groups.  An inner join moves every
 * table of another group by the hash of its key under the map of the first.
 * An outer join of two tables broadcasts its null-supplying side instead.  A
 * full join broadcasts both sides and runs on one segment, as either side can
 * have an unmatched row.  A join the sublinks drive alone keeps the table of
 * the fromlist and broadcasts every table of a sublink whole: the rows of
 * the answer are the rows of the kept table, so they partition the answer,
 * and a kept side broadcast instead would answer one kept row on every
 * segment that misses a partner of it locally, and the answer would repeat
 * the row once per such segment
 */
static bool
gp_slice_join_motion(Query *original, List *tables, List *conds,
					 bool *single_segment, GpJoinTable **agg_stay,
					 bool *kept_rows)
{
	GpJoinTable *stay = NULL;
	GpJoinTable *move = NULL;
	List	   *moves = NIL;
	ListCell   *lc;
	JoinType	jointype;
	bool		redistributing;
	bool		multiway = false;
	bool		outer_same = false;

	*single_segment = false;
	*agg_stay = NULL;
	*kept_rows = false;
	if (!gp_slice_join_load_policies(original, tables))
		return false;

	jointype = gp_slice_join_type_of(original);

	/*
	 * An outer join of a level with a sublink or below a derived table of
	 * the fromlist is not moved at all: a broadcast side joined per segment
	 * loses the rows of an outer join no segment sees whole.  A chain of
	 * base tables joined by one kind of an outer join moves instead: the
	 * kept side of every join of the chain is the same table, so one rule
	 * of the two-way join decides the motions of the whole level
	 */
	outer_same = list_length(tables) > 2 && !original->hasSubLinks &&
		!gp_slice_join_derived(original) &&
		(jointype == JOIN_LEFT || jointype == JOIN_FULL) &&
		list_length(original->jointree->fromlist) == 1 &&
		gp_slice_join_all_same_type(linitial(original->jointree->fromlist),
									&jointype);
	if ((list_length(tables) != 2 || original->hasSubLinks ||
		 gp_slice_join_derived(original)) &&
		!outer_same &&
		!gp_slice_join_all_inner_levels(tables))
		return false;
	stay = gp_slice_join_kept_side(original, tables);
	if (stay != NULL)
	{
		*agg_stay = stay;
		*kept_rows = true;
		foreach(lc, tables)
		{
			move = lfirst(lc);

			if (move == stay || move->policy.replicated)
				continue;
			if (!gp_slice_redistribute_rte(original, stay, move, true))
				return false;
		}
		return true;
	}
	redistributing = false;
	if (!outer_same)
	{
		redistributing = gp_slice_join_redistributable(original, tables, conds,
													   &stay, &moves);
		if (!redistributing && list_length(tables) > 2)
			multiway = gp_slice_join_broadcast_component(original, tables, conds,
														&stay, &moves);
		else if (!redistributing && !gp_slice_join_broadcastable(tables))
			return false;
		if (!redistributing && list_length(tables) > 2 && !multiway)
			return false;
	}
	if ((list_length(tables) != 2 || original->hasSubLinks ||
		 gp_slice_join_derived(original)) &&
		!outer_same &&
		!gp_slice_join_all_inner_levels(tables))
		return false;
	if (redistributing)
	{
		move = linitial(moves);

		/* Either side of a two-way inner join can stay: move the smaller */
		if (jointype == JOIN_INNER && list_length(tables) == 2)
		{
			GpJoinTable *other_stay;
			List	   *other_moves;
			List	   *reverse = list_make2(lsecond(tables), linitial(tables));
			double		rows;
			double		other_rows;

			if (gp_slice_join_redistributable(original, reverse, conds,
											  &other_stay, &other_moves) &&
				gp_slice_join_rows(original, move, &rows) &&
				gp_slice_join_rows(original, linitial(other_moves), &other_rows) &&
				other_rows < rows)
			{
				stay = other_stay;
				moves = other_moves;
				move = linitial(moves);
			}
		}
	}
	else if (multiway)
		*agg_stay = stay;
	else
	{
		stay = linitial(tables);
		move = lsecond(tables);

		/* Broadcast the smaller side when there is no useful join key */
		if (jointype == JOIN_INNER)
		{
			double		rows;
			double		other_rows;

			if (gp_slice_join_rows(original, stay, &rows) &&
				gp_slice_join_rows(original, move, &other_rows) && rows < other_rows)
			{
				stay = lsecond(tables);
				move = linitial(tables);
			}
		}
		*agg_stay = stay;
	}
	switch (jointype)
	{
		case JOIN_INNER:
			if (redistributing)
			{
				foreach(lc, moves)
				{
					move = lfirst(lc);
					if (!gp_slice_redistribute_rte(original, stay, move, false))
						return false;
				}
			}
			else if (multiway)
			{
				foreach(lc, moves)
				{
					move = lfirst(lc);
					if (!gp_slice_redistribute_rte(original, stay, move, true))
						return false;
				}
			}
			else if (!gp_slice_redistribute_rte(original, stay, move, true))
				return false;
			break;
		case JOIN_LEFT:
			if (list_length(tables) > 2)
			{
				foreach(lc, tables)
				{
					move = lfirst(lc);

					if (move == stay || move->policy.replicated)
						continue;
					if (!gp_slice_redistribute_rte(original, stay, move, true))
						return false;
				}
			}
			else if (!gp_slice_redistribute_rte(original, stay, move, true))
				return false;
			break;
		case JOIN_RIGHT:
			if (!gp_slice_redistribute_rte(original, move, stay, true))
				return false;
			break;
		case JOIN_FULL:
			if (list_length(tables) > 2)
			{
				/*
				 * The slice runs on one segment, so every table, the
				 * first included, must be whole there
				 */
				foreach(lc, tables)
				{
					move = lfirst(lc);

					if (move->policy.replicated)
						continue;
					if (!gp_slice_redistribute_rte(original, stay, move, true))
						return false;
				}
			}
			else if (!gp_slice_redistribute_rte(original, stay, move, true) ||
					 !gp_slice_redistribute_rte(original, move, stay, true))
				return false;
			*single_segment = true;
			break;
		default:
			return false;
	}
	return true;
}

/*
 * Resolve a column of a plan to the column of a base table it comes from,
 * through the OUTER_VAR and INNER_VAR references of the plans above
 */
static bool
gp_slice_base_column(Plan *plan, TargetEntry *tle, Index *rti,
					 AttrNumber *attno)
{
	Var		   *var;
	Plan	   *side;
	TargetEntry *stle;

	if (!IsA(tle->expr, Var))
		return false;
	var = (Var *) tle->expr;
	if (var->varno != OUTER_VAR && var->varno != INNER_VAR)
	{
		*rti = var->varno;
		*attno = var->varattno;
		return true;
	}

	side = var->varno == OUTER_VAR ? plan->lefttree : plan->righttree;
	if (side == NULL)
		return false;
	stle = list_nth(side->targetlist, var->varattno - 1);
	return gp_slice_base_column(side, stle, rti, attno);
}

/*
 * Whether an outer join can supply nulls for the columns of a base table.
 * A group by a key of such a table is not local to a segment: unmatched rows
 * on different segments all belong to its null group, which must be finished
 * on the coordinator rather than returned as several groups
 */
static bool
gp_slice_join_nullable(Node *node, Index rti, bool nullable)
{
	JoinExpr   *join;
	bool		left_nullable = nullable;
	bool		right_nullable = nullable;

	if (IsA(node, RangeTblRef))
		return nullable && ((RangeTblRef *) node)->rtindex == rti;
	if (!IsA(node, JoinExpr))
		return false;
	join = (JoinExpr *) node;
	if (join->jointype == JOIN_LEFT || join->jointype == JOIN_FULL)
		right_nullable = true;
	if (join->jointype == JOIN_RIGHT || join->jointype == JOIN_FULL)
		left_nullable = true;

	return gp_slice_join_nullable(join->larg, rti, left_nullable) ||
		gp_slice_join_nullable(join->rarg, rti, right_nullable);
}

/*
 * Whether every column of the expression reads the table of the fromlist:
 * the rows of the answer of a join the sublinks drive alone are the rows of
 * that table, so any grouping over its columns keeps every group on one
 * segment
 */
static bool
gp_slice_join_stay_vars(Node *node, void *context)
{
	Index		rti = *((Index *) context);

	if (node == NULL)
		return true;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup == 0 &&
			((Var *) node)->varno == rti &&
			((Var *) node)->varattno > 0;
	if (IsA(node, Query))
		return false;
	return expression_tree_walker(node, gp_slice_join_stay_vars, context);
}

/*
 * Whether the grouping of the aggregate covers the distribution key of
 * some hashed table of the join, so that every group is on one segment.
 * All hashed tables are colocated by their keys, so grouping by the whole
 * key of any one of them that is not null-supplying is enough.  A broadcast
 * join is the exception: only the key of its stationary table is local.
 * The grouping columns are read from the plan
 */
static bool
gp_slice_join_agg_keys(Agg *agg, List *tables, Query *original,
					   GpJoinTable *only)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);
		bool		all_keys = true;

		if (table->policy.replicated || (only != NULL && table != only) ||
			gp_slice_join_nullable(linitial(original->jointree->fromlist),
								  table->rti, false))
			continue;
		for (int i = 0; i < table->policy.nkeys; i++)
		{
			bool		grouped = false;

			for (int j = 0; j < agg->numCols; j++)
			{
				TargetEntry *tle = list_nth(agg->plan.lefttree->targetlist,
											agg->grpColIdx[j] - 1);
				Index		grti;
				AttrNumber	gattno;

				if (!gp_slice_base_column(agg->plan.lefttree, tle, &grti, &gattno))
					return false;
				if (grti == table->rti && gattno == table->policy.keys[i])
				{
					grouped = true;
					break;
				}
			}
			if (!grouped)
			{
				all_keys = false;
				break;
			}
		}
		if (all_keys)
			return true;
	}
	return false;
}

/*
 * Whether the query is a join the segments can compute: distributed tables
 * joined so that every row of the join is on one segment — hashed tables
 * of one colocation group connected by the whole key, with replicated
 * tables anywhere.  An aggregate over the join is allowed when its
 * grouping covers the key of a hashed table, which is checked on the plan.
 * A copy of the query is returned, or NULL when it is not such a join.
 */
Query *
gp_slice_join_query(Query *parse)
{
	List	   *tables = NIL;
	List	   *conds = NIL;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT)
		return NULL;
	if (parse->hasWindowFuncs ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return NULL;
	if (!IsA(parse->jointree, FromExpr) || parse->jointree->fromlist == NIL)
		return NULL;
	if (!gp_slice_join_from(parse, &tables, &conds))
		return NULL;
	if (!gp_slice_join_sublinks(parse, &tables, &conds))
		return NULL;
	if (list_length(tables) < 2)
		return NULL;
	/*
	 * Large derived joins wait for a cost bound on their nested Motion.  A
	 * derived outer join cannot be rewritten into Motion at the top level,
	 * but a grouped aggregate over it can redistribute the whole derived
	 * answer, unless a full join keeps it whole on one segment alone.
	 */
	if (gp_slice_join_derived(parse) &&
		(!gp_slice_join_all_inner_levels(tables) ||
		 !gp_slice_join_motion_affordable(parse, tables, conds)))
	{
		if (!parse->hasAggs || parse->groupClause == NIL ||
			!gp_slice_join_motion_affordable(parse, tables, conds) ||
			gp_slice_join_derived_full(parse))
			return NULL;
	}
	if (!gp_slice_join_colocated(parse, tables, conds))
	{
		GpJoinTable *stay;
		List	   *moves;

		if (gp_dispatch_in_xact() ||
			(list_length(tables) > 2 &&
			 !gp_slice_join_broadcast_component(parse, tables, conds,
											 &stay, &moves) &&
			 !gp_slice_join_redistributable(parse, tables, conds,
											&stay, &moves) &&
			 !gp_slice_join_broadcastable(tables)))
			return NULL;
	}

	return gp_slice_query_copy(parse);
}

/*
 * Replace the whole plan of a colocated join with a scan that runs it on
 * the segments, keeping the sort and the limit of the query here
 */
bool
gp_slice_join_plan(Query *original, PlannedStmt *stmt)
{
	Plan	   *parent = NULL;
	Plan	   *plan = stmt->planTree;
	bool		sort_above = false;
	bool		replicated = true;
	bool		single_segment = false;
	bool		motion = false;
	bool		kept_rows = false;
	GpJoinTable *agg_stay = NULL;
	GpLocus	locus;
	List	   *tables = NIL;
	List	   *conds = NIL;
	ListCell   *lc;
	CustomScan *cscan;

	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit)))
	{
		if (IsA(plan, Sort))
			sort_above = true;
		parent = plan;
		plan = plan->lefttree;
	}
	if (plan == NULL)
		return false;

	/* Non-aggregate plans without a final Sort keep their ordering here */
	if (original->sortClause != NIL && !sort_above && !original->hasAggs)
		return false;

	locus = gp_locus_plan(plan, stmt->rtable, stmt->subplans);
	if (!gp_slice_join_from(original, &tables, &conds))
		return false;
	if (!gp_slice_join_sublinks(original, &tables, &conds))
		return false;
	if (!gp_slice_join_load_policies(original, tables))
		return false;
	if (!gp_slice_join_colocated(original, tables, conds) &&
		(locus.kind == GP_LOCUS_COORDINATOR || original->hasSubLinks) &&
		!(gp_slice_join_derived(original) &&
		  !gp_slice_join_all_inner_levels(tables)))
	{
		{
			bool		motion_ok = gp_slice_join_motion(original, tables, conds,
													 &single_segment, &agg_stay,
													 &kept_rows);

			if (!motion_ok)
				return false;
		}

		motion = true;
	}
	else if (gp_slice_join_type_of(original) == JOIN_FULL &&
			 locus.kind == GP_LOCUS_ONE_SEGMENT && !locus.replicated)
	{
		if (original->hasSubLinks)
			return false;
		if (!gp_slice_join_motion(original, tables, conds, &single_segment,
								  &agg_stay, &kept_rows))
			return false;
		motion = true;
	}
	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
			replicated = false;
	}
	if (single_segment)
		replicated = true;

	/* The columns the segments return are the target list of the plan */
	if (list_length(plan->targetlist) != list_length(original->targetList))
		return false;

	/* An aggregate over the join can be partial even when it is not by a key */
	if (original->hasAggs && !replicated)
	{
		Plan	   *agg = plan;

		if (gp_slice_join_partial(original, stmt, plan, parent, sort_above, motion))
			return true;
		if (gp_slice_join_redistributed_agg(original, stmt, plan, parent,
										 sort_above, tables))
			return true;
		if (original->groupClause == NIL)
			return false;

		if (kept_rows)
		{
			RangeTblRef *ref = linitial_node(RangeTblRef,
											 original->jointree->fromlist);
			RangeTblEntry *rte = rt_fetch(ref->rtindex, original->rtable);
			Index		rti = ref->rtindex;
			ListCell   *lgroup;
			Plan	   *join = plan;

			if (rte->rtekind != RTE_SUBQUERY || rte->subquery == NULL)
				return false;
			foreach(lgroup, original->groupClause)
			{
				SortGroupClause *sc = lfirst_node(SortGroupClause, lgroup);
				TargetEntry *tle = get_sortgroupclause_tle(sc,
														   original->targetList);

				if (!gp_slice_join_stay_vars((Node *) tle->expr, &rti))
					return false;
			}

			/*
			 * The rows of the answer are the rows of the subquery: the
			 * aggregate keeps its place above, and its input, the join
			 * below, becomes the slice of the segments.  The order of the
			 * groups stays with the aggregate and the sort above the join
			 */
			while (join != NULL && (IsA(join, Agg) || IsA(join, Sort) ||
									IsA(join, IncrementalSort) ||
									IsA(join, Limit)))
			{
				parent = join;
				join = join->lefttree;
			}
			if (join == NULL)
				return false;
			cscan = gp_slice_make(join, rte->subquery, replicated, motion,
								  NIL);
			parent->lefttree = &cscan->scan.plan;
			return true;
		}
		else
		{
			if (original->groupClause == NIL)
				return false;
			while (agg != NULL && !IsA(agg, Agg))
			{
				if (agg->righttree != NULL)
					return false;
				agg = agg->lefttree;
			}
			if (agg == NULL ||
				!gp_slice_join_agg_keys((Agg *) agg, tables, original, agg_stay))
				return false;
		}
	}

	/* A failed partial aggregate must keep the original ordering here */
	if (original->sortClause != NIL && !sort_above)
		return false;

	cscan = gp_slice_make(plan, original, replicated, motion, original->sortClause);

	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;

	return true;

}

/*
 * Whether a subquery is answered by one segment: a replicated table for a
 * single-table aggregate, or all replicated tables for a join
 */
static bool
gp_slice_subquery_replicated(Query *subquery, bool is_join)
{
	ListCell   *lc;

	if (!is_join)
	{
		foreach(lc, subquery->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
			GpPolicy	policy;

			if (rte->rtekind == RTE_RELATION &&
				gp_policy_get(rte->relid, &policy) && policy.replicated)
				return true;
		}
		return false;
	}
	else
	{
		List	   *tables = NIL;
		List	   *conds = NIL;

		if (!gp_slice_join_from(subquery, &tables, &conds))
			return false;
		if (!gp_slice_join_sublinks(subquery, &tables, &conds))
			return false;
		if (!gp_slice_join_colocated(subquery, tables, conds))
		{
			GpJoinTable *stay;
			List	   *moves;

			/* Both sides of a full join are broadcast to one segment */
			return !subquery->hasSubLinks && list_length(tables) == 2 &&
				gp_slice_join_type_of(subquery) == JOIN_FULL &&
				(gp_slice_join_redistributable(subquery, tables, conds,
											   &stay, &moves) ||
				 gp_slice_join_broadcastable(tables));
		}
		foreach(lc, tables)
		{
			GpJoinTable *table = lfirst(lc);

			if (!table->policy.replicated)
				return false;
		}
		return true;
	}
}

/*
 * Whether the query is a select over one subquery, with nothing of its own
 * but a projection, a condition, a sort and a limit: whatever the planner put
 * below the sort and the limit is the plan of the subquery alone — an
 * aggregate, a join, or a SubqueryScan the planner chose to keep
 */
static bool
gp_slice_subquery_outer(Query *parse, RangeTblEntry **rte_out)
{
	RangeTblRef *ref;
	RangeTblEntry *rte;

	/* A condition pushed into the subquery may leave hasAggs set here.  A real
	 * aggregate of the outer query is not a unary node and never reaches this
	 * predicate through the walker's unary chain */
	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->distinctClause != NIL || parse->rowMarks != NIL)
		return false;
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return false;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, parse->rtable);
	if (rte->rtekind != RTE_SUBQUERY)
		return false;

	*rte_out = rte;
	return true;
}

/*
 * A subquery in FROM has a locus of its own: if it is a colocated join or
 * a single-table aggregate by the key, it runs on the segments even when
 * the query above it is not colocated at all.  The subquery keeps no
 * limit of its own: a limit taken on every segment is not the limit of
 * the whole.  The planner often inlines the plan of a simple subquery
 * with no SubqueryScan of its own, so the plan below the sort and the
 * limit of the outer query is replaced, whatever node it is.  A plan that
 * answers the ORDER BY by its own order, with no sort on top, is left
 * alone: the replacement would not answer it
 */
static GpLocus
gp_slice_subquery_walk(Query *parse, Plan **planp, bool sort_above)
{
	Plan	   *plan = *planp;
	RangeTblEntry *rte;
	Query	   *subquery;
	Query	   *candidate;
	bool		is_agg;
	bool		replicated;
	bool		motion = false;
	bool		copies = false;
	CustomScan *cscan;
	GpLocus	locus = gp_locus_make(GP_LOCUS_COORDINATOR);

	if (plan == NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);

	/* A condition or projection above the subquery changes rows, not their locus */
	if (IsA(plan, Sort) || IsA(plan, Limit) || IsA(plan, Result) ||
		IsA(plan, SubqueryScan))
	{
		Plan	  **childp = IsA(plan, SubqueryScan) ?
			&((SubqueryScan *) plan)->subplan : &plan->lefttree;
		GpLocus	child = gp_slice_subquery_walk(parse, childp,
							   sort_above || IsA(plan, Sort));
		return gp_locus_unary(plan, child);
	}

	if (parse->sortClause != NIL && !sort_above)
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	if (!gp_slice_subquery_outer(parse, &rte))
		return gp_locus_make(GP_LOCUS_COORDINATOR);
	subquery = rte->subquery;
	if (subquery->limitCount != NULL || subquery->limitOffset != NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);

	candidate = gp_slice_agg_query(subquery);
	is_agg = candidate != NULL;
	if (candidate == NULL)
		candidate = gp_slice_join_query(subquery);
	if (candidate == NULL)
		return gp_locus_make(GP_LOCUS_COORDINATOR);

	/* The columns the segments return are the non-junk outputs of the subquery */
	if (parse->sortClause != NIL)
	{
		if (list_length(plan->targetlist) != list_length(subquery->targetList))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	else
	{
		int			planned = 0;
		int			needed = 0;
		ListCell   *lc;

		/* The planner may prune a subquery output the outer query never reads */
		foreach(lc, candidate->targetList)
		{
			TargetEntry *subtle = lfirst_node(TargetEntry, lc);
			ListCell   *plc;
			bool		returned = false;

			foreach(plc, plan->targetlist)
			{
				TargetEntry *ptle = lfirst_node(TargetEntry, plc);

				if (ptle->resno == subtle->resno && !ptle->resjunk)
				{
					returned = true;
					break;
				}
			}
			if (!returned)
				subtle->resjunk = true;
		}
		foreach(lc, plan->targetlist)
			if (!lfirst_node(TargetEntry, lc)->resjunk)
				planned++;
		foreach(lc, candidate->targetList)
			if (!lfirst_node(TargetEntry, lc)->resjunk)
				needed++;
		if (planned != needed)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	}

	if (is_agg)
	{
		Plan	   *agg = plan;
		Plan	   *below;

		while (agg != NULL && !IsA(agg, Agg))
		{
			if (agg->righttree != NULL)
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			agg = agg->lefttree;
		}
		if (agg == NULL)
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		below = agg->lefttree;
		while (below != NULL && IsA(below, Sort))
			below = below->lefttree;
		if (below == NULL || !gp_is_gather(below) ||
			!gp_slice_agg_locus(candidate, (Agg *) agg, below,
								&replicated, &motion, &locus))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
	}
	else
	{
		List	   *tables = NIL;
		List	   *conds = NIL;
		bool		single_segment = false;
		bool		kept_rows = false;
		GpJoinTable *agg_stay = NULL;

		locus = gp_locus_plan(plan, candidate->rtable, NULL);
		replicated = gp_slice_subquery_replicated(subquery, true);
		copies = replicated;

		if (!gp_slice_join_from(candidate, &tables, &conds))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		if (candidate->hasSubLinks &&
			!gp_slice_join_sublinks(candidate, &tables, &conds))
			return gp_locus_make(GP_LOCUS_COORDINATOR);
		if (!gp_slice_join_colocated(candidate, tables, conds) &&
			(locus.kind == GP_LOCUS_COORDINATOR || candidate->hasSubLinks))
		{
			if (candidate->hasSubLinks && candidate->hasAggs)
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			if (!gp_slice_join_motion(candidate, tables, conds, &single_segment,
									  &agg_stay, &kept_rows))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			motion = true;
		}
		else if (gp_slice_join_type_of(candidate) == JOIN_FULL &&
				 locus.kind == GP_LOCUS_ONE_SEGMENT && !locus.replicated)
		{
			/* A full join that is not colocated still broadcasts both sides */
			if (!gp_slice_join_motion(candidate, tables, conds, &single_segment,
									  &agg_stay, &kept_rows))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
			motion = true;
		}
		replicated = replicated || single_segment;
		if (subquery->hasAggs && !replicated)
		{
			Plan	   *agg = plan;

			while (agg != NULL && !IsA(agg, Agg))
			{
				if (agg->righttree != NULL)
					return gp_locus_make(GP_LOCUS_COORDINATOR);
				agg = agg->lefttree;
			}
			if (agg == NULL ||
				!gp_slice_join_agg_keys((Agg *) agg, tables, subquery, agg_stay))
				return gp_locus_make(GP_LOCUS_COORDINATOR);
		}
	}
	if (is_agg)
		/* The planner puts both the original HAVING and a pushed outer condition
		 * in the aggregate's qual, not necessarily in the subquery's havingQual */
		candidate->havingQual = (Node *) copyObject((Node *) plan->qual);
	cscan = gp_slice_make(plan, candidate, replicated, motion, parse->sortClause);
	*planp = &cscan->scan.plan;
	if (is_agg)
		return locus;
	if (locus.kind != GP_LOCUS_COORDINATOR)
		return locus;
	locus = gp_locus_make(replicated ? GP_LOCUS_ONE_SEGMENT : GP_LOCUS_SEGMENTS);
	locus.replicated = copies;
	return locus;
}

void
gp_slice_subquery_plans(Query *parse, PlannedStmt *stmt)
{
	(void) gp_slice_subquery_walk(parse, &stmt->planTree, false);
}

/* An aggregate of the query, answered by the segments as a column */
typedef struct GpPartialAgg
{
	Aggref	   *aggref;			/* the aggregate of the query */
	Index		pos;			/* its column in the answer of the segments */
} GpPartialAgg;

/* A grouping column of the segments' answer */
typedef struct GpPartialKey
{
	Expr	   *expr;			/* the expression grouped by */
	Index		pos;
	Index		ressortgroupref;	/* its mark in the grouping */
} GpPartialKey;

/* What is collected from a query on the way to its partial form */
/* An average, answered by a partial sum and a partial count */
typedef struct GpPartialAvg
{
	Aggref	   *aggref;			/* the average of the query */
	Expr	   *sum;			/* partial sum over its argument */
	Expr	   *count;			/* partial count over its argument */
	int			pos_sum;
	int			pos_count;
} GpPartialAvg;

typedef struct GpPartialState
{
	List	   *aggs;			/* GpPartialAgg */
	List	   *avgs;			/* GpPartialAvg */
	List	   *keys;			/* GpPartialKey */
	bool		allow_avg;		/* the segments may answer an average */
	Index		npos;			/* columns of the answer so far */
	int			nextaggno;		/* aggno for the next combining aggregate */
	Plan	   *below;			/* old input, for resolving planned join Vars */
	bool		ok;				/* false once anything unsplittable is met */
} GpPartialState;

/*
 * Whether the segments can answer an aggregate with a column that the
 * coordinator finishes by the same aggregate again: min and max of any
 * type, and sum of a type it adds up in, whose argument, result and
 * transition are all of one type.  count counts the partial columns
 * instead of adding them, and the average divides, so neither can be
 * combined by itself
 */
static bool
gp_partial_splittable(Aggref *aggref)
{
	HeapTuple	agg_tup;
	HeapTuple	proc_tup;
	Form_pg_aggregate aggform;
	Form_pg_proc procform;
	const char *name;
	bool		ok;

	if (aggref->aggkind != AGGKIND_NORMAL || aggref->aggstar ||
		aggref->aggvariadic || aggref->aggdirectargs != NIL ||
		aggref->aggdistinct != NIL || aggref->aggorder != NIL ||
		aggref->aggfilter != NULL ||
		list_length(aggref->args) != 1 ||
		list_length(aggref->aggargtypes) != 1)
		return false;

	agg_tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(agg_tup))
		return false;
	aggform = (Form_pg_aggregate) GETSTRUCT(agg_tup);
	ok = OidIsValid(aggform->aggcombinefn);
	ReleaseSysCache(agg_tup);
	if (!ok)
		return false;

	proc_tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(proc_tup))
		return false;
	procform = (Form_pg_proc) GETSTRUCT(proc_tup);
	name = NameStr(procform->proname);
	ok = (strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
		  strcmp(name, "sum") == 0 || strcmp(name, "avg") == 0);
	ReleaseSysCache(proc_tup);
	if (!ok)
		return false;

	/*
	 * An average moves through a partial sum and a partial count, so its
	 * result type may differ from the type of its argument
	 */
	return strcmp(name, "avg") == 0 ||
		linitial_oid(aggref->aggargtypes) == aggref->aggtype;
}

/* Collect the aggregates of an expression tree, in the order they appear */
/* The OID of a pg_catalog function of one argument, or InvalidOid */
static Oid
gp_partial_func(const char *name, Oid argtype)
{
	Oid			funcoid;

	funcoid = LookupFuncName(list_make2(makeString((char *) "pg_catalog"),
										makeString((char *) name)),
							 1, &argtype, true);
	return funcoid;
}

/*
 * The transition type the executor reads from the aggregate itself: the
 * planner resolves it in the tree it plans, and a copy taken before the
 * planner carries none
 */
static void
gp_partial_transtype(Aggref *aggref)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));

	if (HeapTupleIsValid(tup))
	{
		Form_pg_aggregate aggform = (Form_pg_aggregate) GETSTRUCT(tup);
		Oid			inputTypes[FUNC_MAX_ARGS];

		get_aggregate_argtypes(aggref, inputTypes);
		aggref->aggtranstype = resolve_aggregate_transtype(aggref->aggfnoid,
														   aggform->aggtranstype,
														   inputTypes,
														   list_length(aggref->args));
		ReleaseSysCache(tup);
	}
}

/* The partial column of an average: its sum, or the count of its argument */
static Expr *
gp_partial_avg_column(Aggref *avgref, Oid funcoid, Oid resulttype)
{
	Aggref	   *agg = copyObject(avgref);

	agg->aggfnoid = funcoid;
	agg->aggtype = resulttype;
	gp_partial_transtype(agg);
	return (Expr *) agg;
}

/* The expression cast to another type by the function of its cast, or NULL */
static Expr *
gp_partial_cast(Expr *expr, Oid fromtype, Oid totype)
{
	HeapTuple	tup = SearchSysCache2(CASTSOURCETARGET,
									  ObjectIdGetDatum(fromtype),
									  ObjectIdGetDatum(totype));
	Form_pg_cast castform;
	FuncExpr   *func;

	if (!HeapTupleIsValid(tup))
		return NULL;
	castform = (Form_pg_cast) GETSTRUCT(tup);
	if (!OidIsValid(castform->castfunc))
	{
		ReleaseSysCache(tup);
		return NULL;
	}
	func = makeNode(FuncExpr);
	func->funcid = castform->castfunc;
	func->funcresulttype = totype;
	func->args = list_make1(expr);
	ReleaseSysCache(tup);
	return (Expr *) func;
}

/*
 * The final answer of an average: the sum of the partial sums divided by
 * the sum of the partial counts, NULL when no row had a value.  The two
 * combining aggregates take the numbers of the state the executor reads
 * from the plan, as every aggregate of the final target does
 */
static Expr *
gp_partial_avg_final(GpPartialAvg *avg, GpPartialState *state)
{
	Aggref	   *sum;
	Aggref	   *count;
	Expr	   *divisor;
	Expr	   *zero;
	FuncExpr   *eq;
	CaseWhen   *when;
	CaseExpr   *caseexpr;
	FuncExpr   *divide;
	Var		   *var;
	Oid			casetype;
	Oid			eqop;
	Oid			sumoid = gp_partial_func("sum",
							  exprType((Node *) avg->sum));
	Oid			cntsumoid = gp_partial_func("sum",
							  exprType((Node *) avg->count));
	Oid			opno;

	if (!OidIsValid(sumoid) || !OidIsValid(cntsumoid))
		return NULL;

	var = makeVar(OUTER_VAR, avg->pos_sum, exprType((Node *) avg->sum),
				  -1, 0, 0);
	sum = makeNode(Aggref);
	sum->aggfnoid = sumoid;
	sum->aggtype = get_func_rettype(sumoid);
	sum->aggkind = AGGKIND_NORMAL;
	sum->args = list_make1(makeTargetEntry((Expr *) var, 1, NULL, false));
	sum->aggargtypes = list_make1_oid(exprType((Node *) var));
	sum->aggsplit = AGGSPLIT_SIMPLE;
	gp_partial_transtype(sum);
	sum->aggno = state->nextaggno;
	sum->aggtransno = state->nextaggno;
	state->nextaggno++;

	/*
	 * The sum of the partial counts: a sum over a whole-number column
	 * answers in numeric, whatever the catalog keeps in the transition
	 */
	var = makeVar(OUTER_VAR, avg->pos_count, exprType((Node *) avg->count),
				  -1, 0, 0);
	count = makeNode(Aggref);
	count->aggfnoid = cntsumoid;
	count->aggtype = get_func_rettype(cntsumoid);
	count->aggkind = AGGKIND_NORMAL;
	count->args = list_make1(makeTargetEntry((Expr *) var, 1, NULL, false));
	count->aggargtypes = list_make1_oid(exprType((Node *) var));
	count->aggsplit = AGGSPLIT_SIMPLE;
	gp_partial_transtype(count);
	count->aggno = state->nextaggno;
	count->aggtransno = state->nextaggno;
	state->nextaggno++;

	/*
	 * The guard against the zero count: there is no nullif function in
	 * the catalog, NULLIF is the sugar of the parser, so the case it
	 * stands for is written out
	 */
	eqop = LookupOperName(NULL, list_make1(makeString((char *) "=")),
						  count->aggtype, count->aggtype, true, -1);
	if (!OidIsValid(eqop))
		return NULL;
	zero = gp_partial_cast((Expr *) makeConst(INT8OID, -1, InvalidOid, 8,
											  Int64GetDatum(0), false, true),
						   INT8OID, count->aggtype);
	if (zero == NULL)
		return NULL;

	/*
	 * The divide: by the sum of the counts in numeric where the sum
	 * divides by numeric, else in the type of the sum through the cast of
	 * the count sum, as the average of an interval divides by its whole
	 * count
	 */
	opno = LookupOperName(NULL, list_make1(makeString((char *) "/")),
						  sum->aggtype, count->aggtype, true, -1);
	if (OidIsValid(opno))
	{
		divisor = (Expr *) count;
		casetype = count->aggtype;
	}
	else
	{
		opno = LookupOperName(NULL, list_make1(makeString((char *) "/")),
							  sum->aggtype, sum->aggtype, true, -1);
		if (OidIsValid(opno))
			casetype = sum->aggtype;
		else
		{
			opno = LookupOperName(NULL, list_make1(makeString((char *) "/")),
								  sum->aggtype, INT8OID, true, -1);
			if (!OidIsValid(opno))
				return NULL;
			casetype = INT8OID;
		}
		divisor = gp_partial_cast((Expr *) copyObject(count), count->aggtype,
								  casetype);
		if (divisor == NULL)
			return NULL;
	}

	/*
	 * The answer must be of the type of the average itself: a divide that
	 * answers another type would hand the executor a value of another type
	 */
	if (sum->aggtype != avg->aggref->aggtype)
		return NULL;

	eq = makeNode(FuncExpr);
	eq->funcid = get_opcode(eqop);
	eq->funcresulttype = BOOLOID;
	eq->args = list_make2((Expr *) copyObject(count), zero);
	when = makeNode(CaseWhen);
	when->expr = (Expr *) eq;
	when->result = (Expr *) makeNullConst(casetype, -1, InvalidOid);
	caseexpr = makeNode(CaseExpr);
	caseexpr->casetype = casetype;
	caseexpr->casecollid = InvalidOid;
	caseexpr->args = list_make1(when);
	caseexpr->defresult = divisor;

	divide = makeNode(FuncExpr);
	divide->funcid = get_opcode(opno);
	divide->funcresulttype = sum->aggtype;
	divide->args = list_make2(sum, caseexpr);
	return (Expr *) divide;
}

static bool
gp_partial_walker(Node *node, GpPartialState *state)
{
	ListCell   *lc;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		GpPartialAgg *partial;

		if (!gp_partial_splittable(aggref))
		{
			state->ok = false;
			return true;
		}
		foreach(lc, state->aggs)
		{
			partial = lfirst(lc);
			if (equal(partial->aggref, aggref))
				return false;
		}
		foreach(lc, state->avgs)
		{
			GpPartialAvg *pa = lfirst(lc);

			if (equal(pa->aggref, aggref))
				return false;
		}
		{
			HeapTuple	tup = SearchSysCache1(PROCOID,
											  ObjectIdGetDatum(aggref->aggfnoid));
			bool		isavg;

			if (!HeapTupleIsValid(tup))
			{
				state->ok = false;
				return true;
			}
			isavg = strcmp(NameStr(((Form_pg_proc) GETSTRUCT(tup))->proname),
						   "avg") == 0;
			ReleaseSysCache(tup);
			if (isavg && !state->allow_avg)
			{
				state->ok = false;
				return true;
			}
			if (isavg)
			{
				GpPartialAvg *pa = palloc0(sizeof(GpPartialAvg));
				Oid			sumoid = gp_partial_func("sum",
									 linitial_oid(aggref->aggargtypes));
				Oid			cntoid = gp_partial_func("count", ANYOID);
				if (!OidIsValid(sumoid) || !OidIsValid(cntoid))
				{
					state->ok = false;
					return true;
				}
				pa->aggref = aggref;
				pa->sum = gp_partial_avg_column(aggref, sumoid,
												get_func_rettype(sumoid));
				pa->count = gp_partial_avg_column(aggref, cntoid,
												  get_func_rettype(cntoid));
				pa->pos_sum = ++state->npos;
				pa->pos_count = ++state->npos;
				state->avgs = lappend(state->avgs, pa);
				return false;
			}
		}
		partial = palloc0(sizeof(GpPartialAgg));
		partial->aggref = aggref;
		state->npos++;
		partial->pos = state->npos;
		state->aggs = lappend(state->aggs, partial);
		return false;
	}
	return expression_tree_walker(node, gp_partial_walker, state);
}

/* Replace a group key with its column, an aggregate with its combining one */
static Node *
gp_partial_mutator(Node *node, GpPartialState *state)
{
	ListCell   *lc;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && state->below != NULL)
	{
		Plan	   *source = state->below;

		while (IsA(node, Var) &&
			   (((Var *) node)->varno == OUTER_VAR ||
				((Var *) node)->varno == INNER_VAR))
		{
			Var		   *var = (Var *) node;
			TargetEntry *tle;

			if (var->varattno <= 0)
				break;
			tle = list_nth(source->targetlist, var->varattno - 1);
			node = (Node *) tle->expr;
			if (IsA(node, Var) &&
				((Var *) node)->varno == INNER_VAR && source->righttree != NULL)
				source = source->righttree;
			else if (IsA(node, Var) &&
					 ((Var *) node)->varno == OUTER_VAR && source->lefttree != NULL)
				source = source->lefttree;
		}
	}
	foreach(lc, state->keys)
	{
		GpPartialKey *key = lfirst(lc);

		if (equal(key->expr, node) ||
			(IsA(key->expr, Var) && IsA(node, Var) &&
			 ((Var *) key->expr)->varno == ((Var *) node)->varno &&
			 ((Var *) key->expr)->varattno == ((Var *) node)->varattno))
			return (Node *) makeVar(OUTER_VAR, key->pos,
									exprType(node), exprTypmod(node),
									exprCollation(node), 0);
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Aggref	   *combine;
		Var		   *var;

		foreach(lc, state->avgs)
		{
			GpPartialAvg *pa = lfirst(lc);

			if (equal(pa->aggref, aggref))
			{
				Expr	   *final = gp_partial_avg_final(pa, state);

				if (final == NULL)
				{
					state->ok = false;
					return (Node *) copyObject(aggref);
				}
				return (Node *) final;
			}
		}
		foreach(lc, state->aggs)
		{
			GpPartialAgg *partial = lfirst(lc);

			if (!equal(partial->aggref, aggref))
				continue;
			combine = copyObject(aggref);
			var = makeVar(OUTER_VAR, partial->pos, aggref->aggtype,
						  -1, aggref->aggcollid, 0);
			combine->args = list_make1(makeTargetEntry((Expr *) var, 1,
													   NULL, false));
			combine->aggargtypes = list_make1_oid(aggref->aggtype);
			combine->aggsplit = AGGSPLIT_SIMPLE;
			gp_partial_transtype(combine);
			combine->aggno = state->nextaggno;
			combine->aggtransno = state->nextaggno;
			state->nextaggno++;
			return (Node *) combine;
		}
		state->ok = false;
		return node;
	}
	return expression_tree_mutator(node, gp_partial_mutator, state);
}

/* A final aggregate may read only the columns of the segments' answer */
static bool
gp_partial_final_vars(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno != OUTER_VAR)
		return true;
	return expression_tree_walker(node, gp_partial_final_vars, context);
}

/* Finish a partial aggregate over the slice built from its remote query */
static bool
gp_slice_partial_finish(Agg *agg, GpPartialState *state,
						Query *remote, bool motion,
						List *final_target, List *final_qual)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *below = agg->plan.lefttree;
	List	   *tlist = NIL;
	ListCell   *lc;
	int			pos = 0;

	foreach(lc, remote->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *out;
		Var		   *var;

		pos++;
		var = makeVar(INDEX_VAR, pos, exprType((Node *) tle->expr),
					  exprTypmod((Node *) tle->expr),
					  exprCollation((Node *) tle->expr), 0);
		out = makeTargetEntry((Expr *) var, tle->resno, NULL, false);
		tlist = lappend(tlist, out);
	}
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.startup_cost = agg->plan.lefttree->startup_cost;
	cscan->scan.plan.total_cost = agg->plan.lefttree->total_cost;
	cscan->scan.plan.plan_rows = agg->plan.lefttree->plan_rows;
	cscan->scan.plan.plan_width = agg->plan.lefttree->plan_width;
	cscan->custom_scan_tlist = copyObject(remote->targetList);
	cscan->custom_private = list_make4(makeString(pg_get_querydef(remote, false)),
								   makeBoolean(false), makeBoolean(motion),
								   makeBoolean(gp_slice_autonomous(remote)));
	cscan->methods = &gp_slice_plan_methods;

	/* The final aggregate over the columns of the answer */
	state->ok = true;
	state->below = below;
	tlist = NIL;
	foreach(lc, final_target)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		TargetEntry *out;
		Node	   *expr = gp_partial_mutator((Node *) tle->expr, state);

		if (!state->ok)
			return false;
		out = makeTargetEntry((Expr *) expr, tle->resno,
							  tle->resname ? pstrdup(tle->resname) : NULL,
							  tle->resjunk);
		out->ressortgroupref = tle->ressortgroupref;
		tlist = lappend(tlist, out);
	}
	{
		List	   *qual = (List *) gp_partial_mutator((Node *) final_qual, state);

		if (!state->ok)
			return false;

		/*
		 * An unmatched key leaves a Var of the old input behind, and the
		 * executor asserts against it.  Planned grouping of a query whose
		 * grouping moved into a grouping rangetable entry is such a case:
		 * refuse the rewrite, the coordinator plan answers it as before.
		 */
		if (gp_partial_final_vars((Node *) tlist, NULL) ||
			gp_partial_final_vars((Node *) qual, NULL))
			return false;
		agg->plan.qual = qual;
	}

	{
		AttrNumber *oldcols = agg->grpColIdx;
		Oid		   *oldoperators = agg->grpOperators;
		Oid		   *oldcollations = agg->grpCollations;
		AttrNumber *grpColIdx = palloc_array(AttrNumber, list_length(state->keys));
		Oid		   *operators = palloc_array(Oid, list_length(state->keys));
		Oid		   *collations = palloc_array(Oid, list_length(state->keys));
		int			i = 0;

		foreach(lc, state->keys)
		{
			GpPartialKey *key = lfirst(lc);
			int			j;

			grpColIdx[i] = key->pos;
			for (j = 0; j < agg->numCols; j++)
			{
				TargetEntry *old = list_nth(below->targetlist, oldcols[j] - 1);

				if (old->ressortgroupref == key->ressortgroupref)
					break;
			}
			if (j == agg->numCols)
				return false;
			operators[i] = oldoperators[j];
			collations[i] = oldcollations[j];
			i++;
		}
		agg->grpColIdx = grpColIdx;
		agg->grpOperators = operators;
		agg->grpCollations = collations;
	}
	agg->aggsplit = AGGSPLIT_SIMPLE;
	agg->plan.targetlist = tlist;
	agg->plan.lefttree = &cscan->scan.plan;
	return true;
}

/*
 * Answer an aggregate query in two steps, when the whole of it cannot run
 * on the segments: the segments aggregate their own rows — grouped by the
 * grouping of the query, or all rows together when there is none — and the
 * coordinator finishes the aggregate over the columns of every segment.
 * The final aggregate is the same aggregate again, which only min, max and
 * sum of a closing type answer truly; the grouping columns go to the
 * segments as columns of their own, and HAVING stays on the coordinator,
 * for a group may pass it only as a whole.
 *
 * The plan is the aggregate with its gather replaced by the slice: the
 * target list and the qual of the aggregate are rebuilt over the columns
 * of the answer, and the grouping columns are renumbered to them.
 */
void
gp_slice_partial_plan(Query *parse, PlannedStmt *stmt)
{
	Plan	   *plan = stmt->planTree;
	RangeTblEntry *rte_base;
	GpPolicy	policy;
	GpPartialState state;
	RangeTblRef *ref;
	ListCell   *lc;
	Agg		   *agg;
	Query	   *remote;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		!parse->hasAggs)
		return;
	if (parse->hasWindowFuncs || parse->hasSubLinks ||
		parse->setOperations != NULL || parse->cteList != NIL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL ||
		parse->distinctClause != NIL)
		return;

	/* One hashed table, whose rows are spread over the segments */
	if (!IsA(parse->jointree, FromExpr) ||
		list_length(parse->jointree->fromlist) != 1 ||
		!IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return;
	ref = linitial_node(RangeTblRef, parse->jointree->fromlist);
	rte_base = rt_fetch(ref->rtindex, parse->rtable);
	if (rte_base->rtekind != RTE_RELATION ||
		!gp_policy_get(rte_base->relid, &policy) || policy.replicated)
		return;

	/* The grouping columns lead the answer of the segments */
	state.aggs = NIL;
	state.avgs = NIL;
	state.keys = NIL;
	state.allow_avg = true;
	state.npos = 0;
	state.nextaggno = 0;
	state.below = NULL;
	state.ok = true;
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sc, parse->targetList);
		GpPartialKey *key = palloc0(sizeof(GpPartialKey));

		key->expr = tle->expr;
		state.npos++;
		key->pos = state.npos;
		key->ressortgroupref = tle->ressortgroupref;
		state.keys = lappend(state.keys, key);
	}

	/* The aggregates must be answerable by the segments */
	gp_partial_walker((Node *) parse->targetList, &state);
	if (state.ok)
		gp_partial_walker((Node *) parse->havingQual, &state);
	if (!state.ok || (state.aggs == NIL && state.avgs == NIL))
	return;

	/* Find the aggregate of the plan, below the sort and the limit of it */
	while (plan != NULL && (IsA(plan, Sort) || IsA(plan, Limit) ||
							IsA(plan, Result)))
		plan = plan->lefttree;
	if (plan == NULL || !IsA(plan, Agg))
		return;
	agg = (Agg *) plan;
	if (!gp_is_gather(agg->plan.lefttree))
		return;
	if (agg->numCols != list_length(parse->groupClause))
		return;

	/*
	 * The query of the segments: the grouping columns and the aggregates
	 * as its target list, with HAVING left out, for it sees whole groups
	 * only on the coordinator
	 */
	remote = copyObject(parse);
	/* The planner leaves a list of implicit-AND conditions in the jointree */
	if (remote->jointree->quals != NULL &&
		IsA(remote->jointree->quals, List))
		remote->jointree->quals =
			(Node *) makeBoolExpr(AND_EXPR, (List *) remote->jointree->quals, -1);
	remote->targetList = NIL;
	foreach(lc, state.keys)
	{
		GpPartialKey *key = lfirst(lc);
		TargetEntry *tle = makeTargetEntry(copyObject(key->expr),
										   key->pos, NULL, false);

		tle->ressortgroupref = key->ressortgroupref;
		remote->targetList = lappend(remote->targetList, tle);
	}
	foreach(lc, state.aggs)
	{
		GpPartialAgg *partial = lfirst(lc);
		TargetEntry *tle = makeTargetEntry((Expr *) copyObject(partial->aggref),
										   partial->pos, NULL, false);

		remote->targetList = lappend(remote->targetList, tle);
	}
	foreach(lc, state.avgs)
	{
		GpPartialAvg *pa = lfirst(lc);

		remote->targetList = lappend(remote->targetList,
									 makeTargetEntry((Expr *) copyObject(pa->sum),
													 pa->pos_sum, NULL, false));
		remote->targetList = lappend(remote->targetList,
									 makeTargetEntry((Expr *) copyObject(pa->count),
													 pa->pos_count, NULL, false));
	}
	remote->havingQual = NULL;
	remote->sortClause = NIL;
	remote->limitCount = NULL;
	remote->limitOffset = NULL;

	{
		ListCell   *l1;
		int			i = 0;
		bool		unresolved = false;

		foreach(l1, remote->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, l1);
			ListCell   *l2;
			int			j = 0;

			i++;
			if (rte->rtekind != RTE_SUBQUERY || rte->subquery != NULL)
				continue;
			foreach(l2, remote->rtable)
			{
				RangeTblEntry *rte2 = lfirst_node(RangeTblEntry, l2);
				bool		same = true;
				ListCell   *n1;
				ListCell   *n2;
				j++;
				if (j == i || rte2->rtekind != RTE_RELATION ||
					list_length(rte2->eref->colnames) !=
					list_length(rte->eref->colnames))
					continue;
				forboth(n1, rte2->eref->colnames, n2, rte->eref->colnames)
				{
					if (strcmp(strVal(lfirst(n1)), strVal(lfirst(n2))) != 0)
					{
						same = false;
						break;
					}
				}
				if (!same)
					continue;
				rte->rtekind = RTE_RELATION;
				rte->relid = rte2->relid;
				rte->relkind = rte2->relkind;
				rte->rellockmode = rte2->rellockmode;
				break;
			}
			if (rte->rtekind == RTE_SUBQUERY)
				unresolved = true;
		}
		if (unresolved)
			return;
	}

	(void) gp_slice_partial_finish(agg, &state, remote, false,
								  parse->targetList, (List *) parse->havingQual);
}

/*
 * A sublink of the query is an aggregate over one hashed table that the
 * segments answer with partial columns: a copy of its query is collected
 * for the SQL of the segments, keyed by the relid of that table.  Sublinks
 * of other kinds are skipped: the planner answers them itself
 */
typedef struct GpInitplanCand
{
	Oid			relid;			/* the table the sublink aggregates over */
	Aggref	   *aggref;			/* its aggregate, for pairing with the plan */
	Query	   *query;			/* copy of the subselect for the segments */
	bool		taken;			/* an init plan already pairs with it */
} GpInitplanCand;

/*
 * The context of the walk: the tree walkers hand it over by pointer, so
 * the candidates collected in the depth of the tree are not lost
 */
typedef struct GpInitplanContext
{
	List	   *cands;			/* the candidates collected so far */
} GpInitplanContext;

/* Whether the subselect reads a column of a query above it */
static bool
gp_slice_initplan_outer_vars(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return ((Var *) node)->varlevelsup != 0;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, gp_slice_initplan_outer_vars,
								 context, 0);
	return expression_tree_walker(node, gp_slice_initplan_outer_vars, context);
}

static bool
gp_slice_initplan_query_walker(Node *node, void *context)
{
	GpInitplanContext *ctx = (GpInitplanContext *) context;
	ListCell   *lc;

	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink   *sub = (SubLink *) node;
		RangeTblRef *ref;
		RangeTblEntry *rte;
		TargetEntry *tle;
		GpInitplanCand *cand;
		GpPolicy	policy;
		Query	   *sq;

		if (sub->subLinkType != EXPR_SUBLINK || !IsA(sub->subselect, Query))
			return false;
		sq = (Query *) sub->subselect;

		/*
		 * The candidate is one aggregate over one hashed table: the
		 * segments answer a partial column pair of it.  No grouping: the
		 * partial columns aggregate all rows together
		 */
		if (sq->commandType != CMD_SELECT || sq->hasWindowFuncs ||
			sq->hasSubLinks || sq->setOperations != NULL ||
			sq->cteList != NIL || sq->groupingSets != NIL ||
			sq->groupClause != NIL || sq->havingQual != NULL ||
			sq->rowMarks != NIL || sq->distinctClause != NIL ||
			sq->limitCount != NULL || sq->limitOffset != NULL)
			return false;
		if (!IsA(sq->jointree, FromExpr) ||
			list_length(((FromExpr *) sq->jointree)->fromlist) != 1 ||
			!IsA(linitial(((FromExpr *) sq->jointree)->fromlist),
				 RangeTblRef))
			return false;
		ref = linitial_node(RangeTblRef,
							((FromExpr *) sq->jointree)->fromlist);
		rte = rt_fetch(ref->rtindex, sq->rtable);

		/*
		 * A replicated table is answered whole by one segment already, and
		 * a sum of the partial columns of all segments would count its rows
		 * more than once
		 */
		if (rte->rtekind != RTE_RELATION || rte->relid == 0 ||
			!gp_policy_get(rte->relid, &policy) || policy.replicated)
			return false;
		if (list_length(sq->targetList) != 1)
			return false;
		tle = linitial_node(TargetEntry, sq->targetList);
		if (!IsA(tle->expr, Aggref) ||
			!gp_partial_splittable((Aggref *) tle->expr))
			return false;
		if (gp_slice_initplan_outer_vars((Node *) sq, NULL))
			return false;
		foreach(lc, ctx->cands)
		{
			cand = lfirst(lc);

			if (cand->relid == rte->relid)
				return false;
		}
		cand = palloc0(sizeof(GpInitplanCand));
		cand->relid = rte->relid;
		cand->aggref = (Aggref *) tle->expr;
		cand->query = copyObject(sq);
		ctx->cands = lappend(ctx->cands, cand);
		return true;
	}
	if (IsA(node, Query))
	{
		Query	   *q = (Query *) node;
		ListCell   *rt;

		/*
		 * The sublinks live in the derived subqueries of the range table:
		 * query_tree_walker does not descend there by itself.  The return
		 * of the descent says nothing: a sublink that does not become a
		 * candidate only stays with the planner
		 */
		foreach(rt, q->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, rt);

			if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
				gp_slice_initplan_query_walker((Node *) rte->subquery,
											   context);
		}
		return query_tree_walker((Query *) node,
								 gp_slice_initplan_query_walker, context, 0);
	}
	return expression_tree_walker(node, gp_slice_initplan_query_walker,
								  context);
}

/* Collect the pushable aggregate sublinks of the query */
List *
gp_slice_initplan_query(Query *parse)
{
	GpInitplanContext ctx;

	if (!IsUnderPostmaster || parse->commandType != CMD_SELECT ||
		parse->hasWindowFuncs || parse->setOperations != NULL ||
		parse->groupingSets != NIL || parse->rowMarks != NIL)
	{
		return NIL;
	}

	ctx.cands = NIL;
	gp_slice_initplan_query_walker((Node *) parse, &ctx);
	return ctx.cands;
}

/*
 * An init plan paired with its candidate: the subplan aggregate of the
 * init plan is rewritten over the slice of the candidate
 */
typedef struct GpInitplanPair
{
	Agg		   *agg;			/* the subplan aggregate of the init plan */
	GpInitplanCand *cand;		/* the candidate over the same table */
} GpInitplanPair;

/* Every plan node with init plans: the tree, both branches, the subqueries */
static void
gp_slice_initplan_plans(Plan *plan, List **plans)
{
	if (plan == NULL)
		return;
	if (plan->initPlan != NIL)
		*plans = lappend(*plans, plan);
	gp_slice_initplan_plans(plan->lefttree, plans);
	gp_slice_initplan_plans(plan->righttree, plans);
	if (IsA(plan, SubqueryScan))
		gp_slice_initplan_plans(((SubqueryScan *) plan)->subplan, plans);
}

/*
 * The aggregate of the init plan pairs with the candidate: the same
 * aggregate over the same column of the table.  The aggregate of the plan
 * reads the gather alone: its argument is a column of the table the gather
 * scans.  A grouping or a HAVING leaves the init plan with the planner
 */
static bool
gp_slice_initplan_pairs(GpInitplanCand *cand, Agg *agg, Plan *below)
{
	TargetEntry *tle;
	TargetEntry *parg;
	TargetEntry *carg;
	Aggref	   *aggref;
	Var		   *arg;
	Var		   *cexpr;

	if (agg->numCols != 0 || agg->plan.qual != NIL ||
		list_length(agg->plan.targetlist) != 1)
		return false;
	tle = linitial_node(TargetEntry, agg->plan.targetlist);
	if (!IsA(tle->expr, Aggref))
		return false;
	aggref = (Aggref *) tle->expr;
	if (aggref->aggfnoid != cand->aggref->aggfnoid ||
		list_length(aggref->args) != 1 ||
		list_length(cand->aggref->args) != 1)
		return false;
	parg = linitial_node(TargetEntry, aggref->args);
	carg = linitial_node(TargetEntry, cand->aggref->args);
	if (!IsA(parg->expr, Var) || !IsA(carg->expr, Var))
		return false;
	arg = (Var *) parg->expr;

	/*
	 * The aggregate reads the output of the gather, and its variable names
	 * a column of that output: resolve it down to a column of the scan
	 */
	while (arg->varno == OUTER_VAR || arg->varno == INNER_VAR)
	{
		TargetEntry *itl;

		if (arg->varattno <= 0 || arg->varattno > list_length(below->targetlist))
			return false;
		itl = list_nth(below->targetlist, arg->varattno - 1);
		if (!IsA(itl->expr, Var))
			return false;
		arg = (Var *) itl->expr;
	}

	/*
	 * The variable of the plan names the scanned table by the number in
	 * the rtable of the whole statement, and the variable of the query
	 * names it by the number in the rtable of the subselect, so only the
	 * column and the type are compared
	 */
	cexpr = (Var *) carg->expr;
	if (arg->varno != ((Scan *) below)->scanrelid ||
		arg->varattno != cexpr->varattno ||
		arg->vartype != cexpr->vartype)
		return false;
	return true;
}

/*
 * Rewrite the init plans of the query: an init plan that aggregates over
 * the gather of the table named by a collected candidate is answered by
 * the segments with partial columns, and the final aggregate of the init
 * plan reads the columns of their answer.  Every init plan must find its
 * own candidate: one left without a candidate, or paired with an aggregate
 * of another shape, leaves the whole query with the coordinator
 */
bool
gp_slice_initplan_plan(List *candidates, PlannedStmt *stmt)
{
	List	   *plans = NIL;
	List	   *pairs = NIL;
	ListCell   *lc;
	ListCell   *lc2;
	ListCell   *lc3;

	if (stmt->planTree == NULL)
		return false;
	gp_slice_initplan_plans(stmt->planTree, &plans);

	/*
	 * The first pass pairs every init plan with an unused candidate: two
	 * init plans over one table would both take the one candidate, and the
	 * second has no answer of its own
	 */
	foreach(lc, plans)
	{
		Plan	   *node = lfirst(lc);

		foreach(lc2, node->initPlan)
		{
			SubPlan    *ip = castNode(SubPlan, lfirst(lc2));
			Plan	   *sub;
			Agg		   *agg;
			Plan	   *below;
			RangeTblEntry *rte;
			GpInitplanCand *cand = NULL;
			GpInitplanPair *pair;
			Oid			relid;

			if (!ip->isInitPlan || ip->plan_id <= 0 ||
				ip->plan_id > list_length(stmt->subplans))
				return false;
			sub = list_nth(stmt->subplans, ip->plan_id - 1);
			if (!IsA(sub, Agg))
				return false;
			agg = (Agg *) sub;
			below = agg->plan.lefttree;
			if (below == NULL || !gp_is_gather(below))
				return false;
			if (((Scan *) below)->scanrelid <= 0 ||
				((Scan *) below)->scanrelid > list_length(stmt->rtable))
				return false;
			rte = rt_fetch(((Scan *) below)->scanrelid, stmt->rtable);
			if (rte->rtekind != RTE_RELATION || rte->relid == 0)
				return false;
			relid = rte->relid;
			foreach(lc3, candidates)
			{
				GpInitplanCand *c = lfirst(lc3);

				if (!c->taken && c->relid == relid)
				{
					cand = c;
					break;
				}
			}
			if (cand == NULL || !gp_slice_initplan_pairs(cand, agg, below))
				return false;
			cand->taken = true;
			pair = palloc(sizeof(GpInitplanPair));
			pair->agg = agg;
			pair->cand = cand;
			pairs = lappend(pairs, pair);
		}
	}

	/*
	 * The second pass rewrites every paired init plan in place: the subplan
	 * aggregate stays in the subplans list, its input becomes the slice of
	 * the partial columns, and its target list reads them
	 */
	foreach(lc, pairs)
	{
		GpInitplanPair *pair = lfirst(lc);
		Agg		   *agg = pair->agg;
		GpInitplanCand *cand = pair->cand;
		GpPartialState state;
		Query	   *remote;
		Expr	   *final;
		TargetEntry *ptle;
		TargetEntry *out;
		ListCell   *lc4;

		state.ok = true;
		state.aggs = NIL;
		state.avgs = NIL;
		state.keys = NIL;
		state.allow_avg = true;
		state.npos = 0;
		state.nextaggno = 0;
		state.below = NULL;

		remote = copyObject(cand->query);
		gp_partial_walker((Node *) remote->targetList, &state);
		if (!state.ok || (state.avgs == NIL && state.aggs == NIL))
			return false;

		/*
		 * The final target of the aggregate is the aggregate of the copy:
		 * the finish matches the target entries against the aggregates of
		 * the copy, and the planner has rewritten the original in place
		 * since the collection
		 */
		if (state.avgs != NIL)
			final = (Expr *) ((GpPartialAvg *) lfirst(list_head(state.avgs)))->aggref;
		else
			final = (Expr *) ((GpPartialAgg *) lfirst(list_head(state.aggs)))->aggref;
		remote->targetList = NIL;
		foreach(lc4, state.aggs)
		{
			GpPartialAgg *partial = lfirst(lc4);

			remote->targetList = lappend(remote->targetList,
										 makeTargetEntry((Expr *) copyObject(partial->aggref),
														 partial->pos, NULL,
														 false));
		}
		foreach(lc4, state.avgs)
		{
			GpPartialAvg *pa = lfirst(lc4);

			remote->targetList = lappend(remote->targetList,
										 makeTargetEntry((Expr *) copyObject(pa->sum),
														 pa->pos_sum, NULL, false));
			remote->targetList = lappend(remote->targetList,
										 makeTargetEntry((Expr *) copyObject(pa->count),
														 pa->pos_count, NULL, false));
		}

		/*
		 * The final target of the aggregate is the aggregate of the
		 * candidate itself: the finish matches it against its copy and
		 * answers with the columns of the segments
		 */
		ptle = linitial_node(TargetEntry, agg->plan.targetlist);
		out = makeTargetEntry(final, ptle->resno,
							  ptle->resname, ptle->resjunk);
		if (!gp_slice_partial_finish(agg, &state, remote, false,
									 list_make1(out), NIL))
			return false;
	}
	return true;
}

/* A partitioned join feeds the same partial aggregate as a single table */
static bool
gp_slice_join_partial(Query *original, PlannedStmt *stmt, Plan *plan,
					  Plan *parent, bool sort_above, bool motion)
{
	GpPartialState state;
	GpPartialState finalstate;
	ListCell   *lc;
	Agg		   *agg;
	Query	   *remote;

	if (!IsA(plan, Agg) || gp_slice_join_derived(original))
		return false;
	agg = (Agg *) plan;
	if (agg->numCols != list_length(original->groupClause))
		return false;

	state.aggs = NIL;
	state.avgs = NIL;
	state.keys = NIL;
	state.allow_avg = false;
	state.npos = 0;
	state.nextaggno = 0;
	state.below = NULL;
	state.ok = true;
	foreach(lc, original->groupClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sc, original->targetList);
		GpPartialKey *key = palloc0(sizeof(GpPartialKey));

		key->expr = tle->expr;
		if (IsA(key->expr, Var))
		{
			Var		   *var = (Var *) key->expr;
			RangeTblEntry *group = rt_fetch(var->varno, original->rtable);

			if (group->rtekind == RTE_GROUP && var->varattno > 0)
				key->expr = list_nth(group->groupexprs, var->varattno - 1);
		}
		state.npos++;
		key->pos = state.npos;
		key->ressortgroupref = tle->ressortgroupref;
		state.keys = lappend(state.keys, key);
	}
	gp_partial_walker((Node *) original->targetList, &state);
	if (state.ok)
		gp_partial_walker((Node *) original->havingQual, &state);
	if (!state.ok || (state.aggs == NIL && state.avgs == NIL))
		return false;

	remote = copyObject(original);
	remote->targetList = NIL;
	foreach(lc, state.keys)
	{
		GpPartialKey *key = lfirst(lc);
		TargetEntry *tle = makeTargetEntry(copyObject(key->expr),
									   key->pos, NULL, false);

		tle->ressortgroupref = key->ressortgroupref;
		remote->targetList = lappend(remote->targetList, tle);
	}
	foreach(lc, state.aggs)
	{
		GpPartialAgg *partial = lfirst(lc);
		TargetEntry *tle = makeTargetEntry((Expr *) copyObject(partial->aggref),
									   partial->pos, NULL, false);

		remote->targetList = lappend(remote->targetList, tle);
	}
	remote->havingQual = NULL;
	remote->sortClause = NIL;
	remote->limitCount = NULL;
	remote->limitOffset = NULL;

	/* Remote SQL needs original Vars; the final Agg needs planned Aggrefs */
	finalstate = state;
	finalstate.aggs = NIL;
	state.avgs = NIL;
	finalstate.npos = list_length(state.keys);
	finalstate.nextaggno = 0;
	finalstate.ok = true;
	gp_partial_walker((Node *) agg->plan.targetlist, &finalstate);
	if (finalstate.ok)
		gp_partial_walker((Node *) agg->plan.qual, &finalstate);
	if (!finalstate.ok || finalstate.aggs == NIL)
		return false;

	if (!gp_slice_partial_finish(agg, &finalstate, remote, motion,
								 agg->plan.targetlist, agg->plan.qual))
		return false;
	/* Partial groups from different segments are not sorted by group key */
	agg->aggstrategy = AGG_HASHED;
	if (original->sortClause != NIL && !sort_above)
	{
		Sort	   *sort = make_sort_from_sortclauses(original->sortClause,
												  &agg->plan);

		gp_slice_sort_target(original, sort);

		if (parent == NULL)
			stmt->planTree = &sort->plan;
		else
			parent->lefttree = &sort->plan;
	}
	return true;
}

/* Resolve a grouping key through the parser's GROUP and join RTEs */
static Node *
gp_slice_join_agg_key(Query *original, SortGroupClause *sc)
{
	TargetEntry *tle = get_sortgroupclause_tle(sc, original->targetList);
	Node	   *expr = (Node *) tle->expr;

	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;
		RangeTblEntry *rte = rt_fetch(var->varno, original->rtable);

		if (rte->rtekind == RTE_GROUP && var->varattno > 0)
			expr = list_nth(rte->groupexprs, var->varattno - 1);
	}
	return flatten_join_alias_for_parser(original, expr, 0);
}

static bool
gp_slice_join_agg_equal(Node *a, Node *b)
{
	return equal(a, b) ||
		(IsA(a, Var) && IsA(b, Var) &&
		 ((Var *) a)->varno == ((Var *) b)->varno &&
		 ((Var *) a)->varattno == ((Var *) b)->varattno);
}

/* Mapping of Vars of the original join to columns of its serialized answer */
typedef struct GpJoinAggMap
{
	Query	   *original;
	Query	   *inner;
	bool		ok;
} GpJoinAggMap;

static Node *
gp_slice_join_agg_var(Node *node, GpJoinAggMap *map)
{
	Node	   *expr = node;
	Var		   *var;
	RangeTblEntry *rte;
	ListCell   *lc;

	if (node == NULL)
		return NULL;
	if (!IsA(node, Var))
		return expression_tree_mutator(node, gp_slice_join_agg_var, map);
	var = (Var *) node;
	if (var->varlevelsup != 0 || var->varattno <= 0)
	{
		map->ok = false;
		return node;
	}
	rte = rt_fetch(var->varno, map->original->rtable);
	if (rte->rtekind == RTE_GROUP)
		expr = list_nth(rte->groupexprs, var->varattno - 1);
	else if (rte->rtekind == RTE_JOIN)
		expr = flatten_join_alias_for_parser(map->original, expr, 0);

	foreach(lc, map->inner->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (gp_slice_join_agg_equal((Node *) tle->expr, expr))
			return (Node *) makeVar(1, tle->resno, exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr), 0);
	}
	map->ok = false;
	return node;
}

/* A function RTE that reads a serialized join with all of its output columns */
static RangeTblEntry *
gp_slice_join_agg_rte(Query *inner, const char *serialized)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RangeTblFunction *rtf = makeNode(RangeTblFunction);
	FuncExpr   *func = makeNode(FuncExpr);
	Const	   *arg;
	List	   *types = NIL;
	List	   *typmods = NIL;
	List	   *names = NIL;
	List	   *collations = NIL;
	ListCell   *lc;
	Oid			peer_query_oid = DatumGetObjectId(DirectFunctionCall1(
		regprocedurein, CStringGetDatum("gp.peer_query(text)")));

	foreach(lc, inner->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		types = lappend_oid(types, exprType((Node *) tle->expr));
		typmods = lappend_int(typmods, exprTypmod((Node *) tle->expr));
		names = lappend(names, makeString(pstrdup(tle->resname)));
		collations = lappend_oid(collations, exprCollation((Node *) tle->expr));
	}
	arg = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID, -1,
					PointerGetDatum(cstring_to_text(serialized)), false, false);
	func->funcid = peer_query_oid;
	func->funcresulttype = RECORDOID;
	func->funcretset = true;
	func->funcformat = COERCE_EXPLICIT_CALL;
	func->inputcollid = DEFAULT_COLLATION_OID;
	func->args = list_make1(arg);
	rtf->funccolcount = list_length(types);
	rtf->funccoltypes = types;
	rtf->funccoltypmods = typmods;
	rtf->funccolnames = names;
	rtf->funccolcollations = collations;
	rtf->funcexpr = (Node *) func;

	rte->rtekind = RTE_FUNCTION;
	rte->functions = list_make1(rtf);
	rte->eref = makeAlias("moved", names);
	rte->inFromCl = true;
	return rte;
}

/*
 * Compute a full aggregate after redistributing the join itself by all of its
 * grouping columns.  This is the general fallback when a partial aggregate is
 * not available: every group lands on one segment, whatever its aggregate does.
 */
static bool
gp_slice_join_redistributed_agg(Query *original, PlannedStmt *stmt, Plan *plan,
								Plan *parent, bool sort_above, List *tables)
{
	Agg		   *agg;
	Query	   *inner;
	Query	   *remote;
	GpJoinAggMap map;
	GpJoinTable *maptable = NULL;
	StringInfoData sql;
	AttrNumber *keypos;
	ListCell   *lc;
	CustomScan *cscan;
	RangeTblEntry *rte;
	FromExpr   *from;
	int			pos = 0;
	int			keyidx = 0;
	int			nkeys = list_length(original->groupClause);
	char	   *serialized;

	if (nkeys == 0 || !IsA(plan, Agg))
		return false;
	agg = (Agg *) plan;
	if (agg->numCols != nkeys)
		return false;

	/* The inner query computes the join, before any aggregation */
	inner = copyObject(original);
	inner->targetList = NIL;
	inner->hasAggs = false;
	inner->hasGroupRTE = false;
	inner->groupClause = NIL;
	inner->groupingSets = NIL;
	inner->havingQual = NULL;
	inner->distinctClause = NIL;
	inner->sortClause = NIL;
	inner->limitCount = NULL;
	inner->limitOffset = NULL;
	foreach(lc, original->rtable)
	{
		RangeTblEntry *input = lfirst_node(RangeTblEntry, lc);
		Relation	rel;
		TupleDesc	tupdesc;
		int			rti = foreach_current_index(lc) + 1;

		if (input->rtekind == RTE_SUBQUERY)
		{
			if (input->subquery == NULL)
				return false;
			foreach_node(TargetEntry, subtle, input->subquery->targetList)
			{
				Var		   *var;
				TargetEntry *tle;

				if (subtle->resjunk)
					continue;
				var = makeVar(rti, subtle->resno, exprType((Node *) subtle->expr),
							  exprTypmod((Node *) subtle->expr),
							  exprCollation((Node *) subtle->expr), 0);
				pos++;
				tle = makeTargetEntry((Expr *) var, pos, psprintf("c%d", pos), false);
				inner->targetList = lappend(inner->targetList, tle);
			}
			continue;
		}
		if (input->rtekind != RTE_RELATION &&
			(input->rtekind != RTE_FUNCTION || !OidIsValid(input->relid)))
			continue;
		rel = table_open(input->relid, NoLock);
		tupdesc = RelationGetDescr(rel);
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			Var		   *var;
			TargetEntry *tle;

			if (att->attisdropped)
				continue;
			var = makeVar(rti, att->attnum, att->atttypid,
						  att->atttypmod, att->attcollation, 0);
			pos++;
			tle = makeTargetEntry((Expr *) var, pos, psprintf("c%d", pos), false);
			inner->targetList = lappend(inner->targetList, tle);
		}
		table_close(rel, NoLock);
	}
	if (inner->targetList == NIL)
		return false;

	/* Grouping expressions can be computed columns of the join answer */
	keypos = palloc_array(AttrNumber, nkeys);
	foreach(lc, original->groupClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, lc);
		Node	   *expr = gp_slice_join_agg_key(original, sc);
		TargetEntry *found = NULL;
		ListCell   *it;

		if (!gp_slice_hashable(exprType(expr)))
			return false;
		foreach(it, inner->targetList)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, it);

			if (gp_slice_join_agg_equal((Node *) tle->expr, expr))
			{
				found = tle;
				break;
			}
		}
		if (found == NULL)
		{
			pos++;
			found = makeTargetEntry((Expr *) copyObject(expr), pos,
									psprintf("c%d", pos), false);
			inner->targetList = lappend(inner->targetList, found);
		}
		keypos[keyidx++] = found->resno;
	}

	foreach(lc, tables)
	{
		GpJoinTable *table = lfirst(lc);

		if (!table->policy.replicated)
		{
			maptable = table;
			break;
		}
	}
	if (maptable == NULL)
		return false;

	/*
	 * A derived outer join cannot leave its Motion to the hooks of the
	 * fragment planning: the inner query is serialized as it is, and each
	 * segment would join its local rows only.  When the join equates a
	 * column of one table with a column of the other, redistribute the
	 * second table by that column under the map of the first: every join
	 * pair then meets on one segment, so the derived answer is complete
	 * there.  Tables of one colocation group need no Motion at all.
	 */
	if (gp_slice_join_derived(original) &&
		!gp_slice_join_all_inner_levels(tables))
	{
		GpJoinTable *stay = NULL;
		GpJoinTable *move = NULL;
		AttrNumber	move_attno = 0;
		AttrNumber	keyatts[1];
		List	   *inner_tables = NIL;
		List	   *inner_conds = NIL;
		ListCell   *lc2;
		bool		found = false;

		if (list_length(tables) != 2)
			return false;
		inner_tables = NIL;
		inner_conds = NIL;
		if (!gp_slice_join_from(inner, &inner_tables, &inner_conds) ||
			!gp_slice_join_load_policies(inner, inner_tables))
			return false;
		stay = linitial(inner_tables);
		move = lsecond(inner_tables);
		if (stay->policy.replicated || stay->policy.colocation_id == 0 ||
			move->policy.replicated || move->policy.colocation_id == 0)
			return false;
		if (stay->policy.colocation_id != move->policy.colocation_id)
		{
			foreach(lc2, inner_conds)
			{
				GpJoinCond *cond = lfirst(lc2);
				AttrNumber	attno = 0;

				if (gp_slice_join_move_key(cond->qual, cond->query, stay,
										   move, &attno))
				{
					move_attno = attno;
					found = true;
					break;
				}
			}
			if (!found)
				return false;
			keyatts[0] = move_attno;
			if (!gp_slice_redistribute_rte_keys(inner, stay, move, false, 1,
												keyatts, NULL, false))
				return false;
		}
	}

	/* Keep on the asking segment only rows whose grouping key belongs to it */
	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT * FROM (%s) AS joined WHERE (ARRAY[",
					 pg_get_querydef(inner, false));
	for (int i = 0; i < maptable->policy.nbuckets; i++)
		appendStringInfo(&sql, "%s%d", i > 0 ? "," : "",
						 maptable->policy.map[i]);
	appendStringInfo(&sql, "])[gp.bucket(%d", maptable->policy.nbuckets);
	for (int i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = list_nth(inner->targetList, keypos[i] - 1);

		appendStringInfo(&sql, ", %s%s", quote_identifier(tle->resname),
						 exprCollation((Node *) tle->expr) != InvalidOid ?
						 " COLLATE pg_catalog.default" : "");
	}
	appendStringInfoString(&sql,
		") + 1] OPERATOR(pg_catalog.=) current_setting('gp.requester_segment')::int4");
	serialized = gp_interconnect_plan(sql.data);

	/* The original aggregate now reads the moved columns from one function */
	remote = copyObject(original);
	rte = gp_slice_join_agg_rte(inner, serialized);
	remote->rtable = list_make1(rte);
	remote->rteperminfos = NIL;
	remote->hasGroupRTE = false;
	from = makeNode(FromExpr);
	{
		RangeTblRef *ref = makeNode(RangeTblRef);

		ref->rtindex = 1;
		from->fromlist = list_make1(ref);
	}
	remote->jointree = from;
	map.original = original;
	map.inner = inner;
	map.ok = true;
	remote = query_tree_mutator(remote, gp_slice_join_agg_var, &map, 0);
	if (!map.ok)
		return false;

	cscan = gp_slice_make(plan, remote, false, true, original->sortClause);
	/* All Vars above the replacement refer to the function's output */
	stmt->rtable = copyObject(remote->rtable);
	stmt->permInfos = NIL;
	stmt->unprunableRelids = NULL;
	if (parent == NULL)
		stmt->planTree = &cscan->scan.plan;
	else
		parent->lefttree = &cscan->scan.plan;
	if (original->sortClause != NIL && !sort_above)
	{
		Sort	   *sort = make_sort_from_sortclauses(original->sortClause,
												  &cscan->scan.plan);

		gp_slice_sort_target(original, sort);

		if (parent == NULL)
			stmt->planTree = &sort->plan;
		else
			parent->lefttree = &sort->plan;
	}
	return true;
}

/*
 * Install the node on the coordinator; called from _PG_init()
 */
void
gp_slice_init(void)
{
	RegisterCustomScanMethods(&gp_slice_plan_methods);
}
