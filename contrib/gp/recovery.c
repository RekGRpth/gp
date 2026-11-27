/* -------------------------------------------------------------------------
 *
 * recovery.c
 *		Recovery of the transactions the segments keep prepared
 *
 * A distributed transaction is committed on the segments by COMMIT
 * PREPARED, which reaches them one by one, and a segment that misses it
 * keeps the transaction prepared, holding its locks and its rows from
 * everyone.  The coordinator used to leave such a transaction to a human;
 * a background worker of the coordinator resolves it now.  Every few
 * seconds it asks every segment for the prepared transactions whose
 * global name is one of ours, looks into the clog of the coordinator for
 * the fate of the transaction, and commits or rolls back what it finds.
 * The fate is what the clog says: committed, aborted, and what it does
 * not know — the transaction the coordinator died in the middle of — it
 * rolls back, as the safest of the two.
 *
 * The worker takes the lock of the distributed snapshot for a pass, so it
 * never resolves a transaction whose commit is running right now: that
 * one holds the lock from the moment the segments are all prepared to the
 * moment they all hear COMMIT PREPARED, and the worker waits it out.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/recovery.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "gp.h"

/*
 * The helpers header wraps the libpq functions in versions that attach the
 * results to the memory context they are made in, so a result goes away with
 * its context even when an error jumps over its clearing.  A pass works in a
 * context of its own for that, and takes its results with it when it ends.
 */

#define GP_RECOVERY_RESTART 5

/* The wait event of the connections of the worker to the segments */
static uint32 gp_recovery_wait_event = 0;

/* Seconds between the passes of the worker */
static int	gp_recovery_interval = 5;

pg_noreturn PGDLLEXPORT void gp_recovery_worker_main(Datum main_arg);

static void gp_recovery_pass(void);

PG_FUNCTION_INFO_V1(gp_recovery_pass_now);

Datum
gp_recovery_pass_now(PG_FUNCTION_ARGS)
{
	gp_recovery_pass();
	PG_RETURN_VOID();
}

/*
 * Set up the worker and its setting on the coordinator; called from _PG_init()
 */
