# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A chain of base tables joined by one kind of an outer join keeps the
# rows its every join keeps: a chain of LEFT JOINs stays on the segments
# with the first table and broadcasts the rest, a chain of FULL JOINs
# broadcasts everything to one segment, and the answers match the
# coordinator.  Mixed levels stay on the coordinator, and so does an
# aggregate by a column of a null-supplying table

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
	'SELECT count(*) = 3 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE c (custkey int, nationkey int)');
run("SELECT gp.distribute('c', ARRAY['custkey'])");
run('INSERT INTO c SELECT i, i % 5 FROM generate_series(2, 11) i');
run('CREATE TABLE o (orderkey bigint, custkey int, priority text)');
run("SELECT gp.distribute('o', ARRAY['orderkey'])");
run("INSERT INTO o SELECT i, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 10 + 1 END, CASE WHEN i % 2 = 0 THEN 'HIGH' ELSE 'LOW' END FROM generate_series(1, 20) i");
run('CREATE TABLE l (orderkey bigint, suppkey int, quantity numeric)');
run("SELECT gp.distribute('l', ARRAY['orderkey'])");
run('INSERT INTO l SELECT i, i % 5 + 1, i % 10 + 1 FROM generate_series(2, 41) i');
run('CREATE TABLE n (nationkey int, name text)');
run("SELECT gp.distribute('n', NULL)");
run("INSERT INTO n VALUES (0, 'A'), (1, 'B'), (2, 'C'), (3, 'D'), (4, 'E')");
run('CREATE TABLE lc AS SELECT * FROM c');
run('CREATE TABLE lo AS SELECT * FROM o');
run('CREATE TABLE ll AS SELECT * FROM l');
run('CREATE TABLE ln AS SELECT * FROM n');

my @queries = (
	'SELECT o.orderkey, sum(l.quantity) FROM o LEFT JOIN c ON o.custkey = c.custkey LEFT JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey',
	'SELECT c.custkey, count(*) FROM c LEFT JOIN o ON o.custkey = c.custkey LEFT JOIN n ON c.nationkey = n.nationkey GROUP BY c.custkey ORDER BY c.custkey',
	'SELECT o.orderkey, count(l.suppkey) FROM o LEFT JOIN c ON o.custkey = c.custkey LEFT JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey',
	'SELECT o.orderkey, sum(l.quantity) FROM o FULL JOIN c ON o.custkey = c.custkey FULL JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey',
	'SELECT count(*) FROM o FULL JOIN c ON o.custkey = c.custkey FULL JOIN n ON c.nationkey = n.nationkey',
);
my @locals = (
	'SELECT lo.orderkey, sum(ll.quantity) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey LEFT JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lo.orderkey ORDER BY lo.orderkey',
	'SELECT lc.custkey, count(*) FROM lc LEFT JOIN lo ON lo.custkey = lc.custkey LEFT JOIN ln ON lc.nationkey = ln.nationkey GROUP BY lc.custkey ORDER BY lc.custkey',
	'SELECT lo.orderkey, count(ll.suppkey) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey LEFT JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lo.orderkey ORDER BY lo.orderkey',
	'SELECT lo.orderkey, sum(ll.quantity) FROM lo FULL JOIN lc ON lo.custkey = lc.custkey FULL JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lo.orderkey ORDER BY lo.orderkey',
	'SELECT count(*) FROM lo FULL JOIN lc ON lo.custkey = lc.custkey FULL JOIN ln ON lc.nationkey = ln.nationkey',
);

foreach my $i (0 .. $#queries)
{
	is(run($queries[$i]), run($locals[$i]),
		"the same answer as on the coordinator: $queries[$i]");
}

like(plan_for($queries[0]), qr/GpSlice/,
	'a left join chain runs as a slice with the first table kept');
like(plan_for($queries[3]), qr/GpSlice/,
	'a full join chain runs as a slice on one segment');

# The kept side of the chain is the first table: its null-supplying
# partners cannot carry an aggregate, and a mixed level is not moved
{
	my $nullable = 'SELECT c.custkey, count(*) FROM o LEFT JOIN c ON o.custkey = c.custkey LEFT JOIN l ON l.orderkey = o.orderkey GROUP BY c.custkey ORDER BY c.custkey';
	my $nullable_local = 'SELECT lc.custkey, count(*) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey LEFT JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lc.custkey ORDER BY lc.custkey';

	is(run($nullable), run($nullable_local),
		'an aggregate by a null-supplying column is still right');
	my $mixed = 'SELECT o.orderkey, sum(l.quantity) FROM o LEFT JOIN c ON o.custkey = c.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey';
	my $mixed_local = 'SELECT lo.orderkey, sum(ll.quantity) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lo.orderkey ORDER BY lo.orderkey';

	is(run($mixed), run($mixed_local),
		'a mixed level is still right');
	unlike(plan_for($mixed), qr/GpSlice/,
		'and stays on the coordinator');
}

# NULL keys on the broadcast sides keep their rows through every join
{
	run('INSERT INTO c VALUES (NULL, 0)');
	run('INSERT INTO o VALUES (100, NULL, \'NULLKEY\')');
	run('INSERT INTO l VALUES (100, 1, 1)');
	run('INSERT INTO lc VALUES (NULL, 0)');
	run('INSERT INTO lo VALUES (100, NULL, \'NULLKEY\')');
	run('INSERT INTO ll VALUES (100, 1, 1)');
	my $nulls = 'SELECT o.orderkey, count(l.suppkey) FROM o LEFT JOIN c ON o.custkey = c.custkey LEFT JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey';
	my $nulls_local = 'SELECT lo.orderkey, count(ll.suppkey) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey LEFT JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lo.orderkey ORDER BY lo.orderkey';

	is(run($nulls), run($nulls_local),
		'null keys of the broadcast sides keep their rows');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
