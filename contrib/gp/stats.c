/* -------------------------------------------------------------------------
 *
 * stats.c
 *		ANALYZE on the segments and cluster statistics on the coordinator
 *
 * A distributed table is empty on the coordinator.  ANALYZE therefore runs
 * on every segment first.  Relation sizes are summed over the segments,
 * while attribute statistics come from the first segment, as an initial
 * sample of the distribution of the whole table.  A replicated table is
 * counted once.  The ordinary restore functions put the result into the
 * catalogs of the coordinator, whose planner then sees the cluster statistics.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/stats.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "postmaster/autovacuum.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp.h"

/* A change count of one table, summed over the segments */
typedef struct GpModCount
{
	Oid			relid;
	uint64		mods;
} GpModCount;

static const char *const gp_stat_names[] = {
	"null_frac",
	"avg_width",
	"n_distinct",
	"most_common_vals",
	"most_common_freqs",
	"histogram_bounds",
	"correlation",
	"most_common_elems",
	"most_common_elem_freqs",
	"elem_count_histogram",
	"range_length_histogram",
	"range_empty_frac",
	"range_bounds_histogram",
};

static const char *const gp_stat_types[] = {
	"real",
	"integer",
	"real",
	"text",
	"real[]",
	"text",
	"real",
	"text",
	"real[]",
	"real[]",
	"text",
	"real",
	"text",
};

StaticAssertDecl(lengthof(gp_stat_names) == lengthof(gp_stat_types),
				 "statistic names and types must match");

/* Keep the ANALYZE options when the statement is rebuilt for one relation */
static void
gp_analyze_options(StringInfo sql, List *options)
{
	ListCell   *lc;

	if (options == NIL)
		return;
	appendStringInfoChar(sql, '(');
	foreach(lc, options)
	{
		DefElem    *opt = lfirst_node(DefElem, lc);

		appendStringInfo(sql, "%s%s", lc == list_head(options) ? "" : ", ",
						 quote_identifier(opt->defname));
		if (opt->arg == NULL)
			continue;
		switch (nodeTag(opt->arg))
		{
			case T_Boolean:
				appendStringInfo(sql, " %s", boolVal(opt->arg) ? "true" : "false");
				break;
			case T_Integer:
				appendStringInfo(sql, " %d", intVal(opt->arg));
				break;
			case T_Float:
			case T_String:
				appendStringInfo(sql, " %s",
								 quote_literal_cstr(strVal(opt->arg)));
				break;
			default:
				elog(ERROR, "unexpected ANALYZE option value");
		}
	}
	appendStringInfoString(sql, ") ");
}

/* Append one relation, rather than a local relation of a mixed statement */
static void
gp_analyze_relation(StringInfo sql, List *options, Oid relid, bool inherit,
					List *columns)
{
	const char *schema = get_namespace_name(get_rel_namespace(relid));
	const char *relname = get_rel_name(relid);
	ListCell   *lc;

	if (schema == NULL || relname == NULL)
		return;
	if (sql->len > 0)
		appendStringInfoString(sql, "; ");
	appendStringInfoString(sql, "ANALYZE ");
	gp_analyze_options(sql, options);
	if (!inherit)
		appendStringInfoString(sql, "ONLY ");
	appendStringInfoString(sql, quote_qualified_identifier(schema, relname));
	if (columns == NIL)
		return;
	appendStringInfoChar(sql, '(');
	foreach(lc, columns)
	{
		appendStringInfo(sql, "%s%s", lc == list_head(columns) ? "" : ", ",
						 quote_identifier(strVal(lfirst(lc))));
	}
	appendStringInfoChar(sql, ')');
}

