/* -------------------------------------------------------------------------
 *
 * move.c
 *		Moving a bucket of a colocation group to another segment
 *
 * A bucket moves in three steps: the rows of the bucket copy from the old
 * segment of the bucket to the new one, the bucket's row of the map
 * switches in one transaction, and the rows go away from the old segment.
 * The move gates the writes by the advisory locks of the group: a write
 * of the moving bucket waits, the writes of the other buckets answer,
 * and the readers answer too — the map commits in one transaction, so a
 * read keeps to the side of the switch its snapshot took and never
 * doubles a row.  The tables take a share-update-exclusive lock for the
 * move, which holds the DDL and the vacuum off the rows while they are
 * in two places.  The sessions drop their cached policies after.
 * The number of the segment that owns a bucket is a name, not a position:
 * a move never renumbers anything else.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/move.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp.h"

PG_FUNCTION_INFO_V1(gp_move_bucket);

/* The columns of a table that move, with the text output of every one */
typedef struct GpMoveTable
{
	Oid			relid;
	char		relqual[2 * NAMEDATALEN + 8];	/* schema.name, quoted */
	int			nkeys;
	StringInfoData keys;		/* the names of the key columns */
	int			ncols;
	StringInfoData cols;		/* the names of all the columns */
	FmgrInfo   *outfn;			/* the text output of every column */
} GpMoveTable;

/* The literal of a value, by the output function of its column */
static char *
gp_move_literal(FmgrInfo *outfn, const char *text, bool isnull)
{
	if (isnull)
		return pstrdup("NULL");
	return quote_literal_cstr(text);
}

/* One statement of a connection, tolerantly */
static bool
gp_move_exec(PGconn *conn, const char *sql)
{
	PGresult   *res = libpqsrv_exec(conn, sql, PG_WAIT_EXTENSION);
	bool		ok = res != NULL &&
		(PQresultStatus(res) == PGRES_COMMAND_OK ||
		 PQresultStatus(res) == PGRES_TUPLES_OK);

	if (res != NULL)
		PQclear(res);
	return ok;
}

/* The columns of a moving table, read and remembered */
static void
gp_move_table(Oid relid, GpPolicy *policy, GpMoveTable *table)
{
	Relation	rel = table_open(relid, AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);

	table->relid = relid;
	snprintf(table->relqual, sizeof(table->relqual), "%s.%s",
			 quote_identifier(get_namespace_name(RelationGetNamespace(rel))),
			 quote_identifier(RelationGetRelationName(rel)));
	table->nkeys = policy->nkeys;
	initStringInfo(&table->keys);
	for (int j = 0; j < policy->nkeys; j++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, policy->keys[j] - 1);

		appendStringInfo(&table->keys, "%s%s", j > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
	}
	table->ncols = 0;
	initStringInfo(&table->cols);
	table->outfn = palloc_array(FmgrInfo, tupdesc->natts);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			outoid;
		bool		varlena;

		if (att->attisdropped || att->attgenerated)
			continue;
		appendStringInfo(&table->cols, "%s%s", table->ncols > 0 ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
		getTypeOutputInfo(att->atttypid, &outoid, &varlena);
		fmgr_info(outoid, &table->outfn[table->ncols]);
		table->ncols++;
	}
	table_close(rel, AccessShareLock);
}

/* A cursor over the rows of the bucket, on the old segment */
static bool
gp_move_open_cursor(PGconn *conn, GpMoveTable *table, int nbuckets,
					int bucket, const char *cursor)
{
	StringInfoData sql;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "DECLARE %s CURSOR FOR "
					 "SELECT %s FROM ONLY %s WHERE "
					 "gp.bucket(%d, %s) OPERATOR(pg_catalog.=) %d",
					 cursor, table->cols.data, table->relqual, nbuckets,
					 table->keys.data, bucket);
	return gp_move_exec(conn, sql.data);
}

/* The advisory tag of the locks of the gate, by the layout the cluster
 * keeps for the two-int4 advisory locks: the database, the two keys, and
 * the mark of their kind -- so the locks of the gate are the ordinary
 * advisory locks, taken and watched from SQL as well */
#define GP_GATE_TAG(tag, key1, key2) \
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, key1, key2, 2)

