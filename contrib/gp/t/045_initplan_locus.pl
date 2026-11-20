# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# An init plan that aggregates over the gather of one distributed table is
# answered by the segments with partial columns: every segment answers a
# partial sum and a partial count of the average, and the final aggregate
# of the init plan reads them.  The subselect is collected before the
# planner runs, in the derived subqueries of the range table as well, and
# every init plan must find its own candidate

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE t (a int, b numeric, c text, f float8)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run('CREATE TABLE u (a int)');
run("SELECT gp.distribute('u', ARRAY['a'])");
run('CREATE TABLE rep (b numeric)');
run("SELECT gp.distribute('rep', NULL)");
run("INSERT INTO t SELECT g % 10, g * 1.5, 'x' || g, g * 0.25 FROM generate_series(1, 100) g");
run('INSERT INTO u SELECT g FROM generate_series(1, 60) g');
run('INSERT INTO rep SELECT g * 1.5 FROM generate_series(1, 100) g');
# The same rows in a table of the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lu AS SELECT * FROM u');

# The rewrite of the init plans
like(plan_for('SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'an init plan over the average of one table is answered partly');
like(plan_for('SELECT count(*) FROM t WHERE a > (SELECT avg(a) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'an average of a whole number divides in numeric on the coordinator');
like(plan_for('SELECT count(*) FROM t WHERE f > (SELECT avg(f) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'an average of a float divides on the coordinator');
like(plan_for('SELECT count(*) FROM t WHERE c > (SELECT min(c) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'an init plan over the minimum of one table is answered partly');
like(plan_for('SELECT count(*) FROM t WHERE b > (SELECT max(b) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'an init plan over the maximum of one table is answered partly');
like(plan_for('SELECT count(*) FROM t WHERE b > (SELECT sum(b) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'an init plan over the sum of one table is answered partly');
like(plan_for("SELECT cntrycode, count(*) FROM (
	SELECT 'c' || (a % 3)::text AS cntrycode, b AS bal
	FROM t WHERE b > (SELECT avg(b) FROM t WHERE b > 0)
) custsale GROUP BY cntrycode ORDER BY cntrycode"),
	qr/InitPlan.*GpSlice/s,
	'an init plan inside a derived subquery of the range table is found');
like(plan_for('SELECT count(*) FROM t WHERE a > (SELECT avg(a) FROM u)'),
	qr/InitPlan.*GpSlice/s,
	'an init plan over another table is answered partly');

# The queries that keep their init plans with the coordinator
unlike(plan_for('SELECT count(*) FROM t s WHERE s.b > (SELECT avg(s2.b) FROM t s2 WHERE s2.a = s.a)'),
	qr/InitPlan.*GpSlice/s,
	'a correlated subselect is not an init plan of the segments');
unlike(plan_for('SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM rep)'),
	qr/InitPlan.*GpSlice/s,
	'a replicated table is answered by one segment already');
unlike(plan_for('SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM l)'),
	qr/InitPlan.*GpSlice/s,
	'a table of the coordinator has no gather to answer');
unlike(plan_for('SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM t) AND c > (SELECT min(c) FROM t)'),
	qr/InitPlan.*GpSlice/s,
	'two init plans over one table share the one candidate, so both stay');

my @queries = (
	'SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM t)',
	'SELECT count(*) FROM t WHERE a > (SELECT avg(a) FROM t)',
	'SELECT count(*) FROM t WHERE f > (SELECT avg(f) FROM t)',
	'SELECT count(*) FROM t WHERE c > (SELECT min(c) FROM t)',
	'SELECT count(*) FROM t WHERE c < (SELECT max(c) FROM t)',
	'SELECT count(*) FROM t WHERE b > (SELECT sum(b) FROM t)',
	'SELECT count(*) FROM t WHERE a > (SELECT avg(a) FROM u)',
	'SELECT count(*) FROM t WHERE a > (SELECT avg(a) FROM t WHERE a > 4)',
	"SELECT cntrycode, count(*), sum(bal) FROM (
		SELECT 'c' || (a % 3)::text AS cntrycode, b AS bal
		FROM t WHERE b > (SELECT avg(b) FROM t WHERE b > 0)
	) custsale GROUP BY cntrycode ORDER BY cntrycode",
	'SELECT count(*) FROM t s WHERE s.b > (SELECT avg(s2.b) FROM t s2 WHERE s2.a = s.a)',
	'SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM rep)',
	'SELECT count(*) FROM t WHERE b > (SELECT avg(b) FROM t) AND c > (SELECT min(c) FROM t)',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt\b/l/g;
	$local =~ s/\bu\b/lu/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
