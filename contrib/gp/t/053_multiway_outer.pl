# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A chain of base tables joined by one kind of an outer join keeps the
# rows its every join keeps: a chain of LEFT JOINs stays on the segments
# with the first table and broadcasts the rest, a chain of FULL JOINs
# broadcasts everything to one segment, and the answers match the
# coordinator.  A mixed level keeps the rows of its first table the way
# every join of the level does, so it rides to the segments beside the
# chain, and so does an EXISTS or a NOT EXISTS of the level, correlated
# included: their tables broadcast whole.  A level with a FULL join keeps
# the rows of both sides and stays on the coordinator, and so does an
# aggregate by a column of a null-supplying table

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

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
# partners cannot carry an aggregate, and a mixed level keeps the rows of
# the first table the way every join of the level does
{
	my $nullable = 'SELECT c.custkey, count(*) FROM o LEFT JOIN c ON o.custkey = c.custkey LEFT JOIN l ON l.orderkey = o.orderkey GROUP BY c.custkey ORDER BY c.custkey';
	my $nullable_local = 'SELECT lc.custkey, count(*) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey LEFT JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lc.custkey ORDER BY lc.custkey';

	is(run($nullable), run($nullable_local),
		'an aggregate by a null-supplying column is still right');
	my $mixed = 'SELECT o.orderkey, sum(l.quantity) FROM o LEFT JOIN c ON o.custkey = c.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey';
	my $mixed_local = 'SELECT lo.orderkey, sum(ll.quantity) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey JOIN ll ON ll.orderkey = lo.orderkey GROUP BY lo.orderkey ORDER BY lo.orderkey';

	is(run($mixed), run($mixed_local),
		'a mixed level is still right');
	like(plan_for($mixed), qr/GpSlice/,
		'and rides to the segments with the first table');
}

# An EXISTS, a NOT EXISTS or a correlated EXISTS of the level filters the
# rows of the first table: its tables broadcast whole, so the segments
# answer the same filter the coordinator answers
{
	my $exists = 'SELECT count(*) FROM o LEFT JOIN c ON o.custkey = c.custkey WHERE EXISTS (SELECT 1 FROM l WHERE l.orderkey = o.orderkey)';
	my $exists_local = 'SELECT count(*) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey WHERE EXISTS (SELECT 1 FROM ll WHERE ll.orderkey = lo.orderkey)';
	my $anti = 'SELECT count(*) FROM o LEFT JOIN c ON o.custkey = c.custkey WHERE NOT EXISTS (SELECT 1 FROM l WHERE l.orderkey = o.orderkey)';
	my $anti_local = 'SELECT count(*) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey WHERE NOT EXISTS (SELECT 1 FROM ll WHERE ll.orderkey = lo.orderkey)';
	my $corr = 'SELECT count(*) FROM o LEFT JOIN c ON o.custkey = c.custkey WHERE EXISTS (SELECT 1 FROM l WHERE l.orderkey = o.orderkey AND l.quantity > o.custkey)';
	my $corr_local = 'SELECT count(*) FROM lo LEFT JOIN lc ON lo.custkey = lc.custkey WHERE EXISTS (SELECT 1 FROM ll WHERE ll.orderkey = lo.orderkey AND ll.quantity > lo.custkey)';

	is(run($exists), run($exists_local),
		'an EXISTS of the level answers over the segments');
	like(plan_for($exists), qr/GpSlice/,
		'an EXISTS rides to the segments with the first table');
	is(run($anti), run($anti_local),
		'a NOT EXISTS answers over the segments');
	like(plan_for($anti), qr/GpSlice/,
		'a NOT EXISTS rides to the segments too');
	is(run($corr), run($corr_local),
		'a correlated EXISTS answers over the segments');
	like(plan_for($corr), qr/GpSlice/,
		'a correlated EXISTS rides with its tables broadcast');
}

# A FULL join of the level keeps the rows of both sides: no segment sees
# either side whole, so the level stays on the coordinator
{
	my $fullmixed = 'SELECT count(*) FROM o FULL JOIN c ON o.custkey = c.custkey JOIN l ON l.orderkey = o.orderkey';
	my $fullmixed_local = 'SELECT count(*) FROM lo FULL JOIN lc ON lo.custkey = lc.custkey JOIN ll ON ll.orderkey = lo.orderkey';

	is(run($fullmixed), run($fullmixed_local),
		'a mixed level with a FULL join is still right');
	unlike(plan_for($fullmixed), qr/GpSlice/,
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
