/* -------------------------------------------------------------------------
 *
 * gp.c
 *		Cluster of PostgreSQL servers: a coordinator and segments
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/gp.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "catalog/catalog.h"
#include "catalog/pg_extension_d.h"
#include "catalog/pg_namespace_d.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "port/atomics.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "gp.h"

#if !defined(NEW_OID_HOOK_VERSION) || NEW_OID_HOOK_VERSION < 1
#error "gp requires a server with new_oid_hook"
#endif

PG_MODULE_MAGIC_EXT(
					.name = "gp",
					.version = PG_VERSION
);

static const struct config_enum_entry gp_role_options[] = {
	{"none", GP_ROLE_NONE, false},
	{"coordinator", GP_ROLE_COORDINATOR, false},
	{"segment", GP_ROLE_SEGMENT, false},
	{NULL, 0, false}
};

static const struct config_enum_entry gp_default_distribution_options[] = {
	{"coordinator", GP_DEFAULT_COORDINATOR, false},
	{"first_column", GP_DEFAULT_FIRST_COLUMN, false},
	{NULL, 0, false}
};

/* GUC Variables */
int			gp_role = GP_ROLE_NONE;

/*
 * The number of statements and row streams that have gone to the segments
 * since the start of the coordinator.  The counter lives in the shared
 * memory, for the backends of one session each carry their own copy of
 * the globals of the module
 */
static pg_atomic_uint64 *gp_dispatches_shmem = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

static void gp_shmem_request(void);

/*
 * The shared memory is requested in the hook the server calls before it
 * makes its own
 */
static void
gp_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(sizeof(pg_atomic_uint64));
}

static pg_atomic_uint64 *
gp_dispatches_object(void)
{
	if (gp_dispatches_shmem == NULL)
	{
		bool		found;

		gp_dispatches_shmem = ShmemInitStruct("gp dispatches",
											  sizeof(pg_atomic_uint64),
											  &found);
		if (!found)
			pg_atomic_init_u64(gp_dispatches_shmem, 0);
	}
	return gp_dispatches_shmem;
}

void
gp_dispatches_bump(void)
{
	pg_atomic_fetch_add_u64(gp_dispatches_object(), 1);
}
int			gp_segment_id = 0;
int			gp_row_segment = 0;
bool		gp_autoanalyze = false;
bool		gp_local_routing = false;
static int	gp_requester_segment = 0;
char	   *gp_database = NULL;
int			gp_max_segments = 64;
int			gp_default_distribution = GP_DEFAULT_COORDINATOR;
static char *gp_coordinator = NULL;
static char *gp_host = NULL;
static int	gp_register_retry_interval = 5;

