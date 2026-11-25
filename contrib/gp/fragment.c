/* -------------------------------------------------------------------------
 *
 * fragment.c
 *		The serialized fragment of a query and the statement snapshots
 *
 * The pull of a motion carries the fragment of the peer as the serialized
 * plan of the marker query, and gp.fragment_exec executes it in an ordinary
 * session of the peer, where gp.attach_statement_snapshots brings the
 * snapshots of the statement in.  The map of the snapshots lives as long as
 * the transaction of the backend that attached it.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/fragment.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include <limits.h>

#include "miscadmin.h"
#include "nodes/readfuncs.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "gp.h"

/* The statement snapshots of the last attached map of this backend */
static GpSnapshots *gp_fragment_current_snapshots = NULL;

#define GP_FRAGMENT_MAX_SNAPSHOTS 1024

GpSnapshots *
gp_fragment_snapshots(void)
{
	return gp_fragment_current_snapshots;
}

/*
 * The statement snapshots live as long as the transaction of the backend
 * that attached them: the exported tokens die with the holders of the
 * statement, and a later transaction must not read a stale map
 */
static bool gp_fragment_snapshots_callback_registered = false;

static void
gp_fragment_snapshots_xact_cb(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
	{
		if (gp_fragment_current_snapshots != NULL)
		{
			gp_snapshots_free(gp_fragment_current_snapshots);
			gp_fragment_current_snapshots = NULL;
		}
	}
}

/*
 * Parse the "segment:name" pairs of gp_snapshots_serialize() into a map.
 * The names hold hexadecimal characters and hyphens only, and the segments
 * are under 10^10, so no quoting is possible inside the text.
 */
static GpSnapshots *
gp_fragment_snapshots_parse(const char *map)
{
	GpSnapshots *snapshots;
	const char *pos = map;
	int			count = 1;
	MemoryContext oldcontext;

	while (*pos != '\0')
	{
		if (*pos == ';')
			count++;
		pos++;
	}
	if (count > GP_FRAGMENT_MAX_SNAPSHOTS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("too many statement snapshots")));

	/* The map outlives the command that attached it: backend lifetime */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	snapshots = palloc0(sizeof(*snapshots));
	snapshots->count = count;
	snapshots->entries = palloc0_array(GpSnapshotEntry, count);
	pos = map;
	for (int i = 0; i < count; i++)
	{
		const char *end;
		long		segment_id;

		end = pos + strspn(pos, "0123456789");
		if (end == pos || *end != ':')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid statement snapshots map")));
		segment_id = strtol(pos, NULL, 10);
		if (segment_id > INT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid segment number in the statement snapshots map")));
		snapshots->entries[i].segment_id = (int) segment_id;
		pos = end + 1;
		end = pos + strspn(pos, "0123456789abcdefABCDEF-");
		if (end == pos || (*end != ';' && *end != '\0'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid statement snapshots map")));
		snapshots->entries[i].name = pnstrdup(pos, end - pos);
		if (*end == '\0')
		{
			if (i != count - 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid statement snapshots map")));
			break;
		}
		pos = end + 1;
	}
	MemoryContextSwitchTo(oldcontext);
	return snapshots;
}

/*
 * Attach the statement snapshots to this backend, so a fragment dispatched
 * over libpq reads the same distributed state as its nested pulls: while the
 * transaction has not read anything, it also imports the export of its own
 * segment; under a transaction snapshot the earlier statements already fixed
 * the state, so only the pulls need the map.
 */
PG_FUNCTION_INFO_V1(gp_attach_statement_snapshots);

Datum
gp_attach_statement_snapshots(PG_FUNCTION_ARGS)
{
	char	   *map = text_to_cstring(PG_GETARG_TEXT_PP(0));
	GpSnapshots *snapshots;

	if (gp_role != GP_ROLE_SEGMENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gp.attach_statement_snapshots runs on a segment only")));

	snapshots = gp_fragment_snapshots_parse(map);

	if (!FirstSnapshotSet)
	{
		const char *name = NULL;

		for (int i = 0; i < snapshots->count; i++)
			if (snapshots->entries[i].segment_id == gp_segment_id)
				name = snapshots->entries[i].name;
		if (name == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("no snapshot for segment %d in the statement snapshots",
							gp_segment_id)));
		XactIsoLevel = XACT_REPEATABLE_READ;
		ImportSnapshot(name);
	}

	if (gp_fragment_current_snapshots != NULL)
		gp_snapshots_free(gp_fragment_current_snapshots);
	gp_fragment_current_snapshots = snapshots;

	if (!gp_fragment_snapshots_callback_registered)
	{
		RegisterXactCallback(gp_fragment_snapshots_xact_cb, NULL);
		gp_fragment_snapshots_callback_registered = true;
	}
	PG_RETURN_VOID();
}

/*
 * The plan of the marker query, serialized: the fragments travel as plans,
 * never as SQL text, so a peer executes what the planner of the asking side
 * chose, not what its own planner would choose now
 */
char *
gp_fragment_plan(const char *sql)
{
	PlannedStmt *stmt;
	RawStmt    *raw = linitial(pg_parse_query(sql));
	Query	   *query = parse_analyze_fixedparams(raw, sql, NULL, 0, NULL);
	bool		old = gp_gather_fragment(true);

	PG_TRY();
	{
		stmt = standard_planner(query, sql, 0, NULL, NULL);
	}
	PG_FINALLY();
	{
		gp_gather_fragment(old);
	}
	PG_END_TRY();
	return nodeToString(stmt);
}
