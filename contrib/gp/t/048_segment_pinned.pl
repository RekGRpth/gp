# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A table pinned to a segment keeps every row on that segment alone: the
# buckets of its own colocation group all point there, the reads gather
# from it alone, and the writes reach it by the ordinary routing.  The
# pin is refused without a registered segment, without a key, and with a
# colocation group of another

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub on_segments { my ($q) = @_; return join ' ', map { $_->safe_psql('postgres', $q) } @segments; }

run('CREATE TABLE p (k int, v text)');
run('SELECT gp.distribute(\'p\', ARRAY[\'k\'], segment => 2)');
run('INSERT INTO p SELECT g, \'p\' || g FROM generate_series(1, 12) g');

# The rows live on the pinned segment alone
is(on_segments('SELECT count(*) FROM p'), '0 12 0',
	'the rows of a pinned table live on its segment alone');
is(run('SELECT pg_relation_size(\'p\')'), 0,
	'the coordinator keeps no rows of a pinned table');

# The reads and the writes work over the coordinator
is(run('SELECT count(*), min(k), max(k) FROM p'), '12|1|12',
	'a read of a pinned table gathers its segment');
run('UPDATE p SET v = \'x\' || k WHERE k = 3');
is(run('SELECT v FROM p WHERE k = 3'), 'x3', 'an update reaches the pin');
run('DELETE FROM p WHERE k = 12');
is(run('SELECT count(*) FROM p'), 11, 'a delete reaches the pin');
run("INSERT INTO p VALUES (12, 'p12')");
is(run('SELECT count(*) FROM p'), 12, 'an insert reaches the pin');

# A join with a distributed table agrees with the coordinator's answer
run('CREATE TABLE d (a int, w text)');
run("SELECT gp.distribute('d', ARRAY['a'])");
run('INSERT INTO d SELECT g, \'d\' || g FROM generate_series(2, 20) g');
run('CREATE TABLE lc AS SELECT * FROM p');
run('CREATE TABLE ld AS SELECT * FROM d');
is(run('SELECT count(*) FROM p JOIN d ON d.a = p.k'),
	run('SELECT count(*) FROM lc JOIN ld ON ld.a = lc.k'),
	'a join of a pinned and a distributed table agrees');

# The pin is refused without its preconditions
run('CREATE TABLE q (k int)');
eval { $coordinator->safe_psql('postgres',
	'SELECT gp.distribute(\'q\', ARRAY[\'k\'], segment => 99)'); };
like($@, qr/is not registered/, 'an unknown segment is refused');
eval { $coordinator->safe_psql('postgres',
	'SELECT gp.distribute(\'q\', NULL, segment => 2)'); };
like($@, qr/cannot be pinned/, 'a table without a key is refused');
eval { $coordinator->safe_psql('postgres',
	'SELECT gp.distribute(\'q\', ARRAY[\'k\'], colocate_with => \'p\', segment => 2)'); };
like($@, qr/joins no colocation group/, 'colocate_with refuses a pin');

$_->stop foreach (@segments, $coordinator);

done_testing();
