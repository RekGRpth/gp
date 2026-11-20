# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# DISTINCT and DISTINCT ON are local when their whole distribution key is in
# the output: equal rows are on one segment, so no global dedup is needed

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub local_query
{
	my ($query) = @_;

	$query =~ s/\bt\b/l/g;
	$query =~ s/\bm\b/lm/g;
	$query =~ s/\br\b/lr/g;
	return $query;
}

run('CREATE TABLE t (a int, b text)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t VALUES (1, 'x'), (1, 'x'), (1, 'y'), (2, 'x'),
	(2, 'y'), (3, 'x'), (3, 'x')");
run('CREATE TABLE m (a int, b int)');
run("SELECT gp.distribute('m', ARRAY['a', 'b'])");
run("INSERT INTO m VALUES (1, 2), (1, 2), (1, 3), (2, 1), (2, 1), (2, 2)");
run('CREATE TABLE r (a int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (10), (10), (20), (30), (30)');
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lm AS SELECT * FROM m');
run('CREATE TABLE lr AS SELECT * FROM r');

like(plan_for('SELECT DISTINCT a FROM t'), qr/GpSlice/,
	'DISTINCT by the distribution key is local');
unlike(plan_for('SELECT DISTINCT a FROM t'), qr/GpMotion/,
	'a local DISTINCT needs no motion');
like(plan_for('SELECT DISTINCT a, b FROM t'), qr/GpSlice/,
	'DISTINCT by all output columns is local');
like(plan_for('SELECT DISTINCT ON (a) a, b FROM t ORDER BY a, b'),
	qr/GpSlice/, 'DISTINCT ON keeps the distribution key in the output');
unlike(plan_for('SELECT DISTINCT b FROM t'), qr/GpSlice/,
	'another key stays on the coordinator');
like(plan_for('SELECT DISTINCT a, b FROM m'), qr/GpSlice/,
	'a compound distribution key is distinct locally');
unlike(plan_for('SELECT DISTINCT a FROM m'), qr/GpSlice/,
	'a prefix of a compound key stays on the coordinator');
like(plan_for('SELECT DISTINCT a FROM r'), qr/GpSlice/,
	'a replicated DISTINCT uses one segment');
like(plan_for('SELECT DISTINCT a, b FROM t ORDER BY a, b LIMIT 2'),
	qr/GpSlice/, 'sort and limit stay above DISTINCT');

my @queries = (
	'SELECT DISTINCT a FROM t ORDER BY a',
	'SELECT DISTINCT a, b FROM t ORDER BY a, b',
	'SELECT DISTINCT ON (a) a, b FROM t ORDER BY a, b',
	'SELECT DISTINCT b FROM t ORDER BY b',
	'SELECT DISTINCT a, b FROM m ORDER BY a, b',
	'SELECT DISTINCT a FROM m ORDER BY a',
	'SELECT DISTINCT a FROM r ORDER BY a',
	'SELECT DISTINCT a, b FROM t ORDER BY a, b LIMIT 2',
);
foreach my $query (@queries)
{
	is(run($query), run(local_query($query)),
		"the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
