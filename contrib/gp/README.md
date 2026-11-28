# gp: a cluster of PostgreSQL servers

`gp` turns several PostgreSQL servers into one MPP cluster in the manner of
Greenplum: a **coordinator** that clients connect to, and **segments** that
hold the data and run parts of queries.  It is an extension plus a single
patch of the server, `new_oid_hook`, rather than a fork.

**Status: under development, not for production.**  The cluster forms,
segments register themselves, and DDL of the coordinator runs on the
segments with the same OIDs.  Distributed queries and transactions run on
the cluster; the roadmap names what remains, see
[Roadmap](#roadmap).

## Requirements

- PostgreSQL master (the development of PostgreSQL 20) with the
  `new_oid_hook` patch.  The extension refuses to build without it
  (`NEW_OID_HOOK_VERSION`).  The patch is proposed to pgsql-hackers; until
  it is in, the server is built from this tree.
- `gp` in `shared_preload_libraries` on every server of the cluster.
- `max_prepared_transactions` above zero on the segments, for two-phase
  commit of DDL.

## Architecture

### Servers and their roles

Every server of a cluster loads `gp` and has a role, `gp.role`:

| `gp.role` | Number, `gp.segment_id` | What it does |
|---|---|---|
| `none` (default) | 0 | Works on its own; `gp` does nothing |
| `coordinator` | 0 | Takes clients, keeps the configuration of the cluster, sends commands to the segments |
| `segment` | 1, 2, ... | Holds data, registers on the coordinator, runs what the coordinator sends |

A segment number is the name of a server, not its position: numbers may
have gaps, which keeps the cluster open to growing and shrinking by moving
buckets (below).  A misconfigured server does not start: a segment needs
`gp.segment_id` and `gp.coordinator`, and other roles may not have them.

### Segments of the cluster (`segments.c`, `gp.c`)

The coordinator keeps the segments in the table `gp.segments` of one
database, `gp.database` (`postgres` by default).  Sessions in any database
need them, so a background worker of the coordinator, the *segments
loader*, keeps a copy in shared memory of `gp.max_segments` entries.  It
reads the table at start and again whenever a transaction that changed it
commits: a statement trigger on the table asks for that, and an object
access hook when the table is dropped.  `gp.cluster_segments()` shows the
copy.

A segment registers itself: at start, its background worker creates the SQL
part of `gp` in `gp.database`, connects to `gp.coordinator`, calls
`gp.register_segment(segment_id, gp.host, port)` there and exits.  While the
coordinator is down or has no extension, the worker retries every
`gp.register_retry_interval`, so segments may start before the coordinator.
A number registered at another address, or an address under another number,
is refused.  The row of a segment in `gp.segments` carries its number,
address and server port, and nothing else.

### Connections and commands (`dispatch.c`)

A session of the coordinator opens a connection to a segment when it needs
it first and keeps it until it ends.  It connects to the database of the
session as the user that logged in, without a password: segments trust the
coordinator by `pg_hba.conf`, certificates or a password file.  `SET
SESSION AUTHORIZATION` may take the session to a role that cannot log in,
so the connection always takes the login user.

Before a command, the segments get the settings of the session: those a
user may change, set for the database, the role, the client or the session
rather than in the configuration of the server, and the role of the
commands, which follows `SET ROLE`, `SET SESSION AUTHORIZATION` and
`SECURITY DEFINER` functions alike, so a command runs on the segments as
the same user as on the coordinator and the objects it creates get the
same owner.  A connection remembers what it has got and gets only the
difference.

`gp.exec_on_all(sql)` sends a command to all segments at once and returns
the command status of each.  Each segment runs it in a transaction of its
own, unless the transaction of the coordinator has sent DDL to the segment
already: then the command runs in that transaction and sees the DDL.  A
cancel or an abort on the coordinator cancels it on the segments.  It is a
tool for maintenance and tests.

### The same OIDs everywhere (`oids.c`)

A plan refers to tables, types and functions by OID, so an object must have
the same OID on every server.  With `new_oid_hook`, an extension may
propose the OID of a new catalog row; the server checks it and refuses one
in use.

- On the coordinator, the hook takes each OID from `GetNewOidWithIndex()`
  itself and records it with its catalog; `gp.recorded_oids()` shows the
  OIDs of the transaction.
- A segment gets the OIDs of a command in `gp.dispatch_oids` and proposes
  them in turn, failing if one is for another catalog, if it needs more, or
  if some are left over at commit: then it has run the command differently.
- A segment creates no objects of its own unless `gp.allow_local_ddl` is
  on.  Such objects get OIDs from the allocator with a large offset; the
  coordinator refuses to assign OIDs in that range, so the ranges cannot
  collide.

Relfilenumbers stay local and may differ.  The registration worker of a
segment uses this local range to create the SQL part of `gp` before
registering, so motion can call its functions without any setup by the user.

### DDL on the segments (`ddl.c`)

A DDL statement on the coordinator runs there first; then the segments run
the same statement, its own text cut out of the query string, with the
OIDs it got on the coordinator.  Statements nested in a sent one (elements
of `CREATE SCHEMA`) and statements of extension scripts are not sent on
their own.  DDL in a function is sent when the function runs on the
coordinator.

A unique index of a distributed table must include the whole key of its
distribution: a segment sees its own rows only, so an index without the
key cannot tell two equal rows apart when they sit on different segments.
A table distributed by hand learns this from `gp.distribute()`, and a
statement that makes such an index on a distributed table fails.

What goes to the segments: `CREATE`, `ALTER` and `DROP` of tables, indexes,
sequences and views; `CREATE SCHEMA` and `DROP SCHEMA`; composite, enum and
range types, domains, `ALTER TYPE ... ADD VALUE`; base and shell types,
operators, aggregates, casts, collations, conversions, operator classes and
families, text search objects, transforms and access methods; extended
statistics, which hold no rows of their own; `CREATE` and
`ALTER` of functions and procedures, whose bodies the segments do not check
(`check_function_bodies = off`), as a body may use a temporary table;
renaming and moving to another schema; roles,
from any database, with `GRANT role`, `ALTER ROLE ... SET`, `REASSIGN
OWNED` and `DROP OWNED`; `GRANT` and `REVOKE` on objects and databases,
`ALTER DEFAULT PRIVILEGES`; `COMMENT`; `OWNER TO`; `CREATE`, `ALTER` and
`DROP EXTENSION`; `TRUNCATE`.  Commands that cannot run
in a transaction block are sent after the coordinator (see below); other
statements run on the coordinator only.

An extension goes as a whole: the segments run its script themselves, with
the OIDs of the coordinator, so it must be installed on them in the same
version, or the statement fails everywhere.  Its objects are then on the
segments, and DDL on them (`GRANT`, `COMMENT`, `ALTER EXTENSION ... SET
SCHEMA`) goes there as well.

What stays on the coordinator: temporary objects (functions and types of
`pg_temp` too) and an object that depends on a temporary one, which stays
with it — a function of a temporary sequence or type, a domain of a
temporary type; one statement may not mix them with other objects.  A table
`LIKE` a temporary one takes its columns on the coordinator, and the
segments make it as the coordinator sees it.  Materialized views and
foreign tables never go to the segments, and the commands on them — an
index, `ALTER`, `GRANT`, `COMMENT`, `REFRESH` — stay on the coordinator
with them.  Not sent yet: triggers, policies, event triggers, tablespaces
and `ALTER DATABASE`.  What is refused, as it would stay on the coordinator
while the commands around it go to the segments: rules on tables of the
cluster.  Extension `gp` is special: its DDL is not dispatched from the
coordinator; the registration worker of every segment creates its own copy
in `gp.database` before registering, so motion can call its functions.
`USAGE` on the schema is granted to `PUBLIC` on every node.
What is refused: `ALTER TABLE` that computes a column with a non-immutable expression (`ADD COLUMN ... DEFAULT now()`, `serial`,
identity, `ALTER COLUMN TYPE ... USING random()`), as each segment would
compute its own values.

The segments run DDL in transactions that follow the transaction of the
coordinator: the first statement opens one, and the end of the transaction
of the coordinator ends them by two-phase commit.  Before the coordinator
commits, each segment runs `PREPARE TRANSACTION 'gp_<xid>'`, `<xid>` being
the transaction of the coordinator; a segment that cannot fails the commit,
and the others roll back.  After the coordinator commits, the segments run
`COMMIT PREPARED`.  An error on a segment at any statement fails the
transaction everywhere.  A subtransaction cannot be rolled back on the
segments yet, so a transaction that rolled back one after sending DDL
cannot commit.  `gp.dispatch_ddl = off` keeps DDL on the coordinator, for
maintenance.

The commit of a transaction that the segments take part in keeps one lock
of the coordinator, which a query that reads the segments takes as well:
from the moment the segments prepare to the moment they all hear `COMMIT
PREPARED`, the query either waits or sees its snapshots taken with no
transaction in between, so every query sees a distributed transaction in
full or not at all.  This makes the commits run one at a time, and a
query wait out the one that is running; a faster scheme can come later.

A transaction that wrote one segment alone ends with a plain `COMMIT`
there, with no prepared transaction: a segment lost in that moment rolls
back rather than stay prepared, and the coordinator warns that its rows
may be lost.

A segment lost between the commit of the coordinator and `COMMIT PREPARED`
keeps its transaction prepared, holding its locks, and the coordinator
warns.  A background worker of the coordinator, `gp recovery`, resolves
what is left: every `gp.recovery_interval` seconds it asks every segment
for the transactions it keeps prepared under a name of ours, looks into
the clog of the coordinator for the fate of each, and runs `COMMIT
PREPARED` or `ROLLBACK PREPARED` on the segment as the clog says — a
coordinator lost before it told is rolled back, as the safest of the two.
A pass runs under the lock of the distributed snapshot above, so it never
touches a transaction whose commit is running.  `gp.recovery_pass_now()`
runs a pass at once.

#### Commands outside a transaction

`CREATE DATABASE`, `DROP DATABASE`, `VACUUM`, `REINDEX`, `CREATE INDEX
CONCURRENTLY` and `DROP INDEX CONCURRENTLY` cannot run in a transaction
block, so the coordinator runs them first and then all segments at once,
each in a transaction of its own.  Two-phase commit does not cover them.  The
coordinator is the first to run them, as it gives the OIDs of `CREATE
DATABASE` and `CREATE INDEX CONCURRENTLY`.  A segment gets them by `SET
gp.dispatch_oids` in the session, with `gp.dispatch_oids_session` on, in a
command of its own, and both are reset after.

When a segment fails, the connections to the segments are closed, which
ends what they keep of the command, and:

- `CREATE DATABASE` is dropped on the segments where it succeeded, and the
  coordinator rolls back; its directory of files may stay there.
- `DROP DATABASE` has removed the files on the coordinator already, so a
  failure on a segment is a `WARNING`.  Run `DROP DATABASE IF EXISTS` again
  to finish.
- `CREATE INDEX CONCURRENTLY` leaves the index on the coordinator and the
  segments that built it, and an invalid one where it failed.  Drop it with
  `DROP INDEX`.

`VACUUM` goes without OIDs, and `VACUUM FULL` lets the segments create
relations for the rewrite (`gp.allow_local_ddl`, so only for superusers).  A
list of tables that mixes temporary ones or those of extension `gp` with
others is refused.  `ANALYZE` runs on the segments too; the coordinator sums
relation sizes over them (counting a replicated table once), imports attribute
statistics from the first segment, and uses the imported sizes and selectivity
for planning.

### Distribution (`distribution.c`)

`gp.distribute(table, key, colocate_with)` distributes an empty table by
hash of its key columns, or replicates it to all segments if the key is
`NULL`.  A replicated table keeps a copy on the coordinator as well:
writes reach every segment and the copy beside, and a query that reads
such a table reads the copy alone, without dispatching anything.  A row goes to a bucket by `gp.bucket()`, the extended hash
functions of the key types combined as hash partitioning does, with a seed
of its own; the bucket goes to a segment by a map.

`gp.move_bucket(table, bucket, segment)` moves a bucket of the
colocation group of a table to another segment: the rows of the bucket
copy to the new segment, the bucket's row of the map switches in one
transaction, and the rows go away from the old segment.  The tables of
the group are locked against the statements of the cluster while the
rows are in two places, and the sessions drop their cached policies
after.  Every hash table of the group moves its share, so the joins by
the key stay local.  Moving the buckets is how a cluster grows and
shrinks: a new segment registers and takes buckets one by one, and a
leaving segment hands out all of its buckets before its row goes away.

`gp.distribute_range(table, key, bounds, segments)` distributes an empty
table by the ranges of one key column: the bounds are the texts of the
type of the key, `bounds[i]` ends the bucket `i` and starts the next, and
the bucket goes to `segments[i]`; the first range holds all below
`bounds[0]`, the last all above the last bound, and they are open above,
so every key of the column has a range.  `gp.distribute_list(table, key,
values, segments)` distributes by the values of one column the same way.
A row whose key falls into no range and matches no value — a NULL key,
for neither distribution has a bucket for it — cannot be inserted.  The
planner answers such tables by their key columns as it answers the tables
of a hash, while the joins that move rows by the hash of the keys are
answered by the coordinator: rows that sit by ranges keep no order by
that hash.

With `coordinator = true` the map of the group names the coordinator
itself for a share of the buckets: those rows live in the copy of the
table on the coordinator, insert through its executor, and read in the
gather beside the answers of the segments; the function of the segment
of a row names them 0.  The rules of the planner that ride the hash of
the keys refuse such tables the way they refuse the ranges and the
values, and `UPDATE` and `DELETE` of a whole shared table are not
supported yet, for the rows of the share keep out of the segments the
statement runs on.

The map belongs to a *colocation group*, not to a table: tables of a group
have keys of the same types and the same number of buckets (1024), so rows
with equal keys are on the same segment and a join by the key is local.  A
new group spreads its buckets over the segments in turn;
`gp.bucket_segment()` reads the map.  Growing or shrinking the cluster will
move buckets, about 1/N of the data, rather than rehash everything.

Policies live in each database (`gp.distribution_policy`,
`gp.colocation_groups`).  An event trigger drops the policy of a dropped
table.

By default a table stays on the coordinator, empty or not.  `SET
gp.default_distribution = first_column` (also for a role or a database)
distributes each new permanent table by its first column that has a hash
function, a deterministic collation and is not generated, as
`gp.distribute()` does; a table with no such column stays.  A partition
goes with its table.  `CREATE TABLE AS` and `SELECT INTO` take three steps
to make such a table: the table without rows on the coordinator, as `WITH NO
DATA` makes it; the same table on the segments, with the same OIDs; and the
rows by `INSERT ... SELECT`.  `CREATE TABLE AS EXECUTE`, a temporary table
and a materialized view stay on the coordinator, and a statement with
parameters is refused.

### COPY into distributed tables (`copy.c`)

The table on the coordinator holds no rows of a hash-distributed table,
so `COPY ... FROM` into one does not run there as usual.  A replicated
table keeps a copy on the coordinator: its rows go to every segment and
into the copy beside, through the constraints and the row triggers of
the table.  The hook reads and parses
the input with the code of `COPY` itself (files, programs, `STDIN`, every
format), evaluates the defaults of columns the input lacks, finds the
segment of each row by its bucket, and sends the rows to the segments as
`COPY ... FROM STDIN (FORMAT binary)`, which takes every type as it is.  A
row of a replicated table goes to all segments and into the copy of the
coordinator.  The segments run the
`COPY` in the transactions that follow the transaction of the coordinator,
so the two-phase commit covers it; they check the constraints and compute
the generated columns themselves.  The command tag counts the rows of the
input.

The privileges are checked as `COPY` does, on the coordinator for the user,
and again on the segments, where the connection is of the same user.
Triggers and constraints of the table do not run on the coordinator, and
`WHERE`, row-level security and `ON_ERROR` other than `stop` are refused.
A failure of a row on a segment, or in the input, fails the statement
everywhere.  The hook does not call the hooks that were set before it for
such a `COPY`.

### INSERT into distributed tables (`insert.c`, `route.c`)

The planner hook takes the plan of an `INSERT` into a distributed table
that the standard planner has made, and puts a custom scan, `GpInsert`, in
place of the `ModifyTable` on top.  The source of the rows stays as it is:
`VALUES`, a `SELECT`, `DEFAULT VALUES`, the defaults of the columns and the
functions in them run on the coordinator, so `now()` and `nextval()` give
one value for all segments.  `GpInsert` takes each row, finds the segment
of its bucket, and sends it as `COPY` does (`route.c` is what the two share).
The segments check the constraints and compute the generated columns.  The
command tag counts the rows, and the transaction of the coordinator covers
them by two-phase commit.

Triggers and constraints of the table do not run on the coordinator.
`RETURNING`, `ON CONFLICT`, row-level security and `WITH CHECK OPTION` are
refused, rather than the rows lost.  An `INSERT ... SELECT` takes the rows of
its `SELECT`, which reads distributed tables as described below.  As the
source may read the segments on the very connections that the rows go on,
`GpInsert` first reads all its rows into a store, and sends them after.

When the source is one hashed table and the `SELECT` reads only its plain
columns, and every target column is filled directly, `GpDml` runs the whole
`INSERT ... SELECT` on the segments instead.  If the source's actual key
columns already hash by the target's distribution key in the same colocation
group, the statement stays local and its `Motion` is false.  Otherwise the
source RTE becomes a `gp.peer_query` marker, planned as `GpMotion`, that hashes
the rows by the target key, using the default routing collation, so each
segment inserts only its own rows.  The target list may reorder the source columns.  Only the needed
source columns are read; an unused one is a `NULL` placeholder in the peer
query, which preserves attribute numbers and column privileges.  Expressions,
defaults, joins, aggregates, limits, replicated tables and a transaction that
has already written stay on the `GpInsert` path, as do dropped columns.
`EXPLAIN (VERBOSE)` shows the remote statement and whether it has a Motion.

When the source is `VALUES` or a `SELECT` without a table, and the key
columns of every row are constants (a default, a folded expression and a
`NULL` count), the planner computes their buckets, and if all fall in one
segment, the plan names it: the rows go to that segment alone, and the
others are not asked, nor opened (*direct dispatch*, as in Greengage).  The
buckets stay in the plan, and the executor checks them against the map
before it relies on the segment.  A key that is a parameter of a prepared
statement, a function or a column of a source table takes the way to all
segments.

### UPDATE and DELETE of distributed tables (`insert.c`)

The rows of UPDATE and DELETE are on the segments, and the statement runs
there, over its own rows, in the transaction of the coordinator, under the
two-phase commit; one segment that wrote alone commits in one phase.  The
plan of the coordinator goes away, and a custom scan, `GpDml`, sends the
statement to the segments on the first call, and answers with the sum of
their counts of rows.  The coordinator takes an exclusive lock on the table
for the whole statement, which runs the writers one at a time and spares
the segments a deadlock of their own.  A replicated table takes its UPDATE
on every segment, over every copy, and on the coordinator, over its own
copy.

What is refused: a change of the distribution key, which would move rows
between segments; `RETURNING`; row-level security; parameters, subqueries,
and anything beyond the target table in the statement, for a segment sees
its own rows of the target table and no rows of the others; `MERGE`, and a
data-modifying statement in WITH.  A statement that matches no row changes
nothing, and answers 0.

### SELECT from distributed tables (`gather.c`)

The rows of a distributed table are on the segments, so a query cannot read
the table on the coordinator.  The hook of the paths of a relation makes
the only path to a distributed table a custom scan, `GpGather`, and the
planner plans the rest, the joins, the aggregates, the sorts and the
windows, on the coordinator as for any table.  A clean `GpGather` answers
in ordinary libpq sessions of the segments: its remote SQL is an ordinary
scan of the table (`EXPLAIN (VERBOSE)` shows it), the rows come back in
the binary format into a tuplestore, from which they go up the plan, and a
rescan, as in a nested loop, reads them from the store again.  A query
with an ORDER BY sorts on the segments, and the gather merges their
sorted answers; the LIMIT of the query rides to the segments only when
the query has no filter of its own, because the remote SQL carries no
WHERE and the filters read the answers above the gather.
The fragments travel in ordinary sessions of the segments: the gather, its
merge, and the slices, whose pull of a motion rides in the query itself as
`gp.peer_query()`.
The conditions of the query and the projection run on the coordinator, so the
result is always correct, if not always cheap: all rows of the table come,
none is chosen on the segments, and the planner does not yet know how many
that is.  A replicated table has all its rows on every segment, so only one of
them is asked, by the process number.  A partitioned table is read as a whole,
the partitions of the coordinator being empty.  Dropped columns are typed NULL
placeholders in the serialized fragment, preserving the table's tuple
descriptor.  `EXPLAIN (VERBOSE)` shows the query that the segments run.

After the transaction has sent a write to the segments, `GpGather` instead
uses their dispatched libpq connections, which belong to the same transaction
and therefore see its writes.  RLS tables and `SET ROLE`/SECURITY DEFINER use
that path too, preserving session settings, `session_user` and `current_user`.
REPEATABLE READ and SERIALIZABLE require an explicit `BEGIN`/`START
TRANSACTION` (or `SET TRANSACTION`) before distributed access.  From then
until `PRE_COMMIT`, a shared lock excludes distributed commits, so a local
coordinator snapshot and the first remote snapshot cannot drift apart; an
implicit transaction with that isolation is refused instead.  Remote
transactions use the same isolation level.  All remote scans of one statement
share a commit-exclusion lock until `ExecutorEnd`, so a distributed commit is
visible everywhere or nowhere and cannot appear between two inputs of the
statement.  A caught subtransaction restores those locks on its abort path.
`FOR UPDATE` and `TABLESAMPLE` on a
distributed table are refused.  `TRUNCATE` goes to the segments with the other
DDL.

The policy of a table is read once, by a session, from `gp.distribution_policy`,
and kept; `gp.distribute()` and the drop of a table invalidate it through
the relcache entry of the table, and a change of an extension drops it.

### Fragments (`fragment.c`, `motion.c`)

`GpMotion`, a clean `GpGather` and a clean immutable `GpSlice` carry a
serialized `PlannedStmt`, not SQL text.  The coordinator leaves
`gp.peer_query()` in a query as the marker of a moving table; its planner hook
replaces the function by the `GpMotion` custom scan, whose scan tuple keeps all
columns of the source.  The SQL function remains as a compatibility wrapper,
but a fragment contains no `FunctionScan` for it.  While the coordinator builds
that fragment, `GpGather` is disabled so that the fragment contains an ordinary
scan.

The peers execute their fragments with `gp.fragment_exec(plan)`: the
deserialized plan is locked by its range table and run by the executor of an
ordinary session of the segment, opened as the user of the query, and its
rows stream back to the asker in the binary format.  A session of a pull
opens with who the rows are for, by the setting `gp.requester_segment`,
which the filter of a redistribution reads, and with the snapshots of the
statement attached.

All readers of one statement share one snapshot per segment.  Before the
first of them starts, and under the statement's distributed commit-exclusion
lock, the coordinator opens a separate READ ONLY REPEATABLE READ connection
to every segment and exports a snapshot there.  No MVCC snapshot crosses a
server boundary: transaction IDs of different servers have no meaning for one
another.  The holder connections do not join the dispatched transaction or
change its write state, and close at the end of the statement, its abort and
its cancellation.  The existing lock keeps distributed commits from appearing
between the exports of different segments.

A pull makes its transaction import the export of its own segment before its
first snapshot, and carries the mapping with it.

The actual OIDs of `gp` are saved by `CREATE EXTENSION` in the coordinator's
data directory.  `gp.extension_oids()` returns them to a registering segment,
which creates its copy with exactly those OIDs, so a serialized plan can
call its functions.  Local maintenance objects can use the separate high
OID range instead.

### Aggregates and joins on the segments (`slice.c`)

When a query groups by the whole key of the distribution, every row of a
group is on one segment, so the segment can compute the group to the end:
`SELECT key, count(*) FROM t GROUP BY key` needs no rows on the
coordinator, only the finished groups of every segment.  Such an
aggregate is replaced after the planner by a custom scan, `GpSlice`,
which runs the aggregate of the query on the segments and reads their
rows; the sort and the limit of the query stay on the coordinator.  A
replicated table reads the copy of the coordinator, so its aggregate
runs there as an ordinary one.  When the grouping consists of plain columns with extended hash
functions but does not cover the distribution key, the rows move by those
grouping columns under the table's bucket map.  Every group lands on one segment again, so the whole
aggregate runs there, including `count`, `avg`, distinct and mixed
aggregates.  Windows, grouping sets, a grouping by expressions, a global
aggregate of a hashed table, and a sorted aggregate that answers the
`ORDER BY` of the query by itself are left to the coordinator.  A junk column of the target list — a
grouping or a sort key that is not selected — is not a stopper: it is
dropped when there is no `ORDER BY`, and sent to the segments as a
column of its own when there is one.

A join runs on the segments too, when any row that joins another is on
the same segment with it: tables of one colocation group joined by the
whole key of the distribution, or a replicated table joined to anything,
or two replicated tables.  An inner join of any number of hashed tables
of different colocation groups runs on the segments too, when its join
graph is connected by equalities of the whole keys.  The first table and
the tables of its group stay by default; every table of another group moves
first: every segment asks every segment — itself among them — for the rows that
the hash of its key places on it under the map of the stationary table, and
joins what comes with its own rows.  With statistics from `ANALYZE`, either
side of a two-way inner join can stay instead: the side with fewer rows moves.
The equalities may form a chain:
they need not all name the first table.  The asking is `GpMotion`, a custom
scan that runs on a segment as the user of the query:
it takes the addresses of the segments from the coordinator at every
call, so a cluster that changed stays correct, and it tells each segment
which segment its rows are for by a setting of the connection.  When two
hashed tables have no such colocation, an inner join broadcasts the second
table by default, or the smaller one when both sides have statistics: its whole
copy goes to every segment, so any join condition,
including an expression or a cross join, works.  A multiway inner join can
instead keep one connected colocated component and broadcast every other hashed
table to all segments, preferring the component with the fewest broadcast rows.
An outer join of two tables
broadcasts its null-supplying side instead: the right side of a left join,
and the left side of a right join.  A full join broadcasts both
sides and runs on one segment, because either side can have an unmatched
row.  The peer connections are autonomous, so they cannot see the writes
of the transaction that asks.  After a transaction has sent commands to
the segments, a new plan uses the gather, whose connections are already
in that transaction; a motion plan made before the write refuses to run
after it.  Every segment creates the SQL part of the extension in its
registration worker, so motion can call its functions there.  The whole
plan of such a join is replaced by
`GpSlice`, which runs the join of the query on the segments; the sort and
the limit stay on the coordinator, and a merge join that answers the
`ORDER BY` by itself is left alone.  Inner, left, right and full joins
are all colocated by the same argument: a row without a partner on its
segment has no partner anywhere.  An outer join of more than two tables,
a replicated table in a non-colocated join of several groups are left to
the coordinator.  An aggregate over a colocated join runs on the segments as
well, when its grouping covers the key of a hashed table that is not
null-supplying; a null-supplying key would leave a separate null group
on every segment.  A broadcast join allows only the key of its stationary
table on that fully local path.  A global aggregate or grouping by other keys
can instead use a partial aggregate over any partitioned join: the coordinator
combines the partial results and performs the final grouping.  This supports
`min`, `max`, and `sum` whose argument, result, and transition types match.
Grouping by an expression is not pushed either: the planned aggregate
no longer matches its parse form there, and a half-matched rewrite would
read the columns of the answer with the types of the join.  Global
aggregates that cannot be answered partly keep the coordinator plan.

A subquery in FROM has a locus of its own: an aggregate of a distributed
table grouped by the key or redistributed by plain grouping columns, or
a join with redistribution or broadcast, runs on the segments even inside
a subquery of a query that is not colocated itself.  The planner
often inlines the plan of a simple subquery with no `SubqueryScan` node,
so the plan below the sort and the limit of the outer query is replaced,
whatever node it is.  A projection, a condition, a sort and a limit of the
outer query keep the locus of the subquery and stay above it, including a
condition the planner pushed into the aggregate's `HAVING` and outputs it
pruned.  A plan that answers the `ORDER BY` by its own order is left alone,
and so is a subquery with a limit of its own.

One derived `FROM` level can also be analyzed recursively.  Its base tables
keep their owning query for Motion, and its output columns can feed a full
aggregate redistributed by the outer grouping columns.  This supports an
all-inner derived join of at most six base inputs; larger derived joins wait
for a cost bound on nested Motion.  A derived outer join of two hashed
tables can feed the same redistributed aggregate: the fragment redistributes
the null-supplying side by the join column under the map of the other side,
so every derived group is complete on one segment; full joins and global
aggregates over a derived table stay on the coordinator, because their
partial target list is not supported.

A correlated subquery can run as part of a local slice when every hashed
input at every supported level is connected to the others by the whole
distribution key; replicated inputs need no such connection.  The analysis
keeps the nesting depth and ancestor queries of every condition.  An outer Var
must have the exact `varlevelsup` of its owning ancestor, so equal RT indexes
of unrelated or sibling levels cannot prove colocation.  Nested correlated
levels are supported under the same rule.

An all-inner query can instead use Motion for its nested hashed inputs, in both
two-way and multiway joins.  Every moved RTE is rewritten in the query that
owns it, never merely at the top level.  This also broadcasts the whole input
of an uncorrelated hashed scalar subquery.  Partial, stationary-key, and
redistributed aggregates are supported above that Motion.  Nested outer joins
stay on the coordinator.  These paths cover correlated `EXISTS`, scalar
aggregates, and outer aggregates without changing the original SQL.

A distributed scan also has hash metadata: the colocation group and the
positions, types and collations of all its distribution-key columns in the
output of the subtree.  A direct projection renumbers these columns; sorting
and limiting leave them unchanged.  Dropping any column of a compound key, or
replacing one by a computed expression, makes the hashing unknown without
changing the `SEGMENTS` locus itself.  Grouping must cover the whole key of the
table; grouping by only a prefix redistributes by the grouping columns instead.

Binary joins compute the locus of both inputs.  A replicated input can be
broadcast to the segments of the other one; two replicated inputs meet on one
segment.  Two distributed inputs keep their hashed locus when they belong to
one colocation group and every distribution-key pair meets by equality.  A full
join can always bring both inputs to one segment.  If this analysis proves no
locus, the existing colocation and Motion rules decide the plan as before.  The
same analysis drives top-level `GpSlice` plans; the construction of a Motion
loads the distribution policies it needs independently of that decision.  An
aggregate is checked against the target list below any `Sort`, whose output
positions are not the attribute numbers of the base table.  `UNION ALL` and its
`Append` preserve a compatible hashed locus; different hash maps make one
distributed union without it, while replicated branches are read on one
segment.  A parallel `Gather` is disabled before planning: `GpSlice` is
distributed itself and must not multiply its full answer in parallel workers.
Mixed replicated/hash branches and `UNION DISTINCT` without a shared hashed
locus are not pushed.  `INTERSECT` and `EXCEPT`, including their `ALL` forms,
replace `HashSetOp` when both inputs share that locus, so equal rows and their
multiplicities are computed on the segments.  A window partition by the whole
distribution key is local to its segment; other partition keys stay on the
coordinator.  An `Incremental Sort` above a distributed result becomes a full
`Sort`, because there is no globally presorted input for it.  A nonrecursive
CTE gets its locus from `PlannedStmt.subplans`; projection, condition, sort
and limit above `CteScan` preserve it.  `DISTINCT` and `DISTINCT ON` are local
when their whole distribution key is in the output: `Unique` stays on the
coordinator above the distributed scan and deduplicates it once more.  A
different key or a prefix of a compound key stays on the coordinator.
A flattened query whose pulled-up subquery has already been dropped is not
deparsed into a remote query; it keeps the coordinator's plan.

An aggregate grouped by expressions, and a global aggregate that does
not move all rows to one segment, can be answered in two steps instead:
the segments aggregate their own rows — grouped by the grouping of the
query, or all rows together when there is none — and the coordinator
finishes the aggregate over the columns of every segment.  The final
aggregate is the same aggregate again, which only `min`, `max` and `sum`
of a type they combine in truly answer; `count` sums the partial counts
up, and the average divides, so it stays on the coordinator with every
query it is in.  `HAVING` stays on the coordinator too, for a group may
pass it only as a whole.

An aggregate of one hashed table, grouped by columns that do not cover
its distribution key, answers in two phases instead of moving its rows:
every segment computes a partial aggregate over its own rows, the
partials of all the segments answer one combine query in which every
segment combines the groups the hash of the grouping columns assigns to
it — the fragment reads its own number from `gp.segment_id` — and the
coordinator finishes the aggregate over the per-segment answers, so no
row of the table crosses a session boundary.  The fragment is built as
text: the partial aggregate moves as a serialized plan through
`gp.peer_query()`, like the motion of a join, and the combine query
groups and aggregates its columns.  The marker of the partial aggregate
is wrapped in the same ownership filter, so a segment computes the
partials of the groups of the asking segment only.  The combining
aggregate runs over
the result type of the original one and comes back to it through a cast
when they differ, as `sum` of `int8` answers in `numeric`.  `count`
moves through a `sum` of the partial counts, and the average divides on
the coordinator, which finishes it over the sums of the partial sums
and counts the combine query keeps; distinct and ordered aggregates are
not splittable and take the row-moving fallback.
Queries with volatile functions in their conditions or aggregate
arguments, with external parameters, or after a write in the same
transaction are not pushed either; a table pinned to one segment is read
there alone.

`EXPLAIN (VERBOSE)` shows the query of the segments.

## Trying it

Build the tree with Meson or make; `contrib/gp` is built with the rest.
Then, with the server installed:

```sh
export PGBIN=/path/to/install/bin     # or have its pg_config in PATH
contrib/gp/tools/gp-cluster.sh init   # coordinator on 9000, segments on 9001-9003
                                      # with max_prepared_transactions = 64
contrib/gp/tools/gp-cluster.sh start  # creates extension gp, waits for segments
contrib/gp/tools/gp-cluster.sh status
psql -h localhost -p 9000 postgres -c 'SELECT * FROM gp.cluster_segments()'
contrib/gp/tools/gp-cluster.sh destroy
```

`GP_CLUSTER_DIR`, `GP_PORT` and `GP_SEGMENTS` change the directory
(`~/gp-cluster`), the first port and the number of segments.

To see where the cluster differs from a server on its own, run tests of
PostgreSQL on the running cluster, with tables distributed by their first
column:

```sh
export PG_REGRESS=/path/to/build/src/test/regress/pg_regress
contrib/gp/tools/gp-pg-regress.sh test_setup select join aggregates
```

The diffs are in `$GP_CLUSTER_DIR/regress`.  A test needs the tables of those
it depends on, as in the schedule.  The script drops the tablespace of the
previous run, so a rerun on the same cluster works when the previous run
succeeded; roles of a failed run linger, and call for `gp-cluster.sh destroy`,
`init` and `start` again.  Not every difference is a mistake: tablespaces,
materialized views and the statements listed above as not sent are known.

A test that differs on purpose keeps a copy of its output as
`pgtests/expected/foo.gp.out`, which the script puts in place of the answer
of PostgreSQL; the numbered answers of such a test are dropped, so a cluster
answer cannot hide behind one of them.  The refusals of non-immutable
expressions, `CREATE TABLE AS EXECUTE`, the artifacts of a failed
`CREATE INDEX CONCURRENTLY` and a command on a temporary table together
with others have answers of their own this way.

The manifest `pgtests/MANIFEST` keeps the SHA-256 of the base answer of
PostgreSQL as it was when each answer of our own was written.  Before the
tests run, and after the daily merge of upstream, `tools/gp-gp-manifest.sh
check` compares the hashes: a base answer that changed upstream leaves the
answer of our own stale, and an answer that grew equal to its base again
is asked for removal.  After refreshing the answers, `update` rewrites the
manifest.

CI runs the tests of `tools/gp-pg-regress.tests` this way on every push,
weekly and on demand (`gp-pg-regress.yml`); a failing test fails the job,
which keeps the diffs as an artifact.

## Reference

### Settings

All are set at server start unless noted.

| Setting | Default | Meaning |
|---|---|---|
| `gp.role` | `none` | `none`, `coordinator` or `segment` |
| `gp.segment_id` | 0 | Number of the server: 0 on the coordinator, from 1 on segments |
| `gp.database` | `postgres` | Database with the configuration of the cluster |
| `gp.coordinator` | | Connection string of the coordinator; on segments only |
| `gp.host` | `localhost` | Host of a segment for the other servers |
| `gp.register_retry_interval` | 5s | Time between attempts of a segment to register |
| `gp.max_segments` | 64 | Size of the copy of `gp.segments` on the coordinator |
| `gp.recovery_interval` | 5s | Time between the passes of the recovery worker, on the coordinator |
| `gp.dispatch_oids` | | OIDs of a command from the coordinator; set per command by the coordinator, not in the configuration of a server |
| `gp.dispatch_oids_session` | off | Keeps `gp.dispatch_oids` for the next transaction, for a command out of a transaction block |
| `gp.allow_local_ddl` | off | Lets a segment create objects of its own; superuser, for maintenance |
| `gp.dispatch_ddl` | on | Sends DDL of the coordinator to the segments; superuser, for maintenance |
| `gp.autoanalyze` | off | Analyzes the distributed tables the cluster has changed past the threshold of autovacuum, at the start of a statement of the coordinator; settable by a user |
| `gp.default_distribution` | `coordinator` | `first_column` distributes new tables by their first column; settable by a user |

### Functions

| Function | What it does |
|---|---|
| `gp.register_segment(segment_id, host, port)` | Register a segment; called by the segment itself |
| `gp.cluster_segments()` | Segments as the coordinator knows them, from any database |
| `gp.node_state()` | The state of the nodes of the cluster: a short connection of the coordinator tells a live segment from a dead one, with the reason; open to anyone |
| `gp.stop_node()` | Stop the server of the node the function runs on, the way `pg_ctl -m fast` does; the node is expected back with its own start |
| `gp.stop_segment(segment_id)` | The stop of `gp.stop_node()`, dispatched to a segment from the coordinator |
| `gp.log_tail(lines)` | The tail of the newest log file of the logging collector of this node; a node without the collector refuses with a hint of what to turn on |
| `gp.alter_system(name, value)` | Write a parameter into `postgresql.auto.conf` of every node, the way `ALTER SYSTEM` writes one node; takes effect with `gp.reload_conf()` |
| `gp.reset_system(name)` | Undo `gp.alter_system()` on every node |
| `gp.reload_conf()` | Reread the configuration files of every node, so a written parameter takes effect without a restart |
| `gp.exec_on_all(sql)` | Run a command on all segments, return the status of each |
| `gp.recorded_oids()` | OIDs of the new catalog rows of the transaction on the coordinator |
| `gp.distribute(table, key, colocate_with, segment, coordinator)` | Distribute an empty table by hash of `key`, or replicate it, or pin it to a segment; `coordinator = true` names the coordinator a keeper of its share of the buckets, and its rows live in the copy of the table here |
| `gp.distribute_range(table, key, bounds, segments)` | Distribute an empty table by the ranges of one key column: `bounds[i]` ends the bucket `i` and starts the next, bucket `i` goes to `segments[i]`; the bounds are the texts of the type of the key and must ascend |
| `gp.distribute_list(table, key, values, segments)` | Distribute an empty table by the values of one key column: `values[i]` makes the bucket `i`, which goes to `segments[i]` |
| `gp.bucket(nbuckets, VARIADIC key)` | Bucket of a key |
| `gp.bucket_segment(colocation_id, bucket)` | Segment of a bucket of a colocation group |
| `gp.policy_changed(relation)` | Make every session read the policy of a table again; `gp.distribute()` calls it |
| `gp.segment_id()` | The segment this server is: 0 on the coordinator, and its number on a segment.  A query the segments answer takes the value of the server that read every row: a plain read names the segment of each of its rows, a grouping keyed by it keeps every group on one segment — the skew of the data by the segments — and a condition by it names the segments whose rows pass.  An aggregate above a gather answers it as 0, for no row of theirs is the answer of one server |
| `gp.dispatches()` | How many statements and row streams of this session have gone to the segments.  A query over the tables that keep their rows on the coordinator leaves it unchanged |
| `gp.shrink_segment(segment)` | Shrink the cluster: every bucket the segment keeps moves to the segments that stay, the segment stops, and its row goes away; a mirror of the segment leaves beside it (the standby server itself stops by the deployment tool); superusers only |
| `gp.expand_segment(segment)` | Finish the joining of a segment that started from a copy of the coordinator: the coordinator-share rows truncate on it, and the buckets of every hash group spread over the registered segments; superusers only |
| `gp.rebalance_segments()` | Spread the buckets of every hash colocation group over the registered segments by the even share; superusers only |

`gp.register_segment()`, `gp.exec_on_all()`, `gp.distribute()`, `gp.stop_node()`,
`gp.stop_segment()`, `gp.log_tail()`, `gp.alter_system()`, `gp.reset_system()`
and `gp.reload_conf()` are for superusers only.

## Tests

`sql/gp.sql` runs on a single server with `gp` preloaded.  TAP tests in `t/`
start their own clusters: a segment is a node named `segment-N-PORT`, the
name it gets as cluster_name, so the processes and the logs of a run name
their port, as do the directories of the demo cluster of
`tools/gp-cluster.sh`.  `GpTest::start_cluster()` starts a coordinator and
the segments with these settings, and `GpTest::same_answers()` compares the answer of a
distributed query with the answer of its local copy and, on a mismatch, leaves
the facts in the log before the cluster goes away: the two answers, a fresh
repeat, the answer of every segment over its own rows, the state of the nodes
and the dispatch counters.

| Test | What it checks |
|---|---|
| `001_cluster.pl` | A coordinator and segments start with their roles |
| `002_gp_cluster.pl` | `tools/gp-cluster.sh` through its whole cycle |
| `003_register.pl` | Registration rules; misconfigured servers do not start |
| `004_register_worker.pl` | Segments register themselves, before or after the coordinator starts |
| `005_cluster_segments.pl` | The copy of `gp.segments` follows commits, rollbacks and drops |
| `006_exec_on_all.pl` | Commands on all segments, errors, broken connections, cancels |
| `007_session_settings.pl` | Settings of the session reach the segments |
| `008_distribution.pl` | Distribution policies and colocation groups |
| `009_same_oids.pl` | Objects get the OIDs of the coordinator on a segment |
| `010_ddl.pl` | DDL of the coordinator on the segments: statements, transactions, what stays, what is refused |
| `011_two_phase.pl` | Two-phase commit of DDL, a segment that cannot prepare, a segment lost before `COMMIT PREPARED` |
| `012_roles.pl` | Roles, privileges, comments and owners on the segments |
| `013_nonxact.pl` | `CREATE DATABASE`, `DROP DATABASE`, `VACUUM` and the `CONCURRENTLY` commands on the segments, and their failures |
| `014_extension.pl` | Extensions on the segments with the same OIDs: create, update, cascade, move, drop, a failing segment; extension `gp` stays |
| `015_copy.pl` | `COPY FROM` into distributed and replicated tables: placement by bucket, types, transactions, errors, privileges |
| `016_insert.pl` | `INSERT` into distributed and replicated tables: `VALUES`, `SELECT`, defaults, types, transactions, prepared statements, redistribution of a plain distributed `SELECT`, colocated inserts without Motion, safe fallbacks and privileges |
| `017_direct_dispatch.pl` | `INSERT` with constant keys goes to one segment alone: which segments are asked, folded constants, `NULL` keys, what is not direct |
| `018_gather.pl` | `SELECT` from distributed tables against the same rows in ordinary tables: aggregates, joins, windows, rescans, replicated and partitioned tables, types, own writes, privileges, what is refused |
| `019_default_distribution.pl` | `gp.default_distribution`: the key of a new table, partitions, temporary tables, `CREATE TABLE AS`, `SELECT INTO`, a database's setting |
| `020_dtx_snapshot.pl` | The lock of the distributed snapshot: a reader waits out a commit stopped between its prepare and `COMMIT PREPARED`; writers and readers together keep every transaction whole |
| `021_one_phase.pl` | A transaction that wrote one segment commits there in one phase; a segment lost in the window rolls back and its rows are lost with a warning |
| `022_unique_key.pl` | Unique indexes of a distributed table must include the whole key of the distribution; equal keys stay on one segment, and its index tells them apart |
| `023_update_delete.pl` | `UPDATE` and `DELETE` run on the segments: distributed and replicated tables, rollbacks, a second writer waiting out the first, what is refused |
| `024_recovery.pl` | The recovery worker of the coordinator: a segment lost during `COMMIT PREPARED` has its transaction committed, a coordinator lost between the prepare and its commit has it rolled back on the segments |
| `025_agg_pushdown.pl` | Aggregates on the segments: grouping by the key of the distribution, redistribution by other hashable columns, replicated tables, subqueries, inheritance boundaries, unhashable-type, own-write, session-setting and dropped-column fallbacks |
| `026_join_pushdown.pl` | Joins and aggregates over them on the segments: one colocation group joined by the key, replicated tables, outer joins, joins of three and more tables, what is not pushed, the same answers as on the coordinator |
| `027_subquery_pushdown.pl` | Aggregates and colocated joins inside a subquery of a non-colocated query run on the segments; projections, conditions, sorts and limits keep that locus and renumber hash keys, including pushed `HAVING`, pruned outputs and compound keys; equal hash keys prove colocation without Motion; a derived all-inner level can join a base table and feed a redistributed aggregate, and a derived left join of two hashed tables can feed one too; limits and full-join derived shapes keep their fallbacks |
| `029_redistributed_join.pl` | `GpMotion` replaces the peer-query marker; inner joins of two and more colocation groups redistribute by the whole key; without one, a two-table join broadcasts the second table under any condition, outer joins broadcast the null-supplying side, and full joins broadcast both sides to one segment; expressions, cross joins, aggregates, subqueries, writes and dropped columns are checked |
| `032_gp_manifest.pl` | The manifest of the answers of our own: a base answer changed upstream is caught before the tests turn red, and an answer that equals its base again is asked for removal |
| `033_multiple_slices.pl` | A root and its self-motion reader are separate READ ONLY transactions with one exported snapshot per segment; a segment-local commit between their starts changes nothing, nested cancellation and statement end close every snapshot holder, and a later write still sees itself |
| `034_union_locus.pl` | `UNION ALL` and `Append`: compatible hash keys, reversed and compound projections, different hash maps, replicated inputs, sort and limit, a parallel `Gather` removed, `UNION DISTINCT`, `INTERSECT` and `EXCEPT` when equal rows share a hashed locus; mixed replicated/hash and `UNION DISTINCT` without it stay on the coordinator |
| `035_window_locus.pl` | Window partitions by the whole distribution key: `row_number`, `rank` and `sum`, compound keys, multiple `WindowAgg` nodes, replicated inputs, and `Incremental Sort` converted to a full sort above the distributed answer |
| `036_cte_locus.pl` | Nonrecursive materialized CTEs: the locus comes from `PlannedStmt.subplans`, while projection, a condition, sort and limit above `CteScan` preserve it; replicated CTEs, and why outer joins, multiple CTEs and recursion stay on the coordinator |
| `037_distinct_locus.pl` | `DISTINCT` and `DISTINCT ON`: the whole distribution key in the output, compound keys and their prefixes, replicated inputs, and `Unique` kept above the distributed scan to preserve ordering |
| `028_partial_agg.pl` | Two-step aggregates: `min`, `max` and `sum` by expressions and global ones are answered partly on the segments and finished on the coordinator; `count` and `avg` stay whole on this path |
| `042_correlated_locus.pl` | Correlated and nested subqueries: exact ancestor ownership, colocated and replicated inputs, two-way and multiway Motion in the owning query level, uncorrelated hashed scalar broadcast, partial, stationary-key and redistributed aggregates, independent sibling broadcast, differential answers, and the outer-join fallback |
| `043_multiway_broadcast.pl` | A multiway join keeps a colocated component and broadcasts the other hashed tables to every segment; replicated inputs need no Motion, and the answer is checked against the coordinator over and over, for a multiway slice was once seen racing |
| `044_tpch.pl` | All 22 TPC-H queries on a small deterministic data set: every answer matches ordinary `local_*` tables, and the expected set of distributed-slice plans is fixed. Query texts come from DuckDB 1.5.4 `extension/tpch/dbgen/queries`; this is a correctness and plan regression, not a dbgen or performance benchmark |
| `045_initplan_locus.pl` | An init plan that aggregates over the gather of one distributed table is answered by the segments with partial columns: every segment answers a partial sum and a partial count of the average, and the final aggregate of the init plan reads them |
| `046_sublink_antijoin.pl` | A join the sublinks drive alone — a semi, anti or scalar sublink over other tables — keeps the table of the fromlist and broadcasts every table of a sublink whole: the rows of the answer are the rows of the kept table, so a customer with an order on another segment is not kept |
| `047_replicated_copy.pl` | A replicated table keeps a copy on the coordinator: every write reaches the segments and the copy beside, and every read of such a table takes the copy alone, without a word to the segments, which `gp.dispatches()` proves |
| `048_segment_pinned.pl` | A table pinned to a segment keeps every row on that segment alone: the buckets of its own colocation group all point there, the reads gather from it alone, and the writes reach it by the ordinary routing; the pin is refused without a registered segment, without a key, and with a table that is not empty |
| `049_stop_node.pl` | A node stops without ssh: the superuser asks the coordinator, which dispatches the stop to the segment, and the segment sends the fast shutdown to its own server; the segment comes back with its own start and registers again, the coordinator refuses to stop itself this way, and the functions are for the superuser alone |
| `050_log_tail.pl` | The log of any node of the cluster is read through SQL: the tail of the newest log file of the logging collector, of as many lines as asked; a node without the collector refuses with a hint, the segment answers from its own log, and the read is for the superuser alone |
| `051_node_state.pl` | The state of the nodes through SQL: every segment answers `up`, a stopped one answers `down` with the reason and comes back live with its own start, the addresses are the ones the cluster knows, and an ordinary role reads the state |
| `052_alter_system.pl` | A parameter of the whole cluster through SQL: `gp.alter_system()` reaches every node and `gp.reload_conf()` makes it served, a quoted value survives the trip, an unknown parameter changes nothing, the functions are for the superuser alone, and a segment refuses them |
| `053_multiway_outer.pl` | A chain of base tables joined by one kind of an outer join keeps the rows its every join keeps: a left join chain stays on the segments with the first table and broadcasts the rest, a full join chain broadcasts everything to one segment, null keys keep their rows, mixed levels and aggregates of null-supplying columns stay on the coordinator, and the answers match the coordinator |
| `054_fragment_snapshots.pl` | A fragment of a slice with motions attaches the exported snapshots of its statement: pull workers parked through a segment-local commit answer from the state before it, never a mix of both, and the next statement sees the commit |
| `055_two_phase_agg.pl` | An aggregate grouped by columns that do not cover the distribution key answers in two phases: the fragment combines a serialized partial aggregate through `gp.peer_query()` and the ownership filter reads `gp.segment_id`; the answers match the local ones |

Besides, CI runs the whole test suite of PostgreSQL twice: on the patched
server without `gp`, and with `gp` preloaded on a single server, to show
that the patch and the extension change nothing while not in use.
`tools/check-core-diff.sh` checks that the tree differs from upstream only
by the patch and `contrib/gp`.

## Roadmap

1. **Cluster skeleton** — done: roles, registration, the copy of segments,
   connections, settings of sessions, distribution policies.
2. **DDL and the same OIDs** — done: the same OIDs, propagating DDL
   with roles, privileges, comments and owners, its two-phase commit,
   commands that run outside a transaction and extensions.
3. **Placing data and correct SELECT** — in progress: `COPY`, `INSERT`,
   direct dispatch of `INSERT` and `SELECT` through a gather are done, and
   the 159 tests of the schedule of PostgreSQL all pass on the cluster,
   twelve of them against answers of our own in `pgtests/expected/`: the
   refusals of non-immutable expressions, `CREATE TABLE AS EXECUTE`, the
   artifacts of a failed `CREATE INDEX CONCURRENTLY`, a command on a
   temporary table together with others.  `gp.segment_id()` of a query the
   segments answer is the value of the server that read every row: a plain
   read names the segment of each of its rows, a grouping by it answers the
   skew of the data by the segments, and a condition by it names the
   segments whose rows pass; next are stable functions of time on the
   coordinator and a comparison with a vanilla server.
4. **Distributed transactions and DML** — done: two-phase commit of all
   writes, and one phase for a transaction that wrote one segment;
   distributed snapshots; `UPDATE` and `DELETE` on the segments; unique
   indexes made to include the key of the distribution; and the recovery
   worker that resolves the transactions lost segments keep prepared.
5. **MPP planner** — in progress: the locus model and the rules on top of
   it.  Aggregates whose grouping covers the key of the distribution run on
   the segments, colocated joins of any number of tables too, and other
   groupings are answered partly on the segments; projections, conditions,
   sorts and limits keep a distributed locus through derived tables in FROM;
   binary joins prove colocation from both hashed inputs, and joins whose
   tables are not colocated move them by Motion (item 6); window partitions
   by the whole distribution key are local; `UNION ALL`, `UNION DISTINCT`,
   `INTERSECT` and `EXCEPT` keep compatible hashed loci; nonrecursive CTEs
   inherit that locus through their subplans, above joins and through init
   plans; aggregates of CTEs over joins run on the segments; joins the
   sublinks drive keep the table of the fromlist; `DISTINCT` is local when
   its whole key is in the output; `INSERT ... SELECT` skips Motion when
   source and target are already colocated; a level of a fromlist whose
   joins mix kinds rides too, when every join of the level keeps the rows
   of its leftmost table: the leftmost table partitions the answer, so the
   slice keeps it on its segments and every other table of the level
   broadcasts whole — an inner join over a chain of LEFT joins, the semi,
   anti and scalar sublinks of the level beside them, and a chain of FULL
   joins answered on one segment; a level whose outer joins mix kinds — a
   FULL join beside a LEFT join, a RIGHT join beside them — answers on one
   segment too, where every table comes whole and the joins and the
   sublinks of the level replay the way they would here.  What stays on
   the coordinator: joins whose graph is not connected by keys, a level
   of such tables that keeps a derived table on it, and queries the rules
   cannot prove safe — the fallback always gives a right answer.
6. **Motions** — done: a `GpMotion` custom scan moves tables by
   redistribution or broadcast over a serialized-plan transport with binary
   tuples; clean
   gathers and immutable slices use the same transport, while mutable plans,
   special session semantics and a transaction's writes stay on its dispatched
   connections; multiple slices of one statement
   on a segment are READ ONLY readers of one exported snapshot; an outer join
   broadcasts the null-supplying side — in chains of one kind of an outer join
   as well: a chain of left joins keeps its first table on the segments and
   broadcasts the rest, and a chain of full joins broadcasts every table to
   one segment.
7. **Statistics** — done: manual `ANALYZE` runs on the segments (relation
   sizes are summed over them, attribute statistics are sampled from the
   first segment, and the planner uses the imported cluster sizes), and
   `gp.autoanalyze` analyzes the distributed tables the cluster has
   changed past the threshold autovacuum names: the start of a statement
   sums the change counters of the segments, and the tables past the
   threshold take the same analyze and import.
8. **More kinds of distribution** — replicated with the coordinator, on the
   coordinator only, on one segment, by the ranges of a key column, by the
   values of one, and with the coordinator keeping a share of the buckets
   of a hash.  A row goes to the segment of the range that holds
   its key or of the value that equals it, and a key with no range and no
   value has no segment: such a row cannot be inserted.  The planner
   answers by the key columns as it answers by the columns of a hash; the
   joins of such tables are answered by the coordinator, for the rules of
   a join ride the hash of the keys
9. **Management without ssh** — done: systemd units, the state of the
   nodes, the stop of a node, the tail of a log, and settings on all
   servers.

10. **Growth and shrink of the cluster** — done: mirrors (a mirror
   streams from its primary and takes its place when the primary dies; a
   mirror of a shrinking primary may stay and take its place), the move
   of a bucket of a colocation group (`gp.move_bucket`), the even-share
   rebalance of the buckets over the segments
   (`gp.rebalance_segments`), the shrink of a segment
   (`gp.shrink_segment`), and the growth of one (`gp.expand_segment`
   joins a copy of the coordinator as a segment, `gp.expand_cleanup`
   cleans its copies of the tables).  A move gates the writes by the
   advisory locks of the group: a write of the moving bucket waits,
   the writes of the other buckets and the readers answer while the
   rows are in two places, and a write that runs on the segments holds
   the whole group, for it may touch any bucket of it.  The map commits
   in one transaction, so no read doubles a row and no write loses one.

Later: the repair of a move that a crash breaks in the middle; the
queries whose derived tables ride mixed levels.

## License

PostgreSQL License, see [LICENSE](LICENSE).