/*
 * The shared lock of a bucket: a write of the bucket takes it for its
 * transaction, and a move of the bucket waits for it.  The writers of
 * the other buckets take no part in it and answer
 */
void
gp_gate_bucket_shared(int colocation_id, int bucket)
{
	LOCKTAG		tag;

	GP_GATE_TAG(tag, colocation_id, bucket);
	(void) LockAcquire(&tag, ShareLock, false, false);
}

/*
 * The shared lock of the whole group: a write that may touch any bucket
 * of the group takes it, and a move of any bucket waits for it
 */
void
gp_gate_group_shared(int colocation_id)
{
	LOCKTAG		tag;

	GP_GATE_TAG(tag, colocation_id, -1);
	(void) LockAcquire(&tag, ShareLock, false, false);
}

/*
 * The exclusive locks of a move: the bucket first, the whole group
 * after.  Every taker of the locks of the gate takes the locks of its
 * buckets before the lock of the group, so the waits never close a
 * circle
 */
void
gp_gate_move(int colocation_id, int bucket)
{
	LOCKTAG		tag;

	GP_GATE_TAG(tag, colocation_id, bucket);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
	GP_GATE_TAG(tag, colocation_id, -1);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
}

Datum
gp_move_bucket(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int			bucket = PG_GETARG_INT32(1);
	int			to_segment = PG_GETARG_INT32(2);
	GpPolicy	policy;
	GpSegment  *segments;
	int			nsegments;
	int			from_segment;
	char		fromhost[GP_HOST_LEN] = "";
	int			fromport = 0;
	char		tohost[GP_HOST_LEN] = "";
	int			toport = 0;
	GpMoveTable *tables = NULL;
	int			ntables = 0;
	const char *keys[5] = {"host", "port", "dbname", "connect_timeout", NULL};
	const char *fromvals[5];
	const char *tovals[5];
	char		fromportstr[16];
	char		toportstr[16];
	PGconn	   *from = NULL;
	PGconn	   *to = NULL;
	MemoryContext oldcxt = CurrentMemoryContext;
	MemoryContext movecxt = AllocSetContextCreate(TopMemoryContext,
												  "gp move bucket",
												  ALLOCSET_DEFAULT_SIZES);
	char		cursor[64];
	int			rows = 0;

	/*
	 * Everything the move remembers lives in this context: the contexts
	 * of the SPI calls die with SPI_finish, and the move reads its tables
	 * after it
	 */
	MemoryContextSwitchTo(movecxt);

	if (gp_role != GP_ROLE_COORDINATOR || !IsUnderPostmaster)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("buckets move on the coordinator only"));
	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("buckets move for superusers only"));
	if (bucket < 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bucket must be non-negative"));

	if (!gp_policy_get(relid, &policy) || policy.replicated ||
		policy.nbuckets <= 0 || policy.colocation_id == 0)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("relation \"%s\" is not distributed by hash of a colocation group",
					   get_rel_name(relid)),
				errhint("Only the tables of a hash colocation group move by buckets."));
	if (bucket >= policy.nbuckets)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("bucket %d is out of the %d buckets of the group",
					   bucket, policy.nbuckets));
	from_segment = policy.map[bucket];
	if (to_segment == from_segment)
		PG_RETURN_INT32(0);
	if (to_segment == 0)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("a bucket does not move to the coordinator"),
				errhint("The share of the coordinator is set at the distribution of a table."));

	nsegments = gp_segments_get(&segments);
	if (nsegments == 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the cluster has no segments"));
	for (int i = 0; i < nsegments; i++)
	{
		if (segments[i].segment_id == from_segment)
		{
			strlcpy(fromhost, segments[i].host, sizeof(fromhost));
			fromport = segments[i].port;
		}
		if (segments[i].segment_id == to_segment)
		{
			strlcpy(tohost, segments[i].host, sizeof(tohost));
			toport = segments[i].port;
		}
	}
	pfree(segments);
	if (fromhost[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the segment %d of the bucket is not registered",
					   from_segment));
	if (tohost[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("segment %d is not registered", to_segment));

	SPI_connect();

	/*
	 * The tables of the group move together, for the joins by the key
	 * stay local: every hash table of the group moves its share of the
	 * bucket
	 */
	{
		Oid			atypes[1] = {INT4OID};
		Datum		avalues[1];

		avalues[0] = Int32GetDatum(policy.colocation_id);
		if (SPI_execute_with_args("SELECT p.relid FROM "
								  "gp.distribution_policy p "
								  "JOIN gp.colocation_groups c "
								  "USING (colocation_id) "
								  "WHERE p.colocation_id = $1 "
								  "AND p.kind = 'hash' ORDER BY p.relid",
								  1, atypes, avalues, NULL, true, 0) !=
			SPI_OK_SELECT)
			elog(ERROR, "could not read the tables of the group");

		/*
		 * The rows of the answer live in the context of the SPI call,
		 * which dies with SPI_finish: the copies of the names and the
		 * numbers the move keeps go to the context of the move
		 */
		MemoryContextSwitchTo(movecxt);
		ntables = SPI_processed;
		tables = palloc0_array(GpMoveTable, ntables);
		for (int k = 0; k < ntables; k++)
		{
			HeapTuple	rt = SPI_tuptable->vals[k];
			TupleDesc	rd = SPI_tuptable->tupdesc;
			bool		isnull;

			gp_move_table(DatumGetObjectId(SPI_getbinval(rt, rd, 1, &isnull)),
						  &policy, &tables[k]);
		}
	}
	SPI_finish();

	/*
	 * The move takes the locks of the gate: the writes of the bucket
	 * wait, the writes of the other buckets answer, and the readers
	 * answer too -- the one-transaction switch of the map keeps every
	 * read on one side of it, and no write loses one.  The tables take a
	 * lock that holds the DDL and the vacuum off the rows while they are
	 * in two places
	 */
	gp_gate_move(policy.colocation_id, bucket);
	for (int k = 0; k < ntables; k++)
	{
		Relation	rel = table_open(tables[k].relid, ShareUpdateExclusiveLock);

		table_close(rel, ShareUpdateExclusiveLock);
	}

	/* The connections of the two segments */
	snprintf(fromportstr, sizeof(fromportstr), "%d", fromport);
	snprintf(toportstr, sizeof(toportstr), "%d", toport);
	fromvals[0] = fromhost;
	fromvals[1] = fromportstr;
	fromvals[2] = gp_database;
	fromvals[3] = "30";
	fromvals[4] = NULL;
	tovals[0] = tohost;
	tovals[1] = toportstr;
	tovals[2] = gp_database;
	tovals[3] = "30";
	tovals[4] = NULL;
	from = PQconnectdbParams(keys, fromvals, 1);
	to = PQconnectdbParams(keys, tovals, 1);
	if (from == NULL || PQstatus(from) != CONNECTION_OK ||
		to == NULL || PQstatus(to) != CONNECTION_OK)
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not connect to the segments of the move"),
				errdetail("old: %s new: %s",
						  from != NULL ? pchomp(PQerrorMessage(from)) : "no connection",
						  to != NULL ? pchomp(PQerrorMessage(to)) : "no connection"));

	PG_TRY();
	{
		MemoryContextSwitchTo(movecxt);

		for (int k = 0; k < ntables; k++)
		{
			GpMoveTable *table = &tables[k];

			snprintf(cursor, sizeof(cursor), "gp_move_%d_c%d", bucket, k);
			if (!gp_move_exec(from, "BEGIN READ ONLY") ||
				!gp_move_open_cursor(from, table, policy.nbuckets, bucket,
									 cursor))
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not read the bucket %d of %s on the old segment (%s)",
							   bucket, table->relqual,
							   pchomp(PQerrorMessage(from))));
			if (!gp_move_exec(to, "BEGIN"))
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not begin the copy on the new segment"));

			for (;;)
			{
				PGresult   *res = libpqsrv_exec(from,
												psprintf("FETCH 500 FROM %s",
														 cursor),
												PG_WAIT_EXTENSION);

				if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK)
					ereport(ERROR,
							errcode(ERRCODE_CONNECTION_FAILURE),
							errmsg("could not fetch the bucket %d of %s",
								   bucket, table->relqual));
				if (PQntuples(res) == 0)
				{
					PQclear(res);
					break;
				}
				for (int r = 0; r < PQntuples(res); r++)
				{
					StringInfoData vals;

					initStringInfo(&vals);
					for (int c = 0; c < table->ncols; c++)
					{
						char	   *lit = PQgetisnull(res, r, c) ?
							pstrdup("NULL") :
							gp_move_literal(&table->outfn[c],
											PQgetvalue(res, r, c), false);

						appendStringInfo(&vals, "%s%s", c > 0 ? ", " : "",
										 lit);
						if (!PQgetisnull(res, r, c))
							pfree(lit);
					}
					if (!gp_move_exec(to, psprintf(
							"INSERT INTO %s (%s) VALUES (%s)",
							table->relqual, table->cols.data, vals.data)))
						ereport(ERROR,
								errcode(ERRCODE_CONNECTION_FAILURE),
								errmsg("could not insert a row of the bucket %d of %s on the new segment",
									   bucket, table->relqual));
					rows++;
				}
				PQclear(res);
			}
			(void) gp_move_exec(from, psprintf("CLOSE %s", cursor));
			if (!gp_move_exec(to, "COMMIT"))
				ereport(ERROR,
						errcode(ERRCODE_CONNECTION_FAILURE),
						errmsg("could not commit the copy of %s",
							   table->relqual));
		}
	}
	PG_CATCH();
	{
		/*
		 * The copy unwinds: the batches of the new segment stay
		 * uncommitted, the map stays as it was, and the cluster answers
		 * as before
		 */
		if (from != NULL)
		{
			(void) gp_move_exec(from, "CLOSE ALL");
			(void) gp_move_exec(from, "ROLLBACK");
		}
		if (to != NULL)
			(void) gp_move_exec(to, "ROLLBACK");
		if (from != NULL)
			PQfinish(from);
		if (to != NULL)
			PQfinish(to);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(movecxt);
		PG_RE_THROW();
	}
	PG_END_TRY();
	(void) gp_move_exec(from, psprintf("CLOSE %s", cursor));
	(void) gp_move_exec(from, "COMMIT");
	PQfinish(from);
	PQfinish(to);
	MemoryContextSwitchTo(oldcxt);

	/*
	 * The switch of the map: one transaction, the sessions drop their
	 * cached policies right after
	 */
	SPI_connect();
	{
		Oid			atypes[3] = {INT4OID, INT4OID, INT4OID};
		Datum		avalues[3];

		avalues[0] = Int32GetDatum(policy.colocation_id);
		avalues[1] = Int32GetDatum(bucket);
		avalues[2] = Int32GetDatum(to_segment);
		if (SPI_execute_with_args("UPDATE gp.colocation_groups "
								  "SET map[$2 + 1] = $3 "
								  "WHERE colocation_id = $1", 3, atypes,
								  avalues, NULL, false, 0) != SPI_OK_UPDATE)
			elog(ERROR, "could not switch the bucket %d of the group %d",
				 bucket, policy.colocation_id);
		for (int k = 0; k < ntables; k++)
		{
			char	   *pcsql = psprintf("SELECT gp.policy_changed(%u::regclass)",
										  tables[k].relid);

			SPI_execute(pcsql, false, 0);
			pfree(pcsql);
		}
	}
	SPI_finish();

	/*
	 * The rows of the bucket go away from the old segment, and the move
	 * is done: the map names the new segment, the rows are on it
	 */
	from = PQconnectdbParams(keys, fromvals, 1);
	if (from == NULL || PQstatus(from) != CONNECTION_OK)
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not connect to the old segment %d for the delete",
					   from_segment));
	for (int k = 0; k < ntables; k++)
	{
		GpMoveTable *table = &tables[k];

		if (!gp_move_exec(from, psprintf(
				"DELETE FROM ONLY %s WHERE gp.bucket(%d, %s) "
				"OPERATOR(pg_catalog.=) %d",
				table->relqual, policy.nbuckets, table->keys.data, bucket)))
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not delete the bucket %d of %s from the old segment",
						   bucket, table->relqual));
	}
	PQfinish(from);

	PG_RETURN_INT32(rows);
}

