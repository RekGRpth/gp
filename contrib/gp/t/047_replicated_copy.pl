# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A replicated table keeps a copy on the coordinator: every write reaches
# the segments and the copy beside, and every read of such a table takes
# the copy alone, without a word to the segments.  A query over the tables
# that keep their rows on the coordinator leaves the dispatch counter
# unchanged

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

run('CREATE TABLE ref (k int, v text)');
run("SELECT gp.distribute('ref', NULL)");
run('CREATE TABLE d (a int)');
run("SELECT gp.distribute('d', ARRAY['a'])");
run('CREATE TABLE loc (a int)');
run("INSERT INTO ref SELECT g, 'v' || g FROM generate_series(1, 9) g");

# The copy of the coordinator and the copies of the segments agree
is(run('SELECT count(*) FROM ref'), 9, 'the copy of the coordinator has the rows');
is(run("SELECT string_agg(v, ',' ORDER BY v) FROM ref"),
	"v1,v2,v3,v4,v5,v6,v7,v8,v9", 'and the rows are the routed ones');

# Reads of the reference tables dispatch nothing
{
	my $before = run('SELECT gp.dispatches()');
	run('SELECT * FROM ref ORDER BY k');
	run('SELECT count(*) FROM ref');
	run('SELECT count(*) FROM loc');
	run('SELECT r1.v, l.a FROM ref r1 JOIN loc l ON l.a = 1 ORDER BY r1.v');
	is(run('SELECT gp.dispatches()'), $before,
		'queries over the tables of the coordinator dispatch nothing');
}

# A write to a distributed table dispatches
{
	my $before = run('SELECT gp.dispatches()');
	run('INSERT INTO d VALUES (0)');
	isnt(run('SELECT gp.dispatches()'), $before,
		'a write to a distributed table dispatches');
}

# INSERT, UPDATE and DELETE keep the copy with the segments
run("INSERT INTO ref VALUES (10, 'v10')");
is(run('SELECT count(*) FROM ref'), 10, 'an insert reaches the copy');
is(run("SELECT string_agg(v, ',' ORDER BY k) FROM ref"),
	"v1,v2,v3,v4,v5,v6,v7,v8,v9,v10", 'and the row lands in order');
run('UPDATE ref SET v = \'w\' || k WHERE k > 8');
is(run("SELECT string_agg(v, ',' ORDER BY k) FROM ref"),
	"v1,v2,v3,v4,v5,v6,v7,v8,w9,w10", 'an update reaches the copy');
run('DELETE FROM ref WHERE k = 10');
is(run('SELECT count(*) FROM ref'), 9, 'a delete reaches the copy');

# A rollback undoes the copy beside the segments: the block runs in one
# session, for every run() of its own is a session of its own
is(run("BEGIN;
	INSERT INTO ref VALUES (11, 'v11');
	SELECT count(*) FROM ref;
	ROLLBACK;
	SELECT count(*) FROM ref;"), "10
9",
	'a rollback removes the row from the copy and the segments');

# COPY writes the copy beside the segments: the rows follow the command
# in the stream that psql reads
run("COPY ref FROM STDIN;
"
    . "12\tv12\n"
    . "13\tv13\n");
is(run('SELECT count(*) FROM ref'), 11, 'a copy reaches the rows');
is(run('SELECT count(*) FROM (SELECT DISTINCT v FROM ref) x'), 11,
	'and no row is copied twice');

# A join of a replicated and a distributed table still agrees
run('INSERT INTO d SELECT k FROM ref');
is(run('SELECT count(*) FROM ref r JOIN d ON d.a = r.k'),
	run('SELECT count(*) FROM ref'), 'a join with a distributed table');

# The segments have their own copies still
{
	my $on_segments = join ' ', map { $_->safe_psql('postgres', 'SELECT count(*) FROM ref') }
		@segments;
	is($on_segments, '11 11 11', 'the segments have every row as well');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
