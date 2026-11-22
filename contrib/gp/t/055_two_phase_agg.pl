# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# An aggregate grouped by columns that do not cover the distribution key is
# answered in two phases.  Every segment runs a partial aggregate over its
# own rows; the partials of all the segments answer one combine query, in
# which every segment combines the groups the hash of the grouping columns
# assigns to it, so that every group is combined exactly once.  The
# coordinator finishes the aggregate over the per-segment answers.  No row
# of the table crosses the interconnect: the fragment combines a serialized
# partial aggregate through gp.peer_query(), and the ownership filter reads
# the number of the segment it runs on

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub remote_sql { return $coordinator->safe_psql('postgres', "EXPLAIN (VERBOSE, COSTS OFF) $_[0]"); }

run('CREATE TABLE t (a int, b int, c int, s text, n numeric(10, 2))');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t SELECT g, g % 7, g % 11, 'g' || (g % 5), g * 1.5 FROM generate_series(1, 1000) g");
# The same rows in a table of the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');

my $grouped = 'SELECT b, count(*), sum(c), min(s), max(s) FROM t GROUP BY b';
like(plan_for($grouped), qr/GpSlice/,
	'a grouped count and sum is answered in two phases');
my $remote = remote_sql($grouped);
like($remote, qr/gp\.peer_query/, 'the fragment combines a serialized partial aggregate');
like($remote, qr/gp\.segment_id/, 'the ownership filter reads the segment of the fragment');
like($remote, qr/GROUP BY/, 'the fragment groups the partials by the grouping columns');

like(plan_for('SELECT b, sum(c * 2 + 1), count(*) FROM t GROUP BY b HAVING sum(c) > 700'),
	qr/GpSlice/, 'expression arguments and HAVING move too');
like(plan_for('SELECT b, sum(n) FROM t GROUP BY b'), qr/GpSlice/,
	'a numeric sum answers in its own type without a cast');
like(plan_for('SELECT b, avg(c), count(*) FROM t GROUP BY b'), qr/GpSlice/,
	'an average moves through a partial sum and a partial count');
my $avg_remote = remote_sql('SELECT b, avg(c) FROM t GROUP BY b');
like($avg_remote, qr/sum\(gp_p2\)::bigint.*sum\(gp_p3\)::bigint/s,
	'the combine query keeps the sums and counts of the partials');
like(plan_for('SELECT b, stddev(c) FROM t GROUP BY b'), qr/GpSlice/,
	'an aggregate without a combine function redistributes its rows instead');

# The motion of a two-phase aggregate cannot see the writes of its transaction
my $session = $coordinator->background_psql('postgres', on_error_stop => 1);
$session->query_safe('BEGIN');
$session->query_safe('INSERT INTO t VALUES (2001, 8, 3, \'new\', 1)');
like($session->query_safe('EXPLAIN (COSTS OFF) SELECT b, count(*) FROM t GROUP BY b'),
	qr/GpSlice/,
	'an aggregate after a write is answered in two steps without a motion');
$session->query_safe('ROLLBACK');
$session->quit;

my @queries = (
	'SELECT b, count(*) FROM t GROUP BY b ORDER BY b',
	'SELECT b, sum(c), min(c), max(c) FROM t GROUP BY b ORDER BY b',
	'SELECT s, count(*), sum(c), min(b), max(b) FROM t GROUP BY s ORDER BY s',
	'SELECT b, sum(c * 2 + 1), count(*) FROM t WHERE a <= 500 GROUP BY b ORDER BY b',
	'SELECT b, sum(c) FROM t GROUP BY b HAVING sum(c) > 700 ORDER BY b',
	'SELECT b, count(*) FROM t WHERE s = \'g1\' GROUP BY b HAVING count(*) > 5 ORDER BY b',
	'SELECT b, min(s), max(s) FROM t GROUP BY b ORDER BY b',
	'SELECT b, sum(n) FROM t GROUP BY b ORDER BY b',
	'SELECT b, avg(c), count(*) FROM t GROUP BY b ORDER BY b',
	'SELECT b, avg(n) FROM t GROUP BY b ORDER BY b',
	'SELECT b, avg(c * 2), sum(c), min(c), max(c) FROM t GROUP BY b ORDER BY b',
	'SELECT b, count(*), avg(c) FROM t GROUP BY b HAVING avg(c) > 5 ORDER BY b',
	'SELECT count(*), sum(c) FROM t',
	'SELECT b, avg(c) FROM t WHERE a <= 500 GROUP BY b ORDER BY b',
	'SELECT b, count(*), sum(c) FROM t GROUP BY b ORDER BY sum(c) DESC, b LIMIT 3',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt\b/l/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

# A table pinned to one segment is read there alone
run('CREATE TABLE p (a int, b int)');
run("SELECT gp.distribute('p', ARRAY['a'], segment => 1)");
run('INSERT INTO p SELECT g, g % 3 FROM generate_series(1, 30) g');
run('CREATE TABLE pl AS SELECT * FROM p');
like(plan_for('SELECT b, count(*) FROM p GROUP BY b'), qr/GpSlice/,
	'a pinned table is aggregated in two phases');
is(run('SELECT b, count(*) FROM p GROUP BY b ORDER BY b'),
	run('SELECT b, count(*) FROM pl GROUP BY b ORDER BY b'),
	'a pinned table answers on its segment');

$_->stop foreach (@segments, $coordinator);

done_testing();
