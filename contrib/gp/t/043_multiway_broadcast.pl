# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A multiway join keeps a colocated component and broadcasts the other hashed
# tables to every segment.  Replicated inputs need no Motion

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(same_answers start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }

run('CREATE TABLE c (custkey int, nationkey int)');
run("SELECT gp.distribute('c', ARRAY['custkey'])");
run('INSERT INTO c SELECT i, i % 5 FROM generate_series(1, 10) i');
run('CREATE TABLE o (orderkey bigint, custkey int, priority text)');
run("SELECT gp.distribute('o', ARRAY['orderkey'])");
run("INSERT INTO o SELECT i, i % 10 + 1, CASE WHEN i % 2 = 0 THEN 'HIGH' ELSE 'LOW' END FROM generate_series(1, 20) i");
run('CREATE TABLE l (orderkey bigint, suppkey int, quantity numeric)');
run("SELECT gp.distribute('l', ARRAY['orderkey'])");
run('INSERT INTO l SELECT i, i % 5 + 1, i % 10 + 1 FROM generate_series(1, 40) i');
run('CREATE TABLE s (suppkey int, name text)');
run("SELECT gp.distribute('s', ARRAY['suppkey'])");
run("INSERT INTO s SELECT i, 's' || i FROM generate_series(1, 5) i");
run('CREATE TABLE n (nationkey int, name text)');
run("SELECT gp.distribute('n', NULL)");
run("INSERT INTO n VALUES (0, 'A'), (1, 'B'), (2, 'C'), (3, 'D'), (4, 'E')");
run('CREATE TABLE lc AS SELECT * FROM c');
run('CREATE TABLE lo AS SELECT * FROM o');
run('CREATE TABLE ll AS SELECT * FROM l');
run('CREATE TABLE ls AS SELECT * FROM s');
run('CREATE TABLE ln AS SELECT * FROM n');

like(plan_for('SELECT o.orderkey, sum(l.quantity) FROM c JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey'),
	qr/GpSlice/, 'customer is broadcast while orders and lineitem stay colocated');
unlike(plan_for('SELECT c.custkey, avg(l.quantity) FROM c JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY c.custkey'), qr/GpSlice/,
	'the join result is not redistributed by the broadcast key: the rewrite is not built yet');
unlike(plan_for('SELECT name, count(*) FROM s JOIN l ON s.suppkey = l.suppkey JOIN o ON l.orderkey = o.orderkey JOIN c ON o.custkey = c.custkey GROUP BY name'), qr/GpSlice/,
	'supplier and customer are not broadcast in one multiway plan: the rewrite is not built yet');
like(plan_for('SELECT n.name, sum(l.quantity) FROM c JOIN n ON c.nationkey = n.nationkey JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY n.name'),
	qr/GpSlice/, 'a replicated nation needs no additional Motion');
unlike(plan_for('SELECT c.custkey, sum(l.quantity) FROM c LEFT JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY c.custkey'),
	qr/GpSlice/, 'a multiway outer join keeps the coordinator fallback');

run('SET enable_mergejoin = off');
my @queries = (
	'SELECT o.orderkey, sum(l.quantity) FROM c JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey',
	'SELECT c.custkey, avg(l.quantity) FROM c JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY c.custkey ORDER BY c.custkey',
	'SELECT name, count(*) FROM s JOIN l ON s.suppkey = l.suppkey JOIN o ON l.orderkey = o.orderkey JOIN c ON o.custkey = c.custkey GROUP BY name ORDER BY name',
	'SELECT n.name, sum(l.quantity) FROM c JOIN n ON c.nationkey = n.nationkey JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY n.name ORDER BY n.name',
	'SELECT c.custkey, sum(l.quantity) FROM c LEFT JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY c.custkey ORDER BY c.custkey',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bc\b/lc/g;
	$local =~ s/\bo\b/lo/g;
	$local =~ s/\bl\b/ll/g;
	$local =~ s/\bs\b/ls/g;
	$local =~ s/\bn\b/ln/g;

	# The distributed answer is checked several times: an answer of a
	# multiway slice was once seen with a group counted twice and another
	# one missing, and a single check misses such a race
	for my $attempt (1 .. 5)
	{
		same_answers($coordinator, \@segments, $query, $local,
			"the same answer as on the coordinator (attempt $attempt): $query");
	}
}

$_->stop foreach (@segments, $coordinator);

done_testing();
