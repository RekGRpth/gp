# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The segment of a row: gp.segment_id() of a query the segments answer is
# the value of the server that read the row.  A grouping keyed by it keeps
# every group on one segment, so the aggregate of the query is answered
# there in whole — the skew of the data by the segments, the ORDER BY of
# the query and all — while on the coordinator the function is 0

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE t (a int, b text, c numeric)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t SELECT g % 10, 'x' || g, g * 1.5 FROM generate_series(1, 1000) g");
# The same rows in a table of the coordinator, for the answers of the whole
run('CREATE TABLE l AS SELECT * FROM t');

# The truth: every segment reads its own share, and names itself there
sub segments_read
{
	my ($agg, $where, $group) = @_;
	$where //= '';
	$group //= '';
	my @rows;
	foreach my $segment (@segments)
	{
		my $number = $segment->safe_psql('postgres',
			'SELECT gp.segment_id() FROM t LIMIT 1');
		push(@rows, map { "$number|$_" }
			 split(/\n/, $segment->safe_psql('postgres',
				 "SELECT $agg FROM t $where $group")));
	}
	return join("\n", sort @rows);
}

# The skew of the data by the segments: the flagship query of the function
like(plan_for('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1'),
	qr/GpSlice/, 'the grouping by the segment rides the segments');
like(plan_for('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1 ORDER BY 1'),
	qr/GpSlice/, 'the order by the segment does not stop the pushdown');
like(plan_for('SELECT gp.segment_id(), a % 3, avg(c) FROM t GROUP BY 1, 2'),
	qr/GpSlice/, 'the segment beside another key rides too');
unlike(plan_for('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1 ORDER BY 1'),
	qr/GpGather/, 'the pushdown does not move the whole table for the sort');

is(run('SELECT gp.segment_id(), count(*) FROM t GROUP BY 1 ORDER BY 1'),
	segments_read('count(*)'),
	'the counts of the segments are the counts of their own rows');
is(run('SELECT gp.segment_id(), avg(c) FROM t GROUP BY 1 ORDER BY 1'),
	segments_read('avg(c)'),
	'the averages are the averages of their own rows');
is(run('SELECT gp.segment_id(), a % 3, count(*) FROM t GROUP BY 1, 2 ORDER BY 1, 2'),
	segments_read('a % 3, count(*)', '', 'GROUP BY a % 3'),
	'a key beside the segment groups within its segments');
is(run('SELECT gp.segment_id(), sum(c), min(b), max(b) FROM t GROUP BY 1 ORDER BY 1'),
	segments_read('sum(c), min(b), max(b)'),
	'sum, min and max of the groups of the segment');
is(run('SELECT gp.segment_id(), count(*) FROM t WHERE c > 500 GROUP BY 1 ORDER BY 1'),
	segments_read('count(*)', 'WHERE c > 500'),
	'a filter rides with the segment key');

# The rows of one segment alone, filtered by the segment of the query
my $first = $segments[0]->safe_psql('postgres',
	'SELECT gp.segment_id() FROM t LIMIT 1');
my $first_count = $segments[0]->safe_psql('postgres', 'SELECT count(*) FROM t');
is(run("SELECT gp.segment_id(), count(*) FROM t WHERE gp.segment_id() = $first " .
	   'GROUP BY 1'),
	"$first|$first_count",
	'WHERE by the segment keeps the rows of that segment');

# On the coordinator the function is 0
is(run('SELECT gp.segment_id(), count(*) FROM l'), "0|1000",
	'the function of a table of the coordinator is 0');
is(run("SELECT count(*) FROM t WHERE gp.segment_id() = $first " .
	   'AND gp.segment_id() = 0'), 0,
	'a row is not of the coordinator and of a segment at once');

# The totals stay the ones of the whole table
is(run('SELECT sum(n) FROM (SELECT gp.segment_id(), count(*) AS n ' .
	   'FROM t GROUP BY 1) s'),
	run('SELECT count(*) FROM l'),
	'the groups of the segments add up to the whole table');

$coordinator->stop;
done_testing();
