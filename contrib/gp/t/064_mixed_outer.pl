# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A level of a multiway join whose outer joins mix kinds -- a FULL join
# beside a LEFT join, a FULL join over an inner join, a RIGHT join beside
# them -- keeps the rows of no one table, so the smart rules of a chain do
# not answer it.  It still answers on the segments: the slice takes every
# table whole to one segment and replays the joins of the level the way
# the coordinator would, and the answer stays the one of the whole

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

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

# A FULL join over an inner join: the flat path of the leftmost table
# breaks on the FULL
like(plan_for('SELECT count(*) FROM t1 JOIN t2 USING (a) ' .
			  'FULL JOIN t3 USING (a)'), qr/GpSlice/,
	'a full join over an inner join rides the segments');
is(run('SELECT count(*) FROM t1 JOIN t2 USING (a) FULL JOIN t3 USING (a)'),
	run('SELECT count(*) FROM l1 JOIN l2 USING (a) FULL JOIN l3 USING (a)'),
	'and its answer is the one of the whole');

# A FULL join beside a LEFT join
is(run('SELECT count(*) FROM t1 LEFT JOIN t2 USING (a) ' .
	   'FULL JOIN t3 USING (a)'),
	run('SELECT count(*) FROM l1 LEFT JOIN l2 USING (a) ' .
		'FULL JOIN l3 USING (a)'),
	'a full join beside a left join answers the same');
is(run("SELECT string_agg(c || d, ',' ORDER BY c, d) FROM t1 " .
	   'LEFT JOIN t2 USING (a) FULL JOIN t3 USING (a)'),
	run("SELECT string_agg(c || d, ',' ORDER BY c, d) FROM l1 " .
		'LEFT JOIN l2 USING (a) FULL JOIN l3 USING (a)'),
	'the rows of the mixed level are the ones of the whole');

# A RIGHT join beside others of the level
is(run('SELECT count(*) FROM t1 JOIN t2 USING (a) ' .
	   'RIGHT JOIN t3 USING (a)'),
	run('SELECT count(*) FROM l1 JOIN l2 USING (a) ' .
		'RIGHT JOIN l3 USING (a)'),
	'a right join beside an inner join answers the same');
is(run('SELECT count(*) FROM t1 LEFT JOIN t2 USING (a) ' .
	   'RIGHT JOIN t3 USING (a)'),
	run('SELECT count(*) FROM l1 LEFT JOIN l2 USING (a) ' .
		'RIGHT JOIN l3 USING (a)'),
	'a right join beside a left join answers the same');
is(run('SELECT sum(a1) FROM (SELECT t1.a AS a1, t3.a AS a3 ' .
	   'FROM t1 LEFT JOIN t2 USING (a) RIGHT JOIN t3 ON t1.a = t3.a) s'),
	run('SELECT sum(a1) FROM (SELECT l1.a AS a1, l3.a AS a3 ' .
	   'FROM l1 LEFT JOIN l2 USING (a) RIGHT JOIN l3 ON l1.a = l3.a) s'),
	'the kept rows of a right join are the ones of the right side');

# The aggregates above the mixed level read the same groups
is(run('SELECT count(*), sum(n) FROM (SELECT t1.a, count(*) AS n ' .
	   'FROM t1 JOIN t2 USING (a) FULL JOIN t3 USING (a) GROUP BY t1.a) s'),
	run('SELECT count(*), sum(n) FROM (SELECT l1.a, count(*) AS n ' .
	   'FROM l1 JOIN l2 USING (a) FULL JOIN l3 USING (a) GROUP BY l1.a) s'),
	'an aggregate above the mixed level adds up the same');

# The chains of one kind keep their smart answers: a LEFT chain keeps its
# first table on the segments, a FULL chain answers on one segment
like(plan_for('SELECT count(*) FROM t1 LEFT JOIN t2 USING (a) ' .
			  'LEFT JOIN t3 USING (a)'), qr/GpSlice/,
	'a chain of left joins rides as it did');
is(run('SELECT count(*) FROM t1 LEFT JOIN t2 USING (a) ' .
	   'LEFT JOIN t3 USING (a)'),
	run('SELECT count(*) FROM l1 LEFT JOIN l2 USING (a) ' .
		'LEFT JOIN l3 USING (a)'),
	'and answers the same as before');

$coordinator->stop;
done_testing();
