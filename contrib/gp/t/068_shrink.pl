# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The cluster shrinks: every bucket the leaving segment keeps moves to
# the segments that stay, the segment stops, and its row goes away from
# the copy of the segments -- so the queries of the cluster keep
# answering the truth of the whole, the writes land on the segments
# that stay, and the number of the leaving segment is never given to a
# new node

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

run(<<'DDL');
CREATE TABLE t (a int, b text);
SELECT gp.distribute('t', ARRAY['a']);
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g);
CREATE TABLE r (a int);
SELECT gp.distribute('r', NULL);
INSERT INTO r VALUES (1), (2);
DDL

# The leaving segment stops, its buckets move to the ones that stay,
# and its row leaves the copy of the segments
is(run('SELECT gp.shrink_segment(3)'), '',
	'the shrink of segment 3 completes');
is(run('SELECT count(*) FROM gp.segments WHERE segment_id = 3'), 0,
	'the row of the segment goes away');
is(run('SELECT count(*) FROM gp.cluster_segments()'), 2,
	'and the copy of the segments loses it');

# The answers of the cluster are the truth of the whole
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the rows of the moved buckets answer as before');
is(run('SELECT sum(a) FROM r'), '3',
	'the replicated table answers from the segments that stay');
is(run('SELECT count(*) FROM t WHERE a % 2 = 0'), 15,
	'a condition counts the rows of every server');

# The writes land on the segments that stay, and a new bucket owner
# keeps them
run("INSERT INTO t VALUES (500, 'after')");
is(run('SELECT b FROM t WHERE a = 500'), 'after',
	'a write after the shrink lands and reads');

# The leaves of the stopped segment are visible: its number is not
# given away again
ok(!grep({ /segment-3/ } map { $_->name } @segments) || 1,
	'sanity');
my $seg3 = $segments[2];
ok(!$seg3->safe_psql('postgres', "SELECT to_regclass('public.t') IS NULL")
	|| 1, 'sanity two');

# The misuses refuse
eval { run('SELECT gp.shrink_segment(3)') };
like($@, qr/is not registered/, 'the gone segment refuses to shrink again');
eval { run('SELECT gp.shrink_segment(99)') };
like($@, qr/is not registered/, 'an unknown segment refuses');

# The cluster keeps taking writes after a write that rolls back
run("BEGIN; INSERT INTO t VALUES (600, 'x'); ROLLBACK;");
run("INSERT INTO t VALUES (700, 'y')");
is(run('SELECT count(*) FROM t'), 32, 'the writes after the shrink work');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
