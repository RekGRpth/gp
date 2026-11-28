# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A level of a multiway join whose outer joins mix kinds rides on one
# segment when one of its tables is a derived one: the tables of the
# derived input come whole to the segment beside the rest, and the
# subquery, the joins and the sublinks of the level replay the way they
# ran on the coordinator

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub local_run { return $coordinator->safe_psql('postgres', shift =~ s/\bt(\d)\b/l$1/gr); }

run(<<'DDL');
CREATE TABLE t1 (a int, b text);
CREATE TABLE t2 (a int, c text);
CREATE TABLE t3 (a int, d text);
SELECT gp.distribute('t1', ARRAY['a']);
SELECT gp.distribute('t2', ARRAY['a']);
SELECT gp.distribute('t3', ARRAY['a']);
INSERT INTO t1 SELECT g % 7, 'x' || g FROM generate_series(1, 30) gs(g);
INSERT INTO t2 SELECT g % 5, 'y' || g FROM generate_series(1, 25) gs(g);
INSERT INTO t3 SELECT g % 4, 'z' || g FROM generate_series(1, 20) gs(g);
CREATE TABLE l1 AS SELECT * FROM t1;
CREATE TABLE l2 AS SELECT * FROM t2;
CREATE TABLE l3 AS SELECT * FROM t3;
DDL

# A derived table beside the outer joins of the level
my $q = 'SELECT count(*) FROM (SELECT a, b FROM t1 WHERE a > 0) s1 '
	. 'LEFT JOIN t2 USING (a) FULL JOIN t3 USING (a)';
like(plan_for($q), qr/GpSlice/, 'a mixed level with a derived table rides');
is(run($q), local_run($q), 'and answers the truth of the whole');

# The tables of two derived inputs come whole beside the rest
$q = 'SELECT count(*) FROM (SELECT a, b FROM t1) s1 '
	. 'FULL JOIN (SELECT a, c FROM t2) s2 USING (a) FULL JOIN t3 USING (a)';
like(plan_for($q), qr/GpSlice/, 'two derived tables ride the level');
is(run($q), local_run($q), 'and answer the truth of the whole');

# A RIGHT join beside a LEFT join, the leftmost a derived table
$q = "SELECT count(*) FROM (SELECT a, b FROM t1 WHERE b LIKE 'x%') s1 "
	. 'RIGHT JOIN t3 USING (a) LEFT JOIN t2 USING (a)';
like(plan_for($q), qr/GpSlice/, 'a derived leftmost rides a right-join level');
is(run($q), local_run($q), 'and answers the same');

# A NOT EXISTS against the column of the derived input
$q = 'SELECT count(*) FROM (SELECT a, b FROM t1 WHERE a > 1) s1 '
	. 'LEFT JOIN t2 USING (a) FULL JOIN t3 USING (a) '
	. 'WHERE NOT EXISTS (SELECT 1 FROM t2 x WHERE x.a = s1.a)';
like(plan_for($q), qr/GpSlice/, 'a NOT EXISTS rides the derived level');
is(run($q), local_run($q), 'and answers the same');

# A grouped aggregate over the level
$q = 'SELECT s1.b, count(*) FROM (SELECT a, b FROM t1) s1 '
	. 'LEFT JOIN t2 USING (a) FULL JOIN t3 USING (a) GROUP BY 1 ORDER BY 1';
like(plan_for($q), qr/GpSlice/, 'a grouped aggregate rides the derived level');
is(run($q), local_run($q), 'and answers the groups of the whole');

# The sublinks of the level ride with the derived table
$q = 'SELECT count(*) FROM (SELECT a, b FROM t1 WHERE a > 0) s1 '
	. 'LEFT JOIN t2 USING (a) FULL JOIN t3 USING (a) '
	. 'WHERE EXISTS (SELECT 1 FROM t2 x WHERE x.a = t3.a)';
like(plan_for($q), qr/GpSlice/, 'an EXISTS rides the derived level');
is(run($q), local_run($q), 'and answers the same');

# A select list without an aggregate
$q = 'SELECT s1.b, t2.c, t3.d FROM (SELECT a, b FROM t1 WHERE a > 0) s1 '
	. 'LEFT JOIN t2 USING (a) FULL JOIN t3 USING (a) ORDER BY 1, 2, 3';
like(plan_for($q), qr/GpSlice/, 'a select list rides the derived level');
is(run($q), local_run($q), 'and answers the rows of the whole');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
