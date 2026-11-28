# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# With cluster statistics, a two-way inner join moves its smaller side.  This
# works in either written order, both for redistribution and for broadcast

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (VERBOSE, COSTS OFF) $_[0]"); }

run('CREATE TABLE big (a int, v text)');
run("SELECT gp.distribute('big', ARRAY['a'])");
run("INSERT INTO big SELECT i, 'b' || i FROM generate_series(1, 100) i");
run('CREATE TABLE small (a int, v text)');
run("SELECT gp.distribute('small', ARRAY['a'])");
run("INSERT INTO small SELECT i, 's' || i FROM generate_series(1, 10) i");

# Keep the key types equal, but put small under another bucket map
run(<<'SQL');
INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
SELECT key_types, nbuckets,
       (SELECT array_agg(map[(i % 1024) + 1] ORDER BY i)
        FROM generate_series(1, 1024) i)
FROM gp.colocation_groups
WHERE colocation_id = (SELECT colocation_id FROM gp.distribution_policy
                       WHERE relid = 'big'::regclass)
SQL
run("UPDATE gp.distribution_policy SET colocation_id = (SELECT max(colocation_id) FROM gp.colocation_groups) WHERE relid = 'small'::regclass");
run("SELECT gp.policy_changed('small')");

# The rows of small take the places of the turned map, the way the moves
# of the buckets would place them
run('DELETE FROM small');
run("INSERT INTO small SELECT i, 's' || i FROM generate_series(1, 10) i");
run('CREATE TABLE lb AS SELECT * FROM big');
run('CREATE TABLE ls AS SELECT * FROM small');
run('ANALYZE big, small');

sub moves_smaller
{
	my ($query, $table, $description) = @_;

	like(plan_for($query), qr/::text\) \Q$table\E\(/, $description);
}

moves_smaller('SELECT big.a, small.v FROM big JOIN small ON big.a = small.a',
	'small', 'redistribution moves the smaller second table');
moves_smaller('SELECT big.a, small.v FROM small JOIN big ON small.a = big.a',
	'small', 'redistribution moves the smaller first table too');
moves_smaller('SELECT big.a, small.v FROM big JOIN small ON big.a + 0 = small.a',
	'small', 'broadcast moves the smaller second table');
moves_smaller('SELECT big.a, small.v FROM small JOIN big ON small.a + 0 = big.a',
	'small', 'broadcast moves the smaller table in either order');
moves_smaller('SELECT big.a, small.v FROM small CROSS JOIN big',
	'small', 'a cross join broadcasts the smaller table');
moves_smaller('SELECT small.a, big.v FROM small LEFT JOIN big ON small.a = big.a',
	'big', 'an outer join still broadcasts its null-supplying side');
like(plan_for('SELECT big.a, count(*) FROM big JOIN small ON big.a + 0 = small.a GROUP BY big.a'),
	qr/GpSlice/, 'an aggregate by the stationary side remains local');
like(plan_for('SELECT small.a, count(*) FROM big JOIN small ON big.a + 0 = small.a GROUP BY small.a'),
	qr/GpSlice/, 'an aggregate by the broadcast side is redistributed');

run('SET enable_mergejoin = off');
my @queries = (
	'SELECT big.a, small.v FROM big JOIN small ON big.a = small.a ORDER BY big.a, small.v',
	'SELECT big.a, small.v FROM small JOIN big ON small.a = big.a ORDER BY big.a, small.v',
	'SELECT big.a, small.v FROM big JOIN small ON big.a + 0 = small.a ORDER BY big.a, small.v',
	'SELECT big.a, small.v FROM small JOIN big ON small.a + 0 = big.a ORDER BY big.a, small.v',
	'SELECT big.a, small.v FROM small CROSS JOIN big ORDER BY big.a, small.v',
	'SELECT small.a, big.v FROM small LEFT JOIN big ON small.a = big.a ORDER BY small.a, big.v',
	'SELECT big.a, count(*) FROM big JOIN small ON big.a + 0 = small.a GROUP BY big.a ORDER BY big.a',
);
foreach my $query (@queries)
{
	my $local = $query;
	$local =~ s/\bbig\b/lb/g;
	$local =~ s/\bsmall\b/ls/g;
	is(run($query), run($local), "the same answer as on the coordinator: $query");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
