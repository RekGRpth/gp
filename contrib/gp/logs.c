/* -------------------------------------------------------------------------
 *
 * logs.c
 *		The tail of the server log of this node, read through SQL
 *
 * A reference book of the cluster lives on the segments, and so do the
 * logs: without ssh, the superuser reads the log of any node through
 * SQL.  The function answers the tail of the newest log file of the
 * logging collector; with the collector off, the log goes wherever the
 * administrator of the node has pointed the standard error of the
 * server, and no file of its own is known here.
 *
 * Copyright (c) 2026, Georgy Shelkovy
 * Licensed under the PostgreSQL License, see LICENSE
 *
 * IDENTIFICATION
 *		contrib/gp/logs.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include <sys/stat.h>
#include "utils/builtins.h"

/* The tail is read in one piece, of this size at most */
#define GP_LOG_TAIL_LIMIT (1024 * 1024)

typedef struct GpLogTailState
{
	char	  **lines;			/* the lines of the tail */
	int			nlines;			/* how many there are */
	int			next;			/* the one the next call answers */
} GpLogTailState;

/* The newest .log file of the log directory, or NULL when there is none */
static char *
gp_log_newest(void)
{
	DIR		   *dir;
	struct dirent *entry;
	char	   *path = NULL;
	char	   *fulldir;
	time_t		newest = 0;

	if (is_absolute_path(Log_directory))
		fulldir = pstrdup(Log_directory);
	else
		fulldir = psprintf("%s/%s", DataDir, Log_directory);

	dir = AllocateDir(fulldir);
	if (dir == NULL)
		return NULL;
	while ((entry = ReadDir(dir, fulldir)) != NULL)
	{
		struct stat st;
		char	   *candidate;
		size_t		len = strlen(entry->d_name);

		if (len < 4 ||
			strncmp(entry->d_name + len - 4, ".log", 4) != 0)
			continue;
		candidate = psprintf("%s/%s", fulldir, entry->d_name);
		if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) &&
			st.st_mtime >= newest)
		{
			newest = st.st_mtime;
			path = candidate;
		}
		else
			pfree(candidate);
	}
	FreeDir(dir);
	return path;
}

/* Read the last lines of the file into the state, in one piece */
static void
gp_log_tail_read(GpLogTailState *state, int nlines)
{
	char	   *path = gp_log_newest();
	FILE	   *file;
	struct stat st;
	size_t		want;
	char	   *buffer;
	char	   *cut;
	int			count = 0;
	char	   *q;
	char	   *line;

	if (path == NULL)
		return;
	file = AllocateFile(path, "rb");
	if (file == NULL)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("could not open the log file \"%s\"", path));
	if (fseek(file, 0, SEEK_END) != 0 ||
		((st.st_size = ftell(file)) < 0) ||
		fseek(file, 0, SEEK_SET) != 0)
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("could not size the log file \"%s\"", path));

	/*
	 * The tail is read in one piece, of a megabyte at most: the first
	 * line of the piece may be cut in the middle, and that is the price
	 * of not reading a log of a gigabyte whole.
	 */
	want = (size_t) Min((long long) st.st_size, (long long) GP_LOG_TAIL_LIMIT);
	buffer = palloc(want + 1);
	if (want > 0 &&
		(fseek(file, (long) (st.st_size - (off_t) want), SEEK_SET) != 0 ||
		 fread(buffer, 1, want, file) != want))
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("could not read the log file \"%s\"", path));
	buffer[want] = '\0';
	FreeFile(file);

	/* The cut before the last nlines lines: count newlines from the end */
	cut = buffer;
	for (q = buffer + want - 1; q >= buffer; q--)
	{
		if (*q == '\n')
		{
			if (++count == nlines + 1)
			{
				cut = q + 1;
				break;
			}
		}
	}

	for (line = cut; line < buffer + want;)
	{
		char	   *next = strchr(line, '\n');

		if (next != NULL)
			*next = '\0';
		else
			next = buffer + want;
		if (state->nlines == 0)
			state->lines = palloc(sizeof(char *));
		else
			state->lines = repalloc(state->lines, sizeof(char *) *
									(state->nlines + 1));
		state->lines[state->nlines++] = pstrdup(line);
		line = next + 1;
	}
}

/*
 * The last lines of the newest log file of this node.  The superuser
 * alone, for the log carries the texts of other users' statements.
 */
PG_FUNCTION_INFO_V1(gp_log_tail);

Datum
gp_log_tail(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	GpLogTailState *state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		int32		nlines;

		if (!superuser())
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					errmsg("only a superuser can read the log of a node"));

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		nlines = PG_GETARG_INT32(0);
		if (nlines <= 0)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("the number of lines must be positive"));
		if (!Logging_collector)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("logging_collector is off"),
					errhint("Set logging_collector and reload, and the log of the node becomes readable here."));

		state = (GpLogTailState *) palloc0(sizeof(GpLogTailState));
		funcctx->user_fctx = state;
		gp_log_tail_read(state, nlines);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (GpLogTailState *) funcctx->user_fctx;

	if (state == NULL || state->next >= state->nlines)
		SRF_RETURN_DONE(funcctx);

	SRF_RETURN_NEXT(funcctx,
					CStringGetTextDatum(state->lines[state->next++]));
}
