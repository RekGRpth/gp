# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A level of a multiway join whose outer joins mix kinds -- a FULL join
# beside a LEFT join, a RIGHT join beside them -- answers on one segment,
# where every table comes whole: the joins of the level replay the way
# they ran on the coordinator, and a sublink over the tables of the
# level rides the same way -- its tables come whole beside the rest, and
# the sublink filters or hangs a value on the rows of the level as it
# would here

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

# The local truth of every query, computed on the copies
sub local_run { return $coordinator->safe_psql('postgres', shift =~ s/\bt(\d)\b/l$1/gr); }

# A FULL join beside a LEFT join, with an EXISTS sublink over the level
my $q = 'SELECT count(*) FROM t1 LEFT JOIN t2 USING (a) ' .
	'FULL JOIN t3 USING (a) WHERE EXISTS (SELECT 1 FROM t2 x WHERE x.a = t3.a)';
like(plan_for($q), qr/GpSlice/, 'a mixed level with a sublink rides');
is(run($q), local_run($q), 'and answers the truth of the whole');

# An IN sublink beside the mixed outer joins
like(plan_for("SELECT count(*) FROM t1 JOIN t2 USING (a) FULL JOIN t3 "
	. "USING (a) WHERE t1.a IN (SELECT a FROM t3 WHERE d LIKE 'z%')"),
	qr/GpSlice/, 'an IN sublink rides the mixed level too');
is(run("SELECT count(*) FROM t1 JOIN t2 USING (a) FULL JOIN t3 "
	  . "USING (a) WHERE t1.a IN (SELECT a FROM t3 WHERE d LIKE 'z%')"),
	run('SELECT count(*) FROM l1 JOIN l2 USING (a) FULL JOIN l3 '
		. "USING (a) WHERE l1.a IN (SELECT a FROM l3 WHERE d LIKE 'z%')"),
	'and answers the same');

# A NOT EXISTS sublink over a RIGHT join beside a LEFT join; the link
# of the sublink is the key of the distribution
$q = 'SELECT count(*) FROM t1 LEFT JOIN t2 USING (a) ' .
	'RIGHT JOIN t3 USING (a) WHERE NOT EXISTS (SELECT 1 FROM t2 x WHERE x.a = t3.a)';
like(plan_for($q), qr/GpSlice/, 'a NOT EXISTS rides the right-join level');
is(run($q), local_run($q), 'and answers the same');

# A correlated sublink over the rows of the level
$q = 'SELECT count(*) FROM t3 WHERE EXISTS (SELECT 1 FROM t1 WHERE t1.a = t3.a) ' .
	'AND EXISTS (SELECT 1 FROM t2 WHERE t2.a = t3.a)';
like(plan_for($q), qr/GpSlice/, 'correlated sublinks ride the level');
is(run($q), local_run($q), 'and answer the same');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