/* The distributed relations of an explicit ANALYZE, or all of them */
static List *
gp_analyze_targets(VacuumStmt *stmt, StringInfo sql)
{
	List	   *relids = NIL;
	MemoryContext oldcontext = CurrentMemoryContext;

	if (stmt->rels != NIL)
	{
		ListCell   *lc;

		foreach(lc, stmt->rels)
		{
			VacuumRelation *vrel = lfirst_node(VacuumRelation, lc);
			GpPolicy	policy;
			Oid			relid = RangeVarGetRelid(vrel->relation, NoLock, true);

			if (!OidIsValid(relid) || !gp_policy_get(relid, &policy))
				continue;
			relids = lappend_oid(relids, relid);
			gp_analyze_relation(sql, stmt->options, relid,
								vrel->relation->inh, vrel->va_cols);
		}
		return relids;
	}

	SPI_connect();
	if (SPI_execute("SELECT relid FROM gp.distribution_policy ORDER BY relid",
					true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not list distributed relations for ANALYZE");
	MemoryContextSwitchTo(oldcontext);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;
		Oid			relid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i],
												 SPI_tuptable->tupdesc, 1, &isnull));

		if (isnull)
			continue;
		relids = lappend_oid(relids, relid);
		gp_analyze_relation(sql, stmt->options, relid, true, NIL);
	}
	SPI_finish();
	return relids;
}

/* A restore function returns false for invalid arguments, without an ERROR */
static void
gp_stats_restore(const char *sql)
{
	bool		isnull;
	Datum		result;

	if (SPI_execute(sql, false, 1) != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "could not restore cluster statistics");
	result = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
						   1, &isnull);
	if (isnull || !DatumGetBool(result))
		elog(ERROR, "could not restore cluster statistics");
}

static void gp_stats_import(Oid relid);

/* Sum relation sizes, except that a replicated table is already whole */
static void
gp_stats_relation(Oid relid, bool replicated)
{
	GpQuery    *query;
	PGresult   *res;
	StringInfoData sql;
	uint64		pages = 0;
	uint64		allvisible = 0;
	uint64		allfrozen = 0;
	double		tuples = 0;
	int			rows = 0;
	GpSegment  *segments;
	int			nsegments;
	const char *schema = get_namespace_name(get_rel_namespace(relid));
	const char *relname = get_rel_name(relid);

	if (schema == NULL || relname == NULL)
		return;
	query = gp_query_start(psprintf(
		"SELECT relpages::text, reltuples::text, "
		"relallvisible::text, relallfrozen::text "
		"FROM pg_catalog.pg_class WHERE oid = %u", relid), 0, NULL);
	while ((res = gp_query_next(query)) != NULL)
	{
		if (PQnfields(res) != 4 || PQntuples(res) != 1)
			elog(ERROR, "unexpected relation statistics from a segment");
		if (!replicated || rows == 0)
		{
			pages += strtoull(PQgetvalue(res, 0, 0), NULL, 10);
			tuples += strtod(PQgetvalue(res, 0, 1), NULL);
			allvisible += strtoull(PQgetvalue(res, 0, 2), NULL, 10);
			allfrozen += strtoull(PQgetvalue(res, 0, 3), NULL, 10);
		}
		rows++;
		PQclear(res);
	}
	nsegments = gp_segments_get(&segments);
	if (rows != nsegments)
		elog(ERROR, "relation statistics are missing on a segment");

	/* A share of the coordinator adds its own copy to the sums */
	{
		GpPolicy	policy;

		if (gp_policy_get(relid, &policy) && gp_policy_has_coordinator(&policy))
		{
			Relation	rel = table_open(relid, NoLock);

			pages += rel->rd_rel->relpages;
			tuples += rel->rd_rel->reltuples;
			allvisible += rel->rd_rel->relallvisible;
			allfrozen += rel->rd_rel->relallfrozen;
			table_close(rel, NoLock);
		}
	}

	initStringInfo(&sql);
	appendStringInfo(&sql,
		"SELECT pg_catalog.pg_restore_relation_stats("
		"'version', '%d'::integer, 'schemaname', %s::text, "
		"'relname', %s::text, 'relpages', '%llu'::integer, "
		"'reltuples', '%.17g'::real, 'relallvisible', '%llu'::integer, "
		"'relallfrozen', '%llu'::integer)",
		PG_VERSION_NUM, quote_literal_cstr(schema), quote_literal_cstr(relname),
		(unsigned long long) pages, tuples,
		(unsigned long long) allvisible, (unsigned long long) allfrozen);
	gp_stats_restore(sql.data);
}

