# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# An aggregate grouped by expressions, and a global aggregate that does not
# move all rows to one segment, is answered in two steps: the segments
# aggregate their own rows, and the coordinator finishes the aggregate over
# the columns of every segment.  min, max and sum of a closing type combine
# by the same aggregate again

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
# The same rows in a table of the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');

like(plan_for('SELECT a % 2, sum(c) FROM t GROUP BY a % 2'),
	qr/GpSlice/, 'a sum by another column is answered partly on the segments');
like(plan_for('SELECT a % 2, min(c), max(c) FROM t GROUP BY a % 2'),
	qr/GpSlice/, 'min and max by another column are answered partly');
like(plan_for('SELECT min(c), max(c), sum(c) FROM t'),
	qr/GpSlice/, 'a global aggregate is answered partly on the segments');
like(plan_for('SELECT sum(c) FROM t WHERE c > 100 AND c < 1000'),
	qr/GpSlice/, 'a partial aggregate deparses several WHERE conditions');
like(plan_for('SELECT a % 2, min(c) FROM t GROUP BY a % 2 HAVING min(c) > 100'),
	qr/GpSlice/, 'HAVING stays on the coordinator and does not stop the answer');
unlike(plan_for('SELECT a % 2, count(*) FROM t GROUP BY a % 2'), qr/GpSlice/,
	'count counts the partial columns, so it stays on the coordinator');
like(plan_for('SELECT avg(c) FROM t'), qr/GpSlice/,
	'the average answers a partial sum and a partial count on the segments');
like(plan_for('SELECT a % 2, avg(c) FROM t GROUP BY a % 2'), qr/GpSlice/,
	'a grouped average is answered partly on the segments');
unlike(plan_for('SELECT a % 2, sum(c), count(*) FROM t GROUP BY a % 2'),
	qr/GpSlice/, 'a mix with count stays on the coordinator');

my @queries = (
	'SELECT a % 2, sum(c) FROM t GROUP BY a % 2 ORDER BY 1',
	'SELECT a % 3, min(c), max(c) FROM t GROUP BY a % 3 ORDER BY 1',
	'SELECT min(c), max(c), sum(c) FROM t',
	'SELECT upper(substring(b, 1, 2)), sum(c) FROM t GROUP BY 1 ORDER BY 1',
	'SELECT a % 2, min(c) FROM t GROUP BY a % 2 HAVING min(c) > 100 ORDER BY 1',
	'SELECT a % 2, sum(c) FROM t GROUP BY a % 2 HAVING sum(c) > 300000 ORDER BY 1',
	'SELECT a % 2, sum(c) FROM t WHERE c > 100 GROUP BY a % 2 ORDER BY 1',
	'SELECT sum(c) FROM t HAVING max(c) > 1000',
	'SELECT sum(c) FROM t WHERE c > 100 AND c < 1000',
	'SELECT a % 2, count(*), avg(c) FROM t GROUP BY a % 2 ORDER BY 1',
	'SELECT b, sum(c) FROM t GROUP BY b ORDER BY b LIMIT 5',
	'SELECT avg(c) FROM t',
	'SELECT avg(a) FROM t',
	'SELECT a % 2, avg(c) FROM t GROUP BY a % 2 ORDER BY 1',
	'SELECT avg(c) FROM t WHERE c > 100 AND c < 100000',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bt\b/l/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
