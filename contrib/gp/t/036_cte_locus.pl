# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A nonrecursive CTE has a locus of its own through PlannedStmt.subplans.
# Projection, a condition, sort and limit above CteScan preserve it.  An
# aggregate CTE runs on the segments even when the outer query keeps its
# join, its sublinks and its aggregates on the coordinator

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

my @segments;
foreach my $segment_id (1 .. 3)
{
	my $node = PostgreSQL::Test::Cluster->new("seg$segment_id");
	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
max_prepared_transactions = 10
");
	$node->start;
	push @segments, $node;
}
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 3 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub local_query
{
	my ($query) = @_;

	$query =~ s/\bt\b/l/g;
	$query =~ s/\br\b/lr/g;
	$query =~ s/\bu\b/lu/g;
	return $query;
}

run('CREATE TABLE t (a int, b text, v numeric)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t VALUES (1, 'x', 1), (1, 'y', 2), (2, 'x', 3), (2, 'y', 4)");
run('CREATE TABLE r (a int)');
run("SELECT gp.distribute('r', NULL)");
run('INSERT INTO r VALUES (10), (20)');
run('CREATE TABLE u (b text, n int)');
run("SELECT gp.distribute('u', ARRAY['b'])");
run("INSERT INTO u VALUES ('x', 1), ('y', 2)");
run('CREATE TABLE l AS SELECT * FROM t');
run('CREATE TABLE lr AS SELECT * FROM r');
run('CREATE TABLE lu AS SELECT * FROM u');

like(plan_for('WITH s AS MATERIALIZED (SELECT a, b, v FROM t) SELECT a, b, v FROM s'),
	qr/GpSlice/, 'a nonrecursive CTE runs on the segments');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT a, b, v FROM t) SELECT a, b, v FROM s'),
	qr/GpMotion/, 'a CTE needs no motion');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s WHERE a > 1'),
	qr/GpSlice/, 'a condition above CteScan keeps its locus');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a + 1 FROM s ORDER BY 1'),
	qr/GpSlice/, 'a projection above CteScan keeps its locus');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s ORDER BY a LIMIT 2'),
	qr/GpSlice/, 'sort and limit stay above the CTE');
like(plan_for('WITH s AS MATERIALIZED (SELECT a FROM r) SELECT a FROM s ORDER BY a'),
	qr/GpSlice/, 'a replicated CTE uses one segment');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t) SELECT s.a, t.a FROM s, t'),
	qr/GpSlice/, 'a join of the outer query stays on the coordinator');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT a FROM t), q AS (SELECT a FROM s) SELECT a FROM q'),
	qr/GpSlice/, 'two CTEs stay on the coordinator');
unlike(plan_for('WITH RECURSIVE s(a) AS (SELECT 1 UNION ALL SELECT a + 1 FROM s WHERE a < 3) SELECT a FROM s'),
	qr/GpSlice/, 'a recursive CTE stays on the coordinator');

# An aggregate CTE runs on the segments as a subplan of its own, and the
# join, the sublinks and the aggregates of the outer query stay above it
like(plan_for('WITH s AS MATERIALIZED (SELECT b, count(*) AS c FROM t GROUP BY b) SELECT * FROM s'),
	qr/GpSlice/, 'an aggregate CTE runs on the segments');
like(plan_for('WITH s AS MATERIALIZED (SELECT a, count(*) AS c FROM t GROUP BY a) SELECT * FROM s'),
	qr/GpSlice/, 'an aggregate CTE grouped by the key runs on the segments');
like(plan_for('WITH s AS MATERIALIZED (SELECT a, count(*) AS c FROM r GROUP BY a) SELECT * FROM s'),
	qr/GpSlice/, 'an aggregate CTE of a replicated table runs on one segment');
like(plan_for("WITH s AS MATERIALIZED (SELECT b, sum(v) AS total FROM t GROUP BY b)
	SELECT s.b, s.total FROM s, t WHERE s.b = t.b
	  AND s.total = (SELECT max(total) FROM s)"),
	qr/GpSlice/, 'a join and a sublink of the outer query keep the aggregate CTE on the segments');
like(plan_for('WITH s AS MATERIALIZED (SELECT b, count(*) AS c FROM t GROUP BY b) SELECT count(*) FROM s'),
	qr/GpSlice/, 'an aggregate above the CTE keeps the aggregate CTE on the segments');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS total FROM t GROUP BY b ORDER BY total DESC) SELECT * FROM s'),
	qr/GpSlice/, 'an ORDER BY inside the CTE stays on the coordinator');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT b, count(*) AS c FROM t GROUP BY b), q AS MATERIALIZED (SELECT * FROM s) SELECT * FROM q'),
	qr/GpSlice/, 'an aggregate CTE under another CTE stays on the coordinator');