/* Copy the sample of the first segment, using attnum instead of attname */
static void
gp_stats_attributes(Oid relid, int only)
{
	GpQuery    *query;
	PGresult   *res;
	const char *schema = get_namespace_name(get_rel_namespace(relid));
	const char *relname = get_rel_name(relid);

	if (schema == NULL || relname == NULL)
		return;
	query = gp_query_start(psprintf(
		"SELECT a.attnum::text, s.inherited::text, "
		"s.null_frac::text, s.avg_width::text, s.n_distinct::text, "
		"s.most_common_vals::text, s.most_common_freqs::text, "
		"s.histogram_bounds::text, s.correlation::text, "
		"s.most_common_elems::text, s.most_common_elem_freqs::text, "
		"s.elem_count_histogram::text, s.range_length_histogram::text, "
		"s.range_empty_frac::text, s.range_bounds_histogram::text "
		"FROM pg_catalog.pg_stats s "
		"JOIN pg_catalog.pg_attribute a "
		"ON a.attrelid = s.tableid AND a.attname = s.attname "
		"WHERE s.tableid = %u ORDER BY a.attnum, s.inherited", relid), only, NULL);
	while ((res = gp_query_next(query)) != NULL)
	{
		StringInfoData sql;

		if (PQnfields(res) != lengthof(gp_stat_names) + 2 ||
			PQntuples(res) != 1)
			elog(ERROR, "unexpected attribute statistics from a segment");
		initStringInfo(&sql);
		appendStringInfo(&sql,
			"SELECT pg_catalog.pg_restore_attribute_stats("
			"'version', '%d'::integer, 'schemaname', %s::text, "
			"'relname', %s::text, 'attnum', '%s'::smallint, "
			"'inherited', '%s'::boolean",
			PG_VERSION_NUM, quote_literal_cstr(schema),
			quote_literal_cstr(relname), PQgetvalue(res, 0, 0),
			PQgetvalue(res, 0, 1));
		for (int i = 0; i < lengthof(gp_stat_names); i++)
		{
			if (PQgetisnull(res, 0, i + 2))
				continue;
			appendStringInfo(&sql, ", '%s', %s::%s", gp_stat_names[i],
							 quote_literal_cstr(PQgetvalue(res, 0, i + 2)),
							 gp_stat_types[i]);
		}
		appendStringInfoChar(&sql, ')');
		gp_stats_restore(sql.data);
		PQclear(res);
	}
}

/* Run ANALYZE on the segments and put cluster statistics on the coordinator */
void
gp_analyze(VacuumStmt *stmt)
{
	StringInfoData sql;
	List	   *relids;
	ListCell   *lc;
	GpSegment  *segments;
	int			nsegments;

	initStringInfo(&sql);
	relids = gp_analyze_targets(stmt, &sql);
	if (relids == NIL)
		return;

	gp_dispatch_xact(sql.data);
	nsegments = gp_segments_get(&segments);
	if (nsegments == 0)
		return;

	SPI_connect();
	foreach(lc, relids)
		gp_stats_import(lfirst_oid(lc));
	SPI_finish();
	CommandCounterIncrement();
}

/* Bring the cluster statistics of one table to the coordinator */
static void
gp_stats_import(Oid relid)
{
	GpPolicy	policy;
	GpSegment  *segments;

	if (!gp_policy_get(relid, &policy))
		return;
	if (gp_segments_get(&segments) == 0)
		return;
	gp_stats_relation(relid, policy.replicated);
	gp_stats_attributes(relid, segments[0].segment_id);
}

