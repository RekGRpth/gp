# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The statistics of the cluster without a hand: while gp.autoanalyze is
# on, the start of every statement of the coordinator sums the change
# counters of the segments, and the tables the cluster has changed more
# than the threshold autovacuum names are analyzed the way a manual
# ANALYZE brings their statistics to the coordinator.  The ANALYZE of the
# segments resets their counters, so the tables keep waiting only for the
# changes after it

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

# The GUC lives between the sessions only in the database
run('ALTER DATABASE postgres SET gp.autoanalyze = on');

run('CREATE TABLE t (a int, b text)');
run("SELECT gp.distribute('t', ARRAY['a'])");

# Below the threshold of autovacuum (50 rows plus a tenth) nothing fires
run("INSERT INTO t SELECT g, 'x' || g FROM generate_series(1, 40) g");
is(run("SELECT reltuples::bigint FROM pg_class WHERE oid = 't'::regclass"),
	-1, 'the changes under the threshold wait');

# Past it, the next statement of the coordinator analyzes and imports
run("INSERT INTO t SELECT g, 'y' || g FROM generate_series(1, 60) g");
is(run("SELECT reltuples::bigint FROM pg_class WHERE oid = 't'::regclass"),
	100, 'the cluster statistics land after the threshold');
ok(run("SELECT count(*) > 0 FROM pg_stats WHERE tablename = 't'"),
	'the sample of the segments is imported with the sizes');
ok($segments[0]->safe_psql('postgres',
		"SELECT last_analyze IS NOT NULL FROM pg_stat_user_tables " .
		"WHERE relname = 't'"),
	'the segments were analyzed');

# More changes, one more refresh, exact again
run("INSERT INTO t SELECT g, 'z' || g FROM generate_series(1, 900) g");
is(run("SELECT reltuples::bigint FROM pg_class WHERE oid = 't'::regclass"),
	1000, 'the refresh follows the changes of the cluster');
is(run('SELECT count(*) FROM t'), 1000, 'and the rows are the truth');

# The check leaves the tables of the system alone
run('CREATE EXTENSION IF NOT EXISTS pageinspect');
ok(run('SELECT true'), 'a statement with the check of the catalogs runs');

# While the GUC is off, the statistics stay as they are
run('ALTER DATABASE postgres RESET gp.autoanalyze');
run("INSERT INTO t SELECT g, 'w' || g FROM generate_series(1, 5000) g");
is(run("SELECT reltuples::bigint FROM pg_class WHERE oid = 't'::regclass"),
	1000, 'the GUC off keeps the statistics stale');

$coordinator->stop;
done_testing();