# The columns of the replaced subplan deparse against the whole statement
like($coordinator->safe_psql('postgres',
	'EXPLAIN (VERBOSE, COSTS OFF) WITH s AS MATERIALIZED
		(SELECT b AS bb, sum(v) AS total FROM t GROUP BY bb)
		SELECT * FROM s'),
	qr/Output: s\.bb, s\.total/,
	'the columns of the aggregate CTE deparse against the statement');

# The join of an aggregate CTE with a table runs on the segments when every
# grouping column of the CTE is equated with a column of the table
like(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t FROM s, t WHERE s.b = t.b'),
	qr/GpSlice/, 'the join of an aggregate CTE runs on the segments');
unlike(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t FROM s, t WHERE s.b = t.b'),
	qr/CTE Scan/, 'the join of an aggregate CTE needs no scan of the CTE');
like(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM s LEFT JOIN t ON s.b = t.b'),
	qr/GpSlice/, 'a left join with the CTE kept runs on the segments');
like(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM t LEFT JOIN s ON s.b = t.b'),
	qr/GpSlice/, 'a left join with the table kept runs on the segments');
like(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, u.n FROM s LEFT JOIN u ON s.b = u.b'),
	qr/GpSlice/, 'the CTE meets a table keyed by its grouping column where it stands');
like(plan_for('WITH s AS MATERIALIZED (SELECT b, count(DISTINCT a) AS c FROM t GROUP BY b) SELECT s.b, s.c FROM s, t WHERE s.b = t.b'),
	qr/GpSlice/, 'a distinct aggregate in the CTE joins when the groups are hashed');
like(plan_for('WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t FROM s FULL JOIN t ON s.b = t.b'),
	qr/CTE Scan/, 'a full join of the CTE keeps the scan of the CTE');

# The join of an aggregate CTE with the answer of its own aggregate above
# (an InitPlan) runs on the segments: the filter of the InitPlan carries
# onto the replacement, whose share of the CTE knows nothing of the whole
like(plan_for("WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b)
	SELECT s.b, s.t FROM s, t WHERE s.b = t.b AND s.t = (SELECT max(t) FROM s)"),
	qr/GpSlice/, 'the join with the aggregate of the CTE runs on the segments');
unlike(plan_for("WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b)
	SELECT s.b, s.t FROM s, t WHERE s.b = t.b AND s.t = (SELECT max(t) FROM s)"),
	qr/Join/, 'the join with the aggregate of the CTE needs no join above');
like(plan_for("WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b)
	SELECT s.b, s.t, u.n FROM s LEFT JOIN u ON s.b = u.b WHERE s.t = (SELECT max(t) FROM s)"),
	qr/GpSlice/, 'the join with the aggregate keeps the table keyed by the grouping');
like(plan_for("WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b)
	SELECT s.b, s.t, t.a FROM s JOIN t ON s.b = t.b AND s.t = (SELECT max(t) FROM s)"),
	qr/Join/, 'a sublink in the condition of the join keeps the join above');
like(plan_for("WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b)
	SELECT s.b, s.t, u.n FROM s LEFT JOIN u ON s.b = u.b
	  AND s.t = (SELECT max(t) FROM s)"),
	qr/Join/, 'a sublink in the condition of an outer join keeps the join above');

my @queries = (
	'WITH s AS MATERIALIZED (SELECT a, b, v FROM t) SELECT a, b, v FROM s ORDER BY a, b',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s WHERE a > 1 ORDER BY a',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a + 1 FROM s ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT a FROM s ORDER BY a LIMIT 2',
	'WITH s AS MATERIALIZED (SELECT a FROM r) SELECT a FROM s ORDER BY a',
	'WITH s AS MATERIALIZED (SELECT a FROM t) SELECT s.a, t.a FROM s, t ORDER BY s.a, t.a',
	'WITH s AS MATERIALIZED (SELECT b, count(*) AS c FROM t GROUP BY b) SELECT * FROM s ORDER BY b',
	'WITH s AS MATERIALIZED (SELECT a, count(*) AS c FROM t GROUP BY a) SELECT * FROM s ORDER BY a',
	'WITH s AS MATERIALIZED (SELECT b, v, sum(v) AS total FROM t GROUP BY b, v) SELECT * FROM s ORDER BY b, v',
	'WITH s AS MATERIALIZED (SELECT a, b, sum(v) AS total FROM t GROUP BY a, b) SELECT * FROM s ORDER BY a, b',
	'WITH s AS MATERIALIZED (SELECT a, count(*) AS c FROM r GROUP BY a) SELECT * FROM s ORDER BY a',
	'WITH s AS MATERIALIZED (SELECT b, count(*) AS c FROM t GROUP BY b) SELECT count(*) FROM s',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS total FROM t GROUP BY b)
		SELECT s.b, s.total FROM s, t WHERE s.b = t.b
		  AND s.total = (SELECT max(total) FROM s) ORDER BY s.b',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t FROM s, t WHERE s.b = t.b ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM s LEFT JOIN t ON s.b = t.b ORDER BY 1, 3',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM t LEFT JOIN s ON s.b = t.b ORDER BY 1, 3',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM s RIGHT JOIN t ON s.b = t.b ORDER BY 1, 3',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, u.n FROM s LEFT JOIN u ON s.b = u.b ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, count(DISTINCT a) AS c FROM t GROUP BY b) SELECT s.b, s.c FROM s, t WHERE s.b = t.b ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t FROM s FULL JOIN t ON s.b = t.b ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t FROM s, t WHERE s.b = t.b AND s.t = (SELECT max(t) FROM s) ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM s, t WHERE s.b = t.b AND s.t > (SELECT min(t) FROM s) ORDER BY 1, 3',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, u.n FROM s LEFT JOIN u ON s.b = u.b WHERE s.t = (SELECT max(t) FROM s) ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, t.a FROM s JOIN t ON s.b = t.b AND s.t = (SELECT max(t) FROM s) ORDER BY 1',
	'WITH s AS MATERIALIZED (SELECT b, sum(v) AS t FROM t GROUP BY b) SELECT s.b, s.t, u.n FROM s LEFT JOIN u ON s.b = u.b AND s.t = (SELECT max(t) FROM s) ORDER BY 1',
);
foreach my $query (@queries)
{
	is(run($query), run(local_query($query)),
		"the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
