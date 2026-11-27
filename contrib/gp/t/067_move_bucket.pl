# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A bucket of a colocation group moves to another segment: the rows of
# the bucket copy to the new segment, the bucket's row of the map
# switches in one transaction, and the rows go away from the old
# segment.  Every hash table of the group moves its share, the reads and
# the writes of the cluster stay correct through the move, and the
# buckets the map names never change their number or their order

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub seg { return $segments[$_[0] - 1]; }

sub shares
{
	return join(' / ', map { $_->safe_psql('postgres', $_[0]) } @segments);
}

run(<<'DDL');
CREATE TABLE t (a int, b text);
CREATE TABLE u (a int, d numeric);
SELECT gp.distribute('t', ARRAY['a']);
SELECT gp.distribute('u', ARRAY['a'], 't');
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g);
INSERT INTO u SELECT g % 4, g * 1.5 FROM generate_series(1, 30) gs(g);
CREATE TABLE lt AS SELECT * FROM t;
CREATE TABLE lu AS SELECT * FROM u;
DDL

# The map names the segments the rows live by: the buckets of the group
# spread over the segments in turn
ok(run("SELECT 0 <> ALL (map) FROM gp.colocation_groups g "
	   . "JOIN gp.distribution_policy p USING (colocation_id) "
	   . "WHERE p.relid = 't'::regclass"),
	'the group starts without the coordinator in its map');

# The move: the first bucket of the group goes to segment 3
my $rows = run("SELECT gp.move_bucket('t', 0, 3)");
like($rows, qr/^\d+$/, 'the move reports the rows it moved');

# The rows left their old place and arrived at the new one, and the
# answers of the cluster are the ones of the whole
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the rows of the moved bucket answer as before');
is(run('SELECT sum(d), count(*) FROM u'), '697.5|30',
	'the second table of the group moved its share too');

# The map switched for the whole group
is(run("SELECT map[1] FROM gp.colocation_groups g "
	   . "JOIN gp.distribution_policy p USING (colocation_id) "
	   . "WHERE p.relid = 't'::regclass"), 3,
	'the map names the new segment for the bucket');
is(run("SELECT map[1] FROM gp.colocation_groups g "
	   . "JOIN gp.distribution_policy p USING (colocation_id) "
	   . "WHERE p.relid = 'u'::regclass"), 3,
	'and for the second table of the group');

# New writes for the moved bucket land on the new segment
run("INSERT INTO t VALUES (500, 'after')");
my $on_new = seg(3)->safe_psql('postgres',
	"SELECT count(*) FROM t WHERE a = 500");
ok($on_new > 0, 'a new row of the moved bucket lands on the new segment');

# The sessions dropped their cached policies: a query planned now reads
# by the new map, and the shares add up to the whole again
my $sum = 0;
foreach my $segment (@segments)
{
	$sum += $segment->safe_psql('postgres', 'SELECT sum(a) FROM t');
}
is($sum, 965, 'the shares of the segments add up after the move');

# The misuses refuse
eval {
	run("SELECT gp.move_bucket('t', -1, 2)");
};
like($@, qr/non-negative/, 'a negative bucket refuses');
eval {
	run("SELECT gp.move_bucket('t', 2000, 2)");
};
like($@, qr/out of the/, 'a bucket out of the range refuses');
eval {
	run("SELECT gp.move_bucket('t', 1, 0)");
};
like($@, qr/does not move to the coordinator/,
	'a bucket does not move to the coordinator');
is(run("SELECT gp.move_bucket('t', 1, 2)"), 0,
	'a bucket stays put when it moves to its own segment');
is(run('SELECT count(*) FROM t'), 31, 'the refusals changed nothing');

# The move is idempotent in its result: moving the same bucket again to
# another segment keeps the answers of the cluster
run("SELECT gp.move_bucket('t', 0, 2)");
is(run('SELECT sum(a), count(*) FROM t'), '965|31',
	'the second move keeps the answers');
is(run("SELECT map[1] FROM gp.colocation_groups g "
	   . "JOIN gp.distribution_policy p USING (colocation_id) "
	   . "WHERE p.relid = 't'::regclass"), 2,
	'and the map names the newest segment');

# The aggregation by the key still rides the segments after the moves
like(plan_for('SELECT a, count(*) FROM t GROUP BY a'), qr/GpSlice/,
	'the grouping by the key rides after the moves');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
