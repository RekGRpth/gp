# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The ranges and the values of one key column distribute a table as well:
# a row goes to the segment of the range that holds it, or of the value
# that equals it, and a row whose key has no range and no value cannot be
# inserted.  The planner answers by the key columns as it answers by the
# columns of a hash: the grouping by the key runs on the segments, and the
# joins of the tables that are not colocated move by their keys

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

# The truth: the rows of every segment, read there directly
sub shares { return join(' / ', map { $_->safe_psql('postgres', $_[0]) } @segments); }

# The ranges: 1..10 on segment 1, 11..20 on segment 2, the rest on 3
run('CREATE TABLE t (a int, b text)');
run("SELECT gp.distribute_range('t', 'a', ARRAY['10', '20'], ARRAY[1, 2, 3])");
run("INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 35) gs(g)");
is(shares('SELECT count(*) FROM t'), '10 / 10 / 15',
	'the ranges route the rows to their segments');
is(run('SELECT sum(a) FROM t'), 630, 'and none of them are lost');

# The bounds of the policy, as the catalog keeps them
is(run('SELECT segment FROM gp.distribution_rules ORDER BY bucket'),
	"1\n2\n3", 'the rules name the segment of every range');

# The values: 7 on segment 1, 15 on segment 2
run('CREATE TABLE lv (a int, b text)');
run("SELECT gp.distribute_list('lv', 'a', ARRAY['7', '15'], ARRAY[1, 2])");
run("INSERT INTO lv VALUES (7, 'a'), (15, 'b')");
is(shares('SELECT count(*) FROM lv'), '1 / 1 / 0',
	'the values of a list route to their segments');

# A key with no value cannot be inserted: the last range is open above,
# so a range table takes every key of its column up to it, but a key
# without a value has no bucket
eval { run("INSERT INTO t VALUES (NULL, 'n')") };
like($@, qr/is NULL/, 'a row without a key is refused');
eval { run("INSERT INTO lv VALUES (8, 'x')") };
like($@, qr/matches no value/, 'a row of no value of the list is refused');
is(run('SELECT count(*) FROM t'), 35, 'and none of them landed');

# The misuses refuse at the distribution
run('CREATE TABLE t2 (a int, b text)');
eval { run("SELECT gp.distribute_range('t2', 'a', ARRAY['20', '10'], ARRAY[1, 2, 3])") };
like($@, qr/ascend strictly/, 'the bounds must ascend');
eval { run("SELECT gp.distribute_range('t2', 'a', ARRAY['10'], ARRAY[1, 2, 3])") };
like($@, qr/2 buckets, but 3 segments/, 'the segments must match the ranges');
eval { run("SELECT gp.distribute_list('t2', 'a', ARRAY\['1', '1'\], ARRAY[1, 2])") };
like($@, qr/must differ/, 'the values of a list must differ');

# The planner: the grouping by the key runs where the rows live, the
# aggregate answers the skew of the data by the ranges
like(plan_for('SELECT a, count(*) FROM t GROUP BY a'), qr/GpSlice/,
	'the grouping by the key of a range rides the segments');
is(run('SELECT count(*) FROM (SELECT a, count(*) FROM t GROUP BY a) s'),
	35, 'and the groups are the rows');
like(plan_for('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1'),
	qr/GpSlice/, 'the segment key groups a range table too');
is(run('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1 ORDER BY 1'),
	"1|10\n2|10\n3|15", 'the skew of the ranges by the segments');

# The joins of tables that are not colocated move by their keys
run('CREATE TABLE lv2 (a int, b text)');
run("SELECT gp.distribute_list('lv2', 'a', ARRAY['7'], ARRAY[2])");
run("INSERT INTO lv2 VALUES (7, 'x')");
like(plan_for('SELECT count(*) FROM t JOIN lv2 USING (a)'), qr/GpSlice|Gather/,
	'the join of a range and a list table answers on the cluster');
is(run('SELECT count(*) FROM t JOIN lv2 USING (a)'), 1,
	'and the pairs are the ones of the whole');

$coordinator->stop;
done_testing();
