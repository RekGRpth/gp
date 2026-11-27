# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The share of the coordinator: a hash distribution whose map names the
# coordinator itself keeps some of its rows in the copy of the table here,
# and they insert through the executor of the coordinator -- with its
# indexes and its toast -- while the rest route to the segments as usual.
# The gather reads the share beside the answers of the segments, so the
# query of the cluster is the truth of the whole; the rules of the join
# and of the partial aggregate ride the hash of the keys, which the rows
# of the share keep no order by, so such tables answer by the coordinator

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE t (a int, b text)');
run("SELECT gp.distribute('t', ARRAY['a'], NULL, NULL, true)");

# The map of the group names the coordinator: the bucket 0 is its share
ok(run("SELECT 0 = ANY (map) FROM gp.colocation_groups g " .
	   "JOIN gp.distribution_policy p USING (colocation_id) " .
	   "WHERE p.relid = 't'::regclass"),
	'the map of the group keeps the coordinator');

# The rows: some land on the coordinator, the rest on the segments; the
# query of the cluster sees all of them, and the difference names the
# share of the coordinator
run("INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g)");
is(run('SELECT count(*) FROM t'), 30, 'every row lands once');
my $segment_rows = 0;
foreach my $segment (@segments)
{
	$segment_rows += $segment->safe_psql('postgres', 'SELECT count(*) FROM t');
}
my $share = 30 - $segment_rows;
ok($share > 0, 'the coordinator keeps its share');
ok($segment_rows > 0, 'and the segments keep theirs');

# The queries of the cluster see the share beside the segments
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the query of the cluster adds the share of the coordinator');
is(run('SELECT count(*) FROM t WHERE a % 2 = 0'), 15,
	'a condition counts the rows of every server');
is(run('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1 ORDER BY 1'),
	"0|$share
1|10
2|8
3|7",
	'the segment key names the share of the coordinator as segment 0');

# A plain insert of values lands the same way
run("INSERT INTO t VALUES (500, 'd')");
is(run('SELECT count(*) FROM t'), 31, 'the values insert lands too');
is(run('SELECT b FROM t WHERE a = 500'), 'd', 'and the row is readable');

# The joins and the partial aggregates refuse: the rules ride the hash
run('CREATE TABLE t2 (a int, c text)');
run("SELECT gp.distribute('t2', ARRAY['a'])");
unlike(plan_for('SELECT count(*) FROM t JOIN t2 USING (a)'), qr/GpSlice/,
	'the join of a shared table answers on the coordinator');
unlike(plan_for('SELECT a % 2, count(*) FROM t GROUP BY 1'), qr/GpSlice/,
	'the partial aggregate of a shared table answers on the coordinator');

# UPDATE and DELETE refuse: the rows of the share keep out of the segments
eval { run('UPDATE t SET b = NULL') };
like($@, qr/not supported yet/, 'the update refuses');
eval { run('DELETE FROM t WHERE a = 1') };
like($@, qr/not supported yet/, 'the delete refuses');

$coordinator->stop;
done_testing();
