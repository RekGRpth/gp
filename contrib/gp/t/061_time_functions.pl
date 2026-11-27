# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The time functions of the query (current_date, current_timestamp and the
# rest of their family) are the constants of the transaction by the SQL
# standard: the coordinator folds them before the plan and the SQL of the
# segments are built, so every server of the cluster answers the same
# instant, and the answers of the segments are the ones of the whole

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF, VERBOSE) $_[0]"); }

run('CREATE TABLE t (a int, d date, ts timestamptz)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t SELECT g, date '2026-01-01' + (g % 3), '2026-06-01'::timestamptz " .
	'FROM generate_series(1, 1000) g');
# The same rows in a table of the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');

# The constant of the transaction is one group on the cluster as it is one
# here
is(run('SELECT current_date, count(*) FROM t GROUP BY 1'),
	run('SELECT current_date, count(*) FROM l GROUP BY 1'),
	'current_date groups the cluster as one');
is(run('SELECT current_date, avg(a) FROM t GROUP BY 1'),
	run('SELECT current_date, avg(a) FROM l GROUP BY 1'),
	'and averages beside it');
is(run("SELECT count(*) FROM t WHERE d < current_date"),
	run('SELECT count(*) FROM l WHERE d < current_date'),
	'a condition by current_date answers the same on the cluster');
is(run("SELECT count(*) FROM t WHERE ts > current_timestamp - interval '1 day'"),
	run("SELECT count(*) FROM l WHERE ts > current_timestamp - interval '1 day'"),
	'a condition by current_timestamp answers the same');
is(run('SELECT count(*), max(d) FROM t WHERE d <= current_date'),
	run('SELECT count(*), max(d) FROM l WHERE d <= current_date'),
	'the aggregates take the fold in too');

# The SQL of the segments carries the literal of the fold, not the function
like(plan_for('SELECT count(*) FROM t WHERE d < current_date'),
	qr/Remote SQL:.*'::date/s,
	'the SQL of the segments carries the literal of the date');
unlike(plan_for('SELECT count(*) FROM t WHERE d < current_date'),
	qr/Remote SQL:.*current_date/s,
	'and no current_date: the segments need no clock of their own');
like(plan_for("SELECT count(*) FROM t WHERE ts > current_timestamp"),
	qr/Remote SQL:.*'::timestamp with time zone/s,
	'the literal of the timestamp rides the same way');

# The forms of the family fold too
is(run('SELECT localtimestamp::date, count(*) FROM t GROUP BY 1'),
	run('SELECT localtimestamp::date, count(*) FROM l GROUP BY 1'),
	'localtimestamp groups the cluster as one');
is(run("SELECT count(*) FROM t WHERE d > current_date - 2"),
	run("SELECT count(*) FROM l WHERE d > current_date - 2"),
	'current_date in arithmetic folds the same');

$coordinator->stop;
done_testing();