void
gp_recovery_init(void)
{
	BackgroundWorker worker = {0};

	if (gp_role != GP_ROLE_COORDINATOR)
		return;

	DefineCustomIntVariable("gp.recovery_interval",
							"Seconds between the passes of the recovery of the transactions the segments keep prepared.",
							NULL,
							&gp_recovery_interval,
							5,
							1,
							INT_MAX,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = GP_RECOVERY_RESTART;
	strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "gp_recovery_worker_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "gp recovery", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "gp recovery", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}

static bool
gp_recovery_segment(int segment_id, const char *shost, int sport)
{
	const char *keywords[] = {"host", "port", "dbname", "application_name",
	NULL};
	const char *values[5];
	char		port[12];
	PGconn	   *conn = NULL;
	PGresult   *res;
	int			nrows;

	snprintf(port, sizeof(port), "%d", sport);
	values[0] = shost;
	values[1] = port;
	values[2] = gp_database;
	values[3] = "gp recovery";
	values[4] = NULL;

	PG_TRY();
	{
		conn = libpqsrv_connect_params(keywords, values, false,
									   gp_recovery_wait_event);
		if (PQstatus(conn) != CONNECTION_OK)
		{
			elog(WARNING, "could not connect to segment %d at %s:%d: %s",
				 segment_id, shost, sport,
				 pchomp(PQerrorMessage(conn)));
			libpqsrv_disconnect(conn);
			conn = NULL;
			return false;
		}

		res = libpqsrv_exec_params(conn,
								   "SELECT gid FROM pg_prepared_xacts "
								   "WHERE gid LIKE 'gp\\_%'",
								   0, NULL, NULL, NULL, NULL, 0,
								   gp_recovery_wait_event);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			elog(WARNING, "could not read the prepared transactions of segment %d: %s",
				 segment_id, pchomp(PQresultErrorMessage(res)));
			PQclear(res);
			libpqsrv_disconnect(conn);
			conn = NULL;
			return false;
		}

		nrows = PQntuples(res);
		for (int i = 0; i < nrows; i++)
		{
			char	   *gid = pstrdup(PQgetvalue(res, i, 0));
			const char *fate;
			uint64		xid64;
			TransactionId xid;
			StringInfoData cmd;
			PGresult   *done;

			/* The fate of the transaction, as the clog of the coordinator says */
			xid64 = strtoull(gid + strlen("gp_"), NULL, 10);
			{
				FullTransactionId fxid;

				fxid.value = xid64;
				xid = XidFromFullTransactionId(fxid);
			}

			if (TransactionIdIsInProgress(xid))
			{
				elog(DEBUG1, "transaction %u of %s is still running", xid, gid);
				continue;
			}
			if (TransactionIdDidCommit(xid))
				fate = "COMMIT";
			else
			{
				/*
				 * Aborted, or the coordinator died before it said: the
				 * safest of the two is to undo
				 */
				fate = "ROLLBACK";
				if (!TransactionIdDidAbort(xid))
					elog(WARNING, "the fate of transaction %u of %s is unknown; rolling it back",
						 xid, gid);
			}

			initStringInfo(&cmd);
			appendStringInfoString(&cmd, fate);
			appendStringInfo(&cmd, " PREPARED '%s'", gid);

			done = libpqsrv_exec_params(conn, cmd.data, 0, NULL, NULL, NULL,
										NULL, 0, gp_recovery_wait_event);
			pfree(cmd.data);
			if (PQresultStatus(done) == PGRES_COMMAND_OK)
				elog(LOG, "%s PREPARED '%s' on segment %d", fate, gid,
					 segment_id);
			else
			{
				const char *sqlstate = PQresultErrorField(done, PG_DIAG_SQLSTATE);

				/*
				 * Another pass, or the commit of the transaction itself, has
				 * resolved it already
				 */
				if (sqlstate == NULL || strcmp(sqlstate, "42704") != 0)
					elog(WARNING, "could not %s PREPARED '%s' on segment %d: %s",
						 fate, gid, segment_id,
						 pchomp(PQresultErrorMessage(done)));
			}
			PQclear(done);
		}

		PQclear(res);
		libpqsrv_disconnect(conn);
		conn = NULL;
	}
	PG_CATCH();
	{
		/* What the segment still keeps prepared stays for a later pass */
		libpqsrv_disconnect(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return true;
}

/*
 * One pass: every segment, under the lock of the distributed snapshot.  The
 * pass runs in a context of its own, thrown away at its end: whatever the
 * pass leaves behind goes with the context, and an error that jumps over the
 * clearing of a result does not outlive the pass either.  No error of a pass
 * gets out: the worker tries again later.
 */

/*
 * The watch of the mirrors: a pair of a primary and its physical standby.
 * A primary that stops answering hands the identity of its segment to its
 * mirror: the mirror promotes, takes the extension of the segments, and
 * takes the address of the segment in the copy; the row of the mirror
 * goes away.  The pass runs on the coordinator, in the database of the
 * cluster
 */
/* One statement of a connection, tolerantly */
static bool
gp_mirror_exec(PGconn *conn, const char *sql)
{
	PGresult   *res = libpqsrv_exec(conn, sql, gp_recovery_wait_event);
	bool		ok = res != NULL &&
		(PQresultStatus(res) == PGRES_COMMAND_OK ||
		 PQresultStatus(res) == PGRES_TUPLES_OK);

	if (res != NULL)
		PQclear(res);
	return ok;
}

static bool
gp_mirrors_take_over(int segment_id)
{
	const char *keys[5] = {"host", "port", "dbname", "connect_timeout", NULL};
	const char *vals[5];
	MemoryContext oldcxt = CurrentMemoryContext;
	MemoryContext passcxt = AllocSetContextCreate(TopMemoryContext,
												  "gp mirrors take over",
												  ALLOCSET_SMALL_SIZES);
	const char *dirs;
	char		sockdir[108];
	char		cport[16];
	char		mhost[GP_HOST_LEN] = "";
	int			mport = 0;
	bool		found = false;
	PGconn	   *self = NULL;
	PGconn	   *mirror = NULL;
	PGresult   *res;
	const Oid	atypes[1] = {INT4OID};
	char		sidstr[16];
	const char *avalues[1];
	char	   *oids = NULL;

	StartTransactionCommand();
	MemoryContextSwitchTo(passcxt);

	/* The socket directory and the port of the coordinator, for the
	 * loopback: the statements run as SQL of the database of the cluster,
	 * out of the worker's process, for the pass keeps no state of its own
	 */
	dirs = GetConfigOption("unix_socket_directories", false, false);
	{
		int			n = 0;

		while (dirs != NULL && dirs[n] != '\0' && dirs[n] != ',')
			n++;
		strlcpy(sockdir, dirs != NULL ? dirs : "/tmp",
				n + 1 > sizeof(sockdir) ? sizeof(sockdir) : n + 1);
	}
	snprintf(cport, sizeof(cport), "%d", PostPortNumber);
	snprintf(sidstr, sizeof(sidstr), "%d", segment_id);
	avalues[0] = sidstr;
	vals[0] = sockdir;
	vals[1] = cport;
	vals[2] = gp_database;
	vals[3] = "5";
	vals[4] = NULL;

	/*
	 * The mirror of the segment and the oids of the extension, over the
	 * loopback
	 */
	self = libpqsrv_connect_params(keys, vals, false, gp_recovery_wait_event);
	if (self == NULL || PQstatus(self) != CONNECTION_OK)
	{
		elog(WARNING, "the watch of the mirrors lost the coordinator");
		if (self != NULL)
			libpqsrv_disconnect(self);
		MemoryContextSwitchTo(oldcxt);
		CommitTransactionCommand();
		MemoryContextDelete(passcxt);
		return false;
	}

	res = libpqsrv_exec_params(self,
							   "SELECT host, port FROM gp.mirrors "
							   "WHERE segment_id = $1", 1, atypes, avalues,
							   NULL, NULL, 0, gp_recovery_wait_event);
	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		elog(WARNING, "could not read the mirror of segment %d (%s)", segment_id,
			 res != NULL ? pchomp(PQresultErrorMessage(res)) : "no result");
		if (res != NULL)
			PQclear(res);
		libpqsrv_disconnect(self);
		MemoryContextSwitchTo(oldcxt);
		CommitTransactionCommand();
		MemoryContextDelete(passcxt);
		return false;
	}
	found = PQntuples(res) == 1;
	if (found)
	{
		strlcpy(mhost, PQgetvalue(res, 0, 0), GP_HOST_LEN);
		mport = atoi(PQgetvalue(res, 0, 1));
	}
	PQclear(res);

	if (!found)
	{
		/* No mirror keeps the segment: the pass resolves what it can */
		libpqsrv_disconnect(self);
		MemoryContextSwitchTo(oldcxt);
		CommitTransactionCommand();
		MemoryContextDelete(passcxt);
		return false;
	}

	res = libpqsrv_exec(self, "SELECT gp.extension_oids()::text",
						gp_recovery_wait_event);
	if (res != NULL && PQresultStatus(res) == PGRES_TUPLES_OK &&
		PQntuples(res) == 1)
		oids = pstrdup(PQgetvalue(res, 0, 0));
	if (res != NULL)
		PQclear(res);
	libpqsrv_disconnect(self);

	/*
	 * The mirror promotes and takes the extension of the segments, with
	 * the oids of ours, the way a segment takes them at its first start
	 */
	vals[0] = mhost;
	vals[1] = psprintf("%d", mport);
	vals[2] = gp_database;
	vals[3] = "5";
	vals[4] = NULL;
	mirror = libpqsrv_connect_params(keys, vals, false, gp_recovery_wait_event);
	if (mirror == NULL || PQstatus(mirror) != CONNECTION_OK)
	{
		elog(WARNING, "the mirror of the segment %d at %s:%d does not answer",
			 segment_id, mhost, mport);
		if (mirror != NULL)
			libpqsrv_disconnect(mirror);
		MemoryContextSwitchTo(oldcxt);
		CommitTransactionCommand();
		MemoryContextDelete(passcxt);
		return false;
	}

	/*
	 * An earlier pass may have promoted the mirror already: the identity
	 * below moves once, whatever the pass took to get here
	 */
	if (!gp_mirror_exec(mirror, "SELECT pg_promote(true, 30)"))
		elog(WARNING, "the mirror of the segment %d did not promote this pass",
			 segment_id);
	if (oids != NULL)
	{
		char	   *set = psprintf("SET gp.dispatch_oids = '%s'", oids);

		if (!gp_mirror_exec(mirror, set))
			elog(WARNING, "the mirror %d took no oids of ours", segment_id);
		pfree(set);
	}
	if (!gp_mirror_exec(mirror, "CREATE EXTENSION IF NOT EXISTS gp"))
		elog(WARNING, "the mirror %d took no extension of ours", segment_id);
	libpqsrv_disconnect(mirror);

	/*
	 * The segment takes the address of the mirror, and the row of the
	 * mirror goes away: the copy of the segments reloads on the commit,
	 * and the sessions reconnect by the known rules
	 */
	vals[1] = cport;
	self = libpqsrv_connect_params(keys, vals, false, gp_recovery_wait_event);
	if (self != NULL && PQstatus(self) == CONNECTION_OK)
	{
		char	   *sql = psprintf("UPDATE gp.segments SET host = %s, port = %d "
								   "WHERE segment_id = %d",
								   quote_literal_cstr(mhost), mport,
								   segment_id);

		if (!gp_mirror_exec(self, sql))
			elog(WARNING, "the identity of the segment %d did not move (%s)",
				 segment_id, pchomp(PQerrorMessage(self)));
		pfree(sql);
		sql = psprintf("DELETE FROM gp.mirrors WHERE segment_id = %d",
					   segment_id);
		if (!gp_mirror_exec(self, sql))
			elog(WARNING, "the mirror row of the segment %d did not go (%s)",
				 segment_id, pchomp(PQerrorMessage(self)));
		pfree(sql);
		elog(WARNING, "the identity of the segment %d moved to %s:%d",
			 segment_id, mhost, mport);
	}
	if (self != NULL)
		libpqsrv_disconnect(self);

	MemoryContextSwitchTo(oldcxt);
	CommitTransactionCommand();
	MemoryContextDelete(passcxt);
	return true;
}

void
gp_recovery_pass(void)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	MemoryContext passcxt = AllocSetContextCreate(TopMemoryContext,
												  "gp recovery pass",
												  ALLOCSET_SMALL_SIZES);
	bool		locked = false;

	PG_TRY();
	{
		GpSegment  *segments;
		int			nsegments;

		MemoryContextSwitchTo(passcxt);
		nsegments = gp_segments_get(&segments);
		if (nsegments > 0)
		{
			gp_dtx_exclusive_enter();
			locked = true;
			for (int i = 0; i < nsegments; i++)
			{
				/*
				 * Copy the address out: the array is shared with the
				 * loader, which rewrites it when the cluster changes
				 */
				char		shost[GP_HOST_LEN];
				int			sid = segments[i].segment_id;
				int			sport = segments[i].port;

				strlcpy(shost, segments[i].host, sizeof(shost));
				if (!gp_recovery_segment(sid, shost, sport))
					gp_mirrors_take_over(sid);
			}
		}
	}
	PG_CATCH();
	{
		EmitErrorReport();
		FlushErrorState();
		MemoryContextSwitchTo(oldcxt);
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(passcxt);

	if (locked)
		gp_dtx_exclusive_exit();
}

pg_noreturn void
gp_recovery_worker_main(Datum main_arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(gp_database, NULL, 0);
	gp_recovery_wait_event = WaitEventExtensionNew("GpRecovery");

	for (;;)
	{
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		gp_recovery_pass();

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 gp_recovery_interval * 1000L,
						 PG_WAIT_EXTENSION);
	}
}
