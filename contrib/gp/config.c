/* -------------------------------------------------------------------------
 *
 * config.c
 *		A parameter of all the nodes of the cluster, set through SQL
 *
 * gp.alter_system() and gp.reset_system() write postgresql.auto.conf of
 * every node the way ALTER SYSTEM does on one, without ssh: the call runs
 * the change of the coordinator itself and dispatches it to the segments
 * in transactions of their own, for ALTER SYSTEM refuses a transaction
 * block.  gp.reload_conf() asks every node to reload.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/config.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "gp.h"

/* The SET or RESET of one parameter, as ALTER SYSTEM parses it */
static void
gp_alter_system_run(const char *name, const char *value)
{
	AlterSystemStmt *stmt = makeNode(AlterSystemStmt);
	VariableSetStmt *set = makeNode(VariableSetStmt);

	set->kind = value != NULL ? VAR_SET_VALUE : VAR_RESET;
	set->name = pstrdup(name);
	set->args = value != NULL ?
		list_make1(makeStringConst(pstrdup(value), -1)) : NIL;
	stmt->setstmt = set;
	AlterSystemSetConfigFile(stmt);
}

/*
 * Write the parameter into postgresql.auto.conf of every node: here, and
 * on the segments in transactions of their own.  The local write goes
 * first, for it validates the name and the value.  On a node of its own
 * (gp.role none) it is the only write.
 */
static void
gp_alter_system_all(const char *name, const char *value)
{
	StringInfoData command;
	StringInfoData undo;

	if (gp_role == GP_ROLE_SEGMENT)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("the cluster is configured on the coordinator only"));

	gp_alter_system_run(name, value);

	if (gp_role != GP_ROLE_COORDINATOR)
		return;

	initStringInfo(&command);
	if (value != NULL)
		appendStringInfo(&command, "ALTER SYSTEM SET %s TO %s",
						 quote_identifier(name), quote_literal_cstr(value));
	else
		appendStringInfo(&command, "ALTER SYSTEM RESET %s",
						 quote_identifier(name));

	initStringInfo(&undo);
	if (value != NULL)
		appendStringInfo(&undo, "ALTER SYSTEM RESET %s",
						 quote_identifier(name));

	gp_dispatch_nonxact(command.data, NULL,
						value != NULL ? undo.data : NULL, 0);
}

/* gp.alter_system(name, value): SET on every node.  The superuser alone */
PG_FUNCTION_INFO_V1(gp_alter_system);

Datum
gp_alter_system(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	text	   *value = PG_GETARG_TEXT_PP(1);

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("only a superuser can configure the cluster"));

	gp_alter_system_all(text_to_cstring(name), text_to_cstring(value));
	PG_RETURN_VOID();
}

/* gp.reset_system(name): RESET on every node.  The superuser alone */
PG_FUNCTION_INFO_V1(gp_reset_system);

Datum
gp_reset_system(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("only a superuser can configure the cluster"));

	gp_alter_system_all(text_to_cstring(name), NULL);
	PG_RETURN_VOID();
}

/*
 * gp.reload_conf(): reread the configuration files of every node, so a
 * parameter written by gp.alter_system() takes effect without a restart.
 * The superuser alone, for it changes the server the session runs on
 */
PG_FUNCTION_INFO_V1(gp_reload_conf);

Datum
gp_reload_conf(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("only a superuser can reload the cluster"));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute("SELECT pg_reload_conf()", false, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not reload the configuration of this node");
	SPI_finish();

	if (gp_role == GP_ROLE_SEGMENT)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("the cluster is configured on the coordinator only"));
	if (gp_role == GP_ROLE_COORDINATOR)
		gp_dispatch_xact("SELECT pg_reload_conf()");
	PG_RETURN_VOID();
}