/*
 * Analyze the distributed tables the cluster has changed more than the
 * threshold autovacuum names: the sum of the changes of the segments
 * answers the changes of the cluster, and the ANALYZE of the segments
 * resets their counters.  The check runs at the start of a statement of
 * the coordinator, while gp.autoanalyze is on; a failure of the check
 * warns and leaves the statement alone
 */
void
gp_autoanalyze_check(void)
{
	GpQuery    *query;
	PGresult   *res;
	ListCell   *lc;
	List	   *relids = NIL;
	MemoryContext ctx = CurrentMemoryContext;
	double		threshold;
	double		scale;

	if (!gp_autoanalyze || !IsUnderPostmaster ||
		gp_role != GP_ROLE_COORDINATOR)
		return;

	threshold = strtod(GetConfigOption("autovacuum_analyze_threshold",
									   false, false), NULL);
	scale = strtod(GetConfigOption("autovacuum_analyze_scale_factor",
								   false, false), NULL);

	PG_TRY();
	{
		ListCell   *lc2;
		List	   *counts = NIL;

		query = gp_query_start("SELECT relid::text, "
			"n_mod_since_analyze::text "
			"FROM pg_catalog.pg_stat_user_tables "
			"WHERE n_mod_since_analyze > 0", 0, NULL);
		while ((res = gp_query_next(query)) != NULL)
		{
			Oid			relid;
			uint64		mods;
			ListCell   *lc3;
			GpModCount *m;
			GpModCount *found = NULL;

			relid = (Oid) strtoul(PQgetvalue(res, 0, 0), NULL, 10);
			mods = strtoull(PQgetvalue(res, 0, 1), NULL, 10);
			foreach(lc3, counts)
			{
				GpModCount *c = lfirst(lc3);

				if (c->relid == relid)
				{
					c->mods += mods;
					found = c;
					break;
				}
			}
			if (found == NULL)
			{
				m = palloc_object(GpModCount);
				m->relid = relid;
				m->mods = mods;
				counts = lappend(counts, m);
			}
			PQclear(res);
		}
		gp_query_end(query, false);

		foreach(lc2, counts)
		{
			GpModCount *m = lfirst(lc2);
			GpPolicy	policy;
			const char *schema;
			HeapTuple	tuple;

			schema = get_namespace_name(get_rel_namespace(m->relid));
			if (schema == NULL ||
				strcmp(schema, "pg_catalog") == 0 ||
				strcmp(schema, "information_schema") == 0)
				continue;
			if (!gp_policy_get(m->relid, &policy))
				continue;
			tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(m->relid));
			if (HeapTupleIsValid(tuple))
			{
				double		reltuples;

				reltuples = ((Form_pg_class) GETSTRUCT(tuple))->reltuples;
				ReleaseSysCache(tuple);
				if (m->mods > threshold + scale * reltuples)
					relids = lappend_oid(relids, m->relid);
			}
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(ctx);
		FlushErrorState();
		ereport(WARNING,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("the check of the statistics of the cluster failed"),
				errdetail("The statements go on; turn gp.autoanalyze off "
						  "if the check is not wanted."));
		return;
	}
	PG_END_TRY();

	if (relids == NIL)
		return;

	PG_TRY();
	{
		SPI_connect();
		foreach(lc, relids)
		{
			Oid			relid = lfirst_oid(lc);
			const char *schema = get_namespace_name(get_rel_namespace(relid));
			const char *relname = get_rel_name(relid);

			gp_dispatch_xact(psprintf("ANALYZE %s",
				quote_qualified_identifier(schema, relname)));
			gp_stats_import(relid);
		}
		SPI_finish();
		CommandCounterIncrement();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(ctx);
		FlushErrorState();
		ereport(WARNING,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("the automatic analyze of the cluster failed"),
				errdetail("The statistics stay as they were; the next "
						  "statement checks again."));
	}
	PG_END_TRY();
}
