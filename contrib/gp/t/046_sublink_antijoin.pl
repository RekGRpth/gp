# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A join the sublinks drive alone — a semi, anti or scalar sublink over
# other tables — keeps the table of the fromlist and broadcasts every
# table of a sublink whole: the rows of the answer are the rows of the
# kept table, so a customer with an order on another segment is not kept
# twice, and the scalar subselect of a condition answers the same on
# every segment.  The query of the TPC-H Q22 shape runs on the segments
# above this join

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

my @segments;
foreach my $segment_id (1 .. 3)
{
	my $node = PostgreSQL::Test::Cluster->new("seg$segment_id");
	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
max_prepared_transactions = 10
");
	$node->start;
	push @segments, $node;
}
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 3 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE c (k int, ph text, bal numeric)');
run("SELECT gp.distribute('c', ARRAY['k'])");
run('CREATE TABLE o (ok int, k int)');
run("SELECT gp.distribute('o', ARRAY['ok'])");
# Thirty customers of two countries; the orders leave nine without one,
# and every ordered customer keeps an order on exactly one segment
run("INSERT INTO c SELECT g, '1' || (g % 2)::text || '-x', g * 10.0
	FROM generate_series(1, 30) g");
run('INSERT INTO o SELECT g, g FROM generate_series(1, 21) g');
# The same rows on the coordinator, to compare the answers with
run('CREATE TABLE lc AS SELECT * FROM c');
run('CREATE TABLE lo AS SELECT * FROM o');

# The anti-join keeps a customer of one segment even when its orders sit
# on another: no row is kept twice.  A count over the bare join keeps its
# single group with the coordinator
like(plan_for('SELECT k FROM c WHERE NOT EXISTS (
	SELECT * FROM o WHERE o.k = c.k) ORDER BY k'), qr/GpSlice/,
	'an anti join driven by a sublink runs on the segments');
is(run('SELECT count(*) FROM c WHERE NOT EXISTS (
	SELECT * FROM o WHERE o.k = c.k)'),
	run('SELECT count(*) FROM lc WHERE NOT EXISTS (
	SELECT * FROM lo WHERE lo.k = lc.k)'),
	'a kept customer is answered once');
is(run('SELECT count(*) FROM (SELECT k FROM c WHERE NOT EXISTS (
	SELECT * FROM o WHERE o.k = c.k)) x'),
	run('SELECT count(*) FROM (SELECT k FROM lc WHERE NOT EXISTS (
	SELECT * FROM lo WHERE lo.k = lc.k)) x'),
	'a count over the derived anti join is answered on the segments');

# The query of the Q22 shape: the same anti join with the scalar
# subselect of the condition and the grouping above
my $shape = 'SELECT substring(ph from 1 for 2) AS cn, count(*) AS numcust,
	sum(bal) AS tot
FROM c
WHERE substring(ph from 1 for 2) IN (\'11\', \'10\')
	AND bal > (SELECT avg(bal) FROM c WHERE bal > 0)
	AND NOT EXISTS (SELECT * FROM o WHERE o.k = c.k)
GROUP BY 1 ORDER BY 1';
my $local = $shape;
$local =~ s/\bc\b/lc/g;
$local =~ s/\bo\b/lo/g;
like(plan_for($shape), qr/GpSlice/,
	'the query of the Q22 shape runs on the segments');
is(run($shape), run($local),
	'the Q22 shape returns the same answer as on the coordinator');

# A scalar subselect alone drives the fragment of its table as well
is(run('SELECT count(*) FROM c WHERE bal > (SELECT avg(bal) FROM c)'),
	run('SELECT count(*) FROM lc WHERE bal > (SELECT avg(bal) FROM lc)'),
	'a scalar subselect answers the same on the segments');

# An outer join inside a sublink keeps the query with the coordinator
unlike(plan_for('SELECT count(*) FROM c WHERE EXISTS (
	SELECT 1 FROM o LEFT JOIN c c2 ON c2.k = o.k WHERE o.k = c.k)'),
	qr/GpSlice/,
	'an outer join inside a sublink stays on the coordinator');

$_->stop foreach (@segments, $coordinator);

done_testing();