/*
 * The cleanup of a node that joined the cluster from a copy of the
 * coordinator: the copy holds the rows of the coordinator's own share of
 * every coordinator-share table, and they belong to the coordinator
 * alone.  The node's copies of the distributed tables are empty, and its
 * copies of the replicated ones are whole, the way a segment needs them
 * Returns the number of the truncated tables
 */
PG_FUNCTION_INFO_V1(gp_expand_cleanup);

Datum
gp_expand_cleanup(PG_FUNCTION_ARGS)
{
	int			segment_id = PG_GETARG_INT32(0);
	GpSegment  *segments;
	int			nsegments;
	char		shost[GP_HOST_LEN] = "";
	int			sport = 0;
	const char *keys[5] = {"host", "port", "dbname", "connect_timeout", NULL};
	const char *vals[5];
	char		portstr[16];
	PGconn	   *conn = NULL;
	MemoryContext oldcxt = CurrentMemoryContext;
	StringInfoData truncate_sql;
	int			ntruncated = 0;

	if (gp_role != GP_ROLE_COORDINATOR || !IsUnderPostmaster)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the expansion cleans up on the coordinator only"));

	nsegments = gp_segments_get(&segments);
	if (nsegments == 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("the cluster has no segments"));
	for (int i = 0; i < nsegments; i++)
		if (segments[i].segment_id == segment_id)
		{
			strlcpy(shost, segments[i].host, sizeof(shost));
			sport = segments[i].port;
		}
	if (shost[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("segment %d is not registered", segment_id));

	SPI_connect();

	/*
	 * The coordinator-share tables: hash tables whose map names the
	 * coordinator itself
	 */
	SPI_execute("SELECT p.relid FROM gp.distribution_policy p "
				"JOIN gp.colocation_groups c USING (colocation_id) "
				"WHERE p.kind = 'hash' "
				"AND 0 OPERATOR(pg_catalog.=) ANY (c.map)", true, 0);

	/* The statement built here outlives SPI_finish: its context dies */
	MemoryContextSwitchTo(oldcxt);
	if (SPI_processed > 0)
	{
		bool		first = true;

		initStringInfo(&truncate_sql);
		appendStringInfo(&truncate_sql, "TRUNCATE ");
		for (int k = 0; k < SPI_processed; k++)
		{
			bool		isnull;
			Datum		d = SPI_getbinval(SPI_tuptable->vals[k],
										 SPI_tuptable->tupdesc, 1, &isnull);
			Oid			relid = isnull ? InvalidOid : DatumGetObjectId(d);
			Relation	rel;

			if (!OidIsValid(relid))
				continue;
			rel = table_open(relid, AccessShareLock);
			appendStringInfo(&truncate_sql, "%s%s", first ? "" : ", ",
							 quote_qualified_identifier(
								 get_namespace_name(RelationGetNamespace(rel)),
								 RelationGetRelationName(rel)));
			first = false;
			table_close(rel, AccessShareLock);
			ntruncated++;
		}
	}
	SPI_finish();

	if (ntruncated == 0)
		PG_RETURN_INT32(0);

	snprintf(portstr, sizeof(portstr), "%d", sport);
	vals[0] = shost;
	vals[1] = portstr;
	vals[2] = gp_database;
	vals[3] = "30";
	vals[4] = NULL;
	conn = PQconnectdbParams(keys, vals, 1);
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK)
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("could not connect to the new segment %d at %s:%d for the cleanup",
					   segment_id, shost, sport),
				errdetail("%s", conn != NULL ?
						  pchomp(PQerrorMessage(conn)) : "no connection"));

	PG_TRY();
	{
		if (!gp_move_exec(conn, "BEGIN"))
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not begin the cleanup on the new segment"));
		if (!gp_move_exec(conn, truncate_sql.data))
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not truncate the tables of the coordinator's share on the new segment (%s)",
						   pchomp(PQerrorMessage(conn))));
		if (!gp_move_exec(conn, "COMMIT"))
			ereport(ERROR,
					errcode(ERRCODE_CONNECTION_FAILURE),
					errmsg("could not commit the cleanup on the new segment"));
	}
	PG_CATCH();
	{
		(void) gp_move_exec(conn, "ROLLBACK");
		PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();
	PQfinish(conn);

	PG_RETURN_INT32(ntruncated);
}