pg_noreturn PGDLLEXPORT void gp_register_main(Datum main_arg);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* The role is set at server start, so the hooks come only from there */
	if (!process_shared_preload_libraries_in_progress)
		return;

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = gp_shmem_request;

	DefineCustomEnumVariable("gp.role",
							 "Role of the server in the cluster.",
							 "With none, the server works on its own.",
							 &gp_role,
							 GP_ROLE_NONE,
							 gp_role_options,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("gp.segment_id",
							"Number of the server in the cluster.",
							"0 on the coordinator, from 1 on segments.",
							&gp_segment_id,
							0,
							0,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("gp.requester_segment",
							"Segment for which a motion connection returns rows.",
							"Set by another segment on a connection that pulls rows.",
							&gp_requester_segment,
							0,
							0,
							INT_MAX,
							PGC_USERSET,
							GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("gp.database",
							   "Database that holds the configuration of the cluster.",
							   "Segments register in this database of the coordinator.",
							   &gp_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("gp.coordinator",
							   "Connection string of the coordinator.",
							   "Required on a segment, which registers there.",
							   &gp_coordinator,
							   "",
							   PGC_POSTMASTER,
							   GUC_SUPERUSER_ONLY,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("gp.host",
							   "Host name or address of the server for the other servers of the cluster.",
							   "A segment registers with this host and its port.",
							   &gp_host,
							   "localhost",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomIntVariable("gp.register_retry_interval",
							"Time between attempts of a segment to register on the coordinator.",
							NULL,
							&gp_register_retry_interval,
							5,
							1,
							3600,
							PGC_POSTMASTER,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("gp.max_segments",
							"Maximum number of segments in the cluster.",
							"The coordinator keeps the segments in shared memory of this size.",
							&gp_max_segments,
							64,
							1,
							10000,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("gp.default_distribution",
							 "Where a new table goes if it is not distributed by hand.",
							 "With coordinator, it stays on the coordinator; with first_column, it is distributed by its first column that can be a key.",
							 &gp_default_distribution,
							 GP_DEFAULT_COORDINATOR,
							 gp_default_distribution_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable("gp.dispatch_oids",
							   "OIDs of the objects of a command from the coordinator.",
							   "Set on a segment before a command, as \"catalog:oid\" pairs separated by commas.",
							   &gp_dispatch_oids,
							   "",
							   PGC_USERSET,
							   GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							   gp_check_dispatch_oids,
							   gp_assign_dispatch_oids,
							   NULL);

	DefineCustomBoolVariable("gp.dispatch_oids_session",
							 "Keeps the OIDs of gp.dispatch_oids for the next transaction.",
							 "Set with gp.dispatch_oids on a segment, for a command that cannot run in a transaction block.",
							 &gp_dispatch_oids_session,
							 false,
							 PGC_USERSET,
							 GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("gp.allow_local_ddl",
							 "Allows a segment to create objects of its own.",
							 "Objects of a segment get their OIDs from the coordinator, so this is for maintenance only.",
							 &gp_allow_local_ddl,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("gp.dispatch_ddl",
							 "Sends DDL of the coordinator to the segments.",
							 "Off, DDL runs on the coordinator only, for maintenance.",
							 &gp_dispatch_ddl,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("gp.autoanalyze",
							 "Analyzes the distributed tables the cluster has changed past the threshold of autovacuum.",
							 "The check runs at the start of a statement of the coordinator, sums the change counters of the segments, and analyzes the tables whose cluster has changed more than autovacuum_analyze_threshold plus autovacuum_analyze_scale_factor of their rows; the statistics of the cluster land on the coordinator the way a manual ANALYZE brings them.",
							 &gp_autoanalyze,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("gp");

	/* A misconfigured node does not start */
	if (gp_role == GP_ROLE_SEGMENT && gp_segment_id == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.segment_id\" must be set on a segment"));
	if (gp_role == GP_ROLE_COORDINATOR && gp_segment_id != 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.segment_id\" is set, but \"gp.role\" is \"coordinator\""));
	if (gp_role == GP_ROLE_SEGMENT && gp_coordinator[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.coordinator\" must be set on a segment"));
	if (gp_role == GP_ROLE_COORDINATOR && gp_coordinator[0] != '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("\"gp.coordinator\" is set, but \"gp.role\" is \"coordinator\""));

	if (gp_role == GP_ROLE_COORDINATOR)
	{
		gp_segments_init();
		gp_dispatch_init();
		gp_recovery_init();
		gp_ddl_init();
		gp_insert_init();
		gp_gather_init();
		gp_slice_init();
	}

	gp_motion_init();
	gp_oids_init();

	if (gp_role == GP_ROLE_SEGMENT)
	{
		BackgroundWorker worker = {0};

		worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
			BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = gp_register_retry_interval;
		strlcpy(worker.bgw_library_name, "gp", BGW_MAXLEN);
		strlcpy(worker.bgw_function_name, "gp_register_main", BGW_MAXLEN);
		strlcpy(worker.bgw_name, "gp register segment", BGW_MAXLEN);
		strlcpy(worker.bgw_type, "gp register segment", BGW_MAXLEN);
		RegisterBackgroundWorker(&worker);
	}
}

/* The connection string of the coordinator, for the motion of a segment */
const char *
gp_coordinator_conninfo(void)
{
	return gp_coordinator;
}

/*
 * Create the SQL part of the extension, with the OIDs the coordinator
 * recorded at its creation
 */
static void
gp_segment_extension_create(const char *oids)
{
	if (SPI_execute("SET gp.dispatch_oids_session = on", false, 0) != SPI_OK_UTILITY ||
		SPI_execute(psprintf("SET gp.dispatch_oids = %s",
							 quote_literal_cstr(oids)), false, 0) != SPI_OK_UTILITY ||
		SPI_execute("CREATE EXTENSION gp", false, 0) != SPI_OK_UTILITY)
		ereport(ERROR,
				errmsg("could not create extension gp on segment %d",
					   gp_segment_id));
}

/*
 * Create the SQL part of the extension on a segment, with the actual OIDs
 * recorded by the coordinator at its creation
 */
static void
gp_segment_extension(PGconn *conn)
{
	PGresult   *res;
	char	   *oids;
	bool		isnull;
	Datum		extoid;

	BackgroundWorkerInitializeConnection(gp_database, NULL, 0);
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_execute("SELECT oid FROM pg_extension WHERE extname = 'gp'",
					true, 1) != SPI_OK_SELECT)
		ereport(ERROR,
				errmsg("could not look for extension gp on segment %d",
					   gp_segment_id));
	if (SPI_processed == 0)
	{
		res = libpqsrv_exec_params(conn,
								   "SELECT 1 FROM pg_extension WHERE extname = 'gp'",
								   0, NULL, NULL, NULL, NULL, 0, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			ereport(ERROR,
					errmsg("could not look for extension gp on the coordinator"));
		if (PQntuples(res) == 0)
		{
			PQclear(res);
			PopActiveSnapshot();
			SPI_finish();
			CommitTransactionCommand();
			return;
		}
		PQclear(res);
		res = libpqsrv_exec_params(conn,
								   "SELECT gp.extension_oids(), extnamespace "
								   "FROM pg_extension WHERE extname = 'gp'",
								   0, NULL, NULL, NULL, NULL, 0, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1 ||
			PQgetisnull(res, 0, 0))
			ereport(ERROR,
					errmsg("could not read extension OIDs on the coordinator"));
		oids = pstrdup(PQgetvalue(res, 0, 0));

		/*
		 * The schema of the control file depends on the extension but is
		 * not a member of it: DROP EXTENSION leaves it on the coordinator,
		 * and a recreate of the extension there records no OID of it.  A
		 * segment without the schema takes the OID of the schema of the
		 * coordinator, so the replay of a fresh segment agrees with the
		 * replay of a recreated one
		 */
		if (gp_oids_extension_oid(oids, NamespaceRelationId) == InvalidOid &&
			!PQgetisnull(res, 0, 1))
			oids = psprintf("%u:%u,%s", (unsigned) NamespaceRelationId,
							(unsigned) strtoul(PQgetvalue(res, 0, 1), NULL, 10),
							oids);
		PQclear(res);
		gp_segment_extension_create(oids);
	}
	else
	{
		/*
		 * The extension is here already.  Its OIDs must be the ones the
		 * coordinator has now: a recreate of the extension there left the
		 * copy of the segment with the OIDs of the older creation, so it
		 * is dropped and created again with the list of the coordinator,
		 * in one transaction: a failure puts the old copy back
		 */
		extoid = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
							   1, &isnull);
		res = libpqsrv_exec_params(conn, "SELECT gp.extension_oids()",
								   0, NULL, NULL, NULL, NULL, 0, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
			ereport(ERROR,
					errmsg("could not read extension OIDs on the coordinator"));
		if (!PQgetisnull(res, 0, 0))
		{
			oids = pstrdup(PQgetvalue(res, 0, 0));
			if (gp_oids_extension_oid(oids, ExtensionRelationId) !=
				DatumGetObjectId(extoid))
			{
				ereport(LOG,
						errmsg("the extension gp of segment %d has the OIDs of an older creation",
							   gp_segment_id),
						errdetail("The coordinator has recreated the extension; the segment creates it again."));
				if (SPI_execute("DROP EXTENSION gp", false, 0) != SPI_OK_UTILITY)
					ereport(ERROR,
							errmsg("could not drop extension gp on segment %d",
								   gp_segment_id));
				gp_segment_extension_create(oids);
			}
		}
		PQclear(res);
	}
	PopActiveSnapshot();
	SPI_finish();
	CommitTransactionCommand();
}

/* The address of this segment for other nodes */
const char *
gp_host_address(void)
{
	return gp_host;
}

/*
 * Background worker of a segment: create the SQL part of the extension for
 * motion, then register the segment on the coordinator and exit.  Exit code 0
 * tells the postmaster not to start the worker again; on a failure, exit code
 * 1 makes it retry after gp.register_retry_interval, so a segment may start
 * before the coordinator.
 */
void
gp_register_main(Datum main_arg)
{
	/* A second dbname overrides one in the connection string */
	const char *keywords[] = {"dbname", "dbname", "application_name", NULL};
	const char *values[] = {gp_coordinator, gp_database, "gp register segment", NULL};
	char		segment_id[12];
	char		port[12];
	const char *params[3] = {segment_id, gp_host, port};
	uint32		wait_event;
	PGconn	   *conn;
	PGresult   *res;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	wait_event = WaitEventExtensionNew("GpRegisterSegment");

	conn = libpqsrv_connect_params(keywords, values, true, wait_event);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		ereport(LOG,
				errmsg("could not connect to the coordinator: %s",
					   pchomp(PQerrorMessage(conn))));
		libpqsrv_disconnect(conn);
		proc_exit(1);
	}

	PG_TRY();
	{
		gp_segment_extension(conn);
	}
	PG_CATCH();
	{
		EmitErrorReport();
		AbortCurrentTransaction();
		FlushErrorState();
		libpqsrv_disconnect(conn);
		proc_exit(1);
	}
	PG_END_TRY();

	snprintf(segment_id, sizeof(segment_id), "%d", gp_segment_id);
	snprintf(port, sizeof(port), "%d", PostPortNumber);
	res = libpqsrv_exec_params(conn,
							   "SELECT gp.register_segment($1, $2, $3)",
							   3, NULL, params, NULL, NULL, 0, wait_event);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		ereport(LOG,
				errmsg("could not register as segment %d on the coordinator: %s",
					   gp_segment_id, pchomp(PQerrorMessage(conn))));
		PQclear(res);
		libpqsrv_disconnect(conn);
		proc_exit(1);
	}
	PQclear(res);
	libpqsrv_disconnect(conn);
	ereport(LOG,
			errmsg("registered as segment %d on the coordinator at %s:%d",
				   gp_segment_id, gp_host, PostPortNumber));
	proc_exit(0);
}

/*
 * gp.segment_id(): the segment this server is.  A row of a distributed
 * table keeps its segment, but the planner runs everything above the scan
 * on the coordinator, where no row tells its own: per-row answers come
 * with the pushdown of stage 5, and until then a query that wants the
 * segment of a row filters on it, or takes the answer of the server.
 */
PG_FUNCTION_INFO_V1(gp_segment_id_fn);

Datum
gp_segment_id_fn(PG_FUNCTION_ARGS)
{
	/*
	 * The row a gather of the coordinator hands out came from one segment,
	 * and the gather names it in gp_row_segment while the row is the
	 * answer; the rest of the time the function is the segment this server
	 * is, which is 0 on the coordinator
	 */
	PG_RETURN_INT32(gp_row_segment != 0 ? gp_row_segment : gp_segment_id);
}

PG_FUNCTION_INFO_V1(gp_dispatches_fn);

Datum
gp_dispatches_fn(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pg_atomic_read_u64(gp_dispatches_object()));
}
