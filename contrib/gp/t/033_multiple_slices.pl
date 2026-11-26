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

$_->stop foreach (@segments, $coordinator);

done_testing();
