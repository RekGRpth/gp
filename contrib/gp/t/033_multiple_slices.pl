# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A cross join of two colocated tables runs as a slice with a broadcast
# pull over libpq sessions, and a read query's fragment must not write
# even through an immutable function: the dispatched fragment session
# runs read-only.  The deep live-orchestration scenario (parked readers
# across three dispatched slice sessions, cancellation through their
# wakeups) is tracked separately: it needs a live-debug session on the
# hung query, the wake choreography of the interconnect workers does not
# transfer to the libpq pulls as written.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

run('CREATE TABLE t1 (a int, b text)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('CREATE TABLE t2 (a int, c text)');
run("SELECT gp.distribute('t2', ARRAY['a'])");
run("INSERT INTO t1 VALUES (1, 'root-old')");
run("INSERT INTO t2 VALUES (1, 'peer-old')");
like($coordinator->safe_psql('postgres',
	'EXPLAIN (COSTS OFF) SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2'),
	qr/GpSlice/, 'a cross join runs as a slice with a broadcast motion');

# A read query's fragment must not write even through an immutable
# function: the dispatched fragment session runs read-only
run('CREATE TABLE audit (a int)');
run("SELECT gp.distribute('audit', ARRAY['a'])");
run(<<'SQL');
CREATE FUNCTION gp_actual_write(a int) RETURNS int LANGUAGE plpgsql VOLATILE AS
$$BEGIN INSERT INTO audit VALUES (a); RETURN a; END$$
SQL
run(<<'SQL');
CREATE FUNCTION gp_test_write(a int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS
$$BEGIN RETURN gp_actual_write(a); END$$
SQL
my $check = $coordinator->background_psql('postgres', on_error_stop => 0);
$check->query('SELECT gp_test_write(t1.a) FROM ONLY t1 CROSS JOIN ONLY t2');
like($check->{stderr}, qr/read-only transaction/,
	'a fragment cannot write through an immutable function');
$check->{stderr} = '';
is(run('SELECT count(*) FROM audit'), 0, 'the refused fragment wrote nothing');
$check->quit;

# The plain cross join answers the same as on the coordinator's local
# copy: the broadcast pull feeds the join on every dispatched fragment
is(run('SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2'),
	'root-old|peer-old', 'the cross join answers over the pulled fragment');

# Reading first must not turn a later write of the same transaction READ ONLY,
# and the write must remain visible through the ordinary dispatched connection
my $check2 = $coordinator->background_psql('postgres', on_error_stop => 0);
$check2->query_safe('BEGIN');
is($check2->query_safe('SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2'),
	'root-old|peer-old', 'a transaction takes a consistent snapshot');
$check2->query_safe("INSERT INTO t1 VALUES (2, 'written')");
is($check2->query_safe('SELECT b FROM t1 WHERE a = 2'), 'written',
	'a read followed by a write sees its own write');
$check2->query_safe('ROLLBACK');
$check2->quit;

# A literal limit of an unordered query rides into the SQL of the slice:
# every dispatched session cuts its own share, and the limit of the plan
# cuts the union on the coordinator, so the answers stay the ones of a
# single node.  A sorted query, a compound one and a limit that is not a
# literal keep their limit above the slice
run("INSERT INTO t1 VALUES (2, 'root-2'), (3, 'root-3')");
run("INSERT INTO t2 VALUES (2, 'peer-2'), (3, 'peer-3')");
my $cross = 'SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2';
like($coordinator->safe_psql('postgres',
		"EXPLAIN (VERBOSE, COSTS OFF) $cross LIMIT 3"),
	qr/LIMIT 3/, 'the literal limit rides in the SQL of the slice');
like($coordinator->safe_psql('postgres',
		"EXPLAIN (VERBOSE, COSTS OFF) $cross LIMIT 3 OFFSET 1"),
	qr/LIMIT 4/, 'the offset rides added to the limit');
unlike($coordinator->safe_psql('postgres',
		"EXPLAIN (VERBOSE, COSTS OFF) $cross ORDER BY 1, 2 LIMIT 3"),
	qr/LIMIT 3/, 'a sorted query keeps its limit above the slice');
unlike($coordinator->safe_psql('postgres',
		"EXPLAIN (VERBOSE, COSTS OFF) $cross LIMIT (SELECT 3)"), qr/LIMIT 3/,
	'a limit that is not a literal keeps the whole answers');
is(run("SELECT count(*) FROM ($cross LIMIT 3) s"), 3,
	'the segments answer a cut share and the coordinator cuts again');
is(run("SELECT count(*) FROM ($cross LIMIT 3 OFFSET 1) s"), 3,
	'the offset skips its rows on the coordinator');
is(run("SELECT count(*) FROM ($cross LIMIT 0) s"), 0,
	'a zero limit answers nothing');
is(run("SELECT count(*) FROM ($cross LIMIT 100 OFFSET 9) s"), 0,
	'an offset past the answer answers nothing');
is(run("SELECT count(*) FROM ($cross LIMIT 9223372036854775807 OFFSET 2) s"),
	7, 'an offset by the largest limit answers the rest');
is(run("SELECT count(*) FROM (SELECT b FROM ONLY t1 UNION SELECT c FROM ONLY t2 LIMIT 3) s"),
	3, 'a compound query cuts the union of its arms');
is(run("SELECT b || '|' || c FROM ONLY t1 CROSS JOIN ONLY t2 WHERE b = 'root-2' LIMIT 1"),
	'root-2|peer-2', 'the filter rides before the limit of the fragment');
is(run("SELECT b, c FROM ($cross ORDER BY 1, 2 LIMIT 3) s"),
	"root-2|peer-2\nroot-2|peer-3\nroot-2|peer-old",
	'a sorted query answers the first rows of its order');

$_->stop foreach (@segments, $coordinator);

done_testing();
