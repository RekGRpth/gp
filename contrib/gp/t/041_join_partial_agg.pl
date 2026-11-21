# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A distributed join can feed a partial aggregate: every segment aggregates
# its own join rows, and the coordinator combines them by the final grouping

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(same_answers start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE t1 (a int, g int, v numeric)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('INSERT INTO t1 SELECT i, i % 4, i * 1.5 FROM generate_series(1, 100) i');
run('CREATE TABLE t2 (a int, g int, v numeric)');
run("SELECT gp.distribute('t2', ARRAY['a'], colocate_with => 't1')");
run('INSERT INTO t2 SELECT i, i % 5, i * 2.0 FROM generate_series(41, 120) i');
run('CREATE TABLE l1 AS SELECT * FROM t1');
run('CREATE TABLE l2 AS SELECT * FROM t2');

like(plan_for('SELECT sum(t1.v + t2.v) FROM t1 JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'a global sum over a colocated join is partial');
like(plan_for('SELECT t1.g, sum(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g'),
	qr/GpSlice/, 'a join can be grouped by a non-distribution key');
like(plan_for('SELECT 100 * sum(CASE WHEN t1.g = 0 THEN t2.v ELSE 0 END) / sum(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'the final expression combines two partial sums');
like(plan_for('SELECT t1.g, min(t2.v), max(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g HAVING sum(t2.v) > 500'),
	qr/GpSlice/, 'the final aggregate keeps HAVING on the coordinator');
like(plan_for('SELECT sum(t2.v) FROM t1 JOIN t2 ON t1.a + 0 = t2.a'),
	qr/GpSlice/, 'a broadcast join feeds a partial aggregate');
like(plan_for('SELECT t2.g, sum(t2.v) FROM t1 LEFT JOIN t2 ON t1.a + 0 = t2.a GROUP BY t2.g'),
	qr/GpSlice/, 'final grouping merges null groups of an outer join');
like(plan_for('SELECT count(*) FROM t1 JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'count combines by a sum of the partial counts');
unlike(plan_for('SELECT avg(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'average does not combine by dividing partial sums');
like(plan_for('SELECT sum(t1.g) FROM t1 JOIN t2 ON t1.a = t2.a'),
	qr/GpSlice/, 'sum with another result type combines in its result type');
like(plan_for('SELECT t1.g, count(*) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g'),
	qr/GpSlice/, 'a grouped count uses redistribution instead of a partial count');
like(plan_for('SELECT t1.g, avg(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g'),
	qr/GpSlice/, 'a grouped average uses redistribution instead of a partial average');
like(plan_for('SELECT t1.g, sum(t1.g) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g'),
	qr/GpSlice/, 'a grouped sum can change result type after redistribution');
like(plan_for('SELECT t1.g, count(*) FROM t1 JOIN t2 ON t1.a + 0 = t2.a GROUP BY t1.g'),
	qr/GpSlice/, 'redistribution can follow a broadcast join');

run('SET enable_mergejoin = off');
my @queries = (
	'SELECT sum(t1.v + t2.v) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT t1.g, sum(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g ORDER BY t1.g',
	'SELECT 100 * sum(CASE WHEN t1.g = 0 THEN t2.v ELSE 0 END) / sum(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT t1.g, min(t2.v), max(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g HAVING sum(t2.v) > 500 ORDER BY t1.g',
	'SELECT sum(t2.v) FROM t1 JOIN t2 ON t1.a + 0 = t2.a',
	'SELECT t1.g, sum(t2.v) FROM t1 JOIN t2 ON t1.a + 0 = t2.a GROUP BY t1.g ORDER BY t1.g',
	'SELECT t2.g, sum(t2.v) FROM t1 LEFT JOIN t2 ON t1.a + 0 = t2.a GROUP BY t2.g ORDER BY t2.g',
	'SELECT t1.g, sum(t2.v) FROM t1 LEFT JOIN t2 ON t1.a = t2.a GROUP BY t1.g ORDER BY t1.g LIMIT 2',
	'SELECT count(*) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT avg(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT sum(t1.g) FROM t1 JOIN t2 ON t1.a = t2.a',
	'SELECT t1.g, count(*) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g ORDER BY t1.g',
	'SELECT t1.g, avg(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g ORDER BY t1.g',
	'SELECT t1.g, sum(t1.g) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g ORDER BY t1.g',
	'SELECT t1.g, count(*) FROM t1 JOIN t2 ON t1.a + 0 = t2.a GROUP BY t1.g ORDER BY t1.g',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt1\b/l1/g;
	$local =~ s/\bt2\b/l2/g;
	same_answers($coordinator, \@segments, $query, $local,
		"the same answer as on the coordinator: $query");
}

# If the original aggregate supplied the order without a Sort node, the final
# aggregate over partial groups gets a new Sort on the coordinator
run('SET enable_sort = off');
my $sorted = 'SELECT t1.g, sum(t2.v) FROM t1 JOIN t2 ON t1.a = t2.a GROUP BY t1.g ORDER BY t1.g';
like(plan_for($sorted), qr/Sort.*GpSlice/s,
	'a partial grouped join gets a final Sort when the planner omitted it');
same_answers($coordinator, \@segments, $sorted,
	'SELECT l1.g, sum(l2.v) FROM l1 JOIN l2 ON l1.a = l2.a GROUP BY l1.g ORDER BY l1.g',
	'the added final Sort answers ORDER BY');

$_->stop foreach (@segments, $coordinator);

done_testing();
