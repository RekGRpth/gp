# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A move of a bucket holds the tables of the group against the writes of
# the cluster and lets the readers answer while the rows are in two
# places: the writers wait at the lock (a write that slipped past would
# be lost by the switch), a read keeps to the side of the switch its
# snapshot took, and no read doubles a row and no write loses one

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

run(<<'DDL');
CREATE TABLE t (a int, b text);
CREATE TABLE u (a int, d numeric);
SELECT gp.distribute('t', ARRAY['a']);
SELECT gp.distribute('u', ARRAY['a'], 't');
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g);
INSERT INTO u SELECT g % 4, g * 1.5 FROM generate_series(1, 30) gs(g);
DDL

# A reader with an open cursor holds AccessShare and does not block the
# move: an AccessExclusive move would wait for it
my $reader = $coordinator->background_psql('postgres', on_error_stop => 1);
$reader->query("BEGIN;\nDECLARE c CURSOR FOR SELECT * FROM t;\nFETCH 1 FROM c;\n");
like(run("SET lock_timeout = '3s'; SELECT gp.move_bucket('t', 0, 3)"), qr/^\d+$/,
	'the move answers while a reader holds the table');

# The rows moved: the map names the new segment, and the answers of the
# cluster are the ones of the whole
is(run("SELECT map[1] FROM gp.colocation_groups g "
	   . "JOIN gp.distribution_policy p USING (colocation_id) "
	   . "WHERE p.relid = 't'::regclass"), 3,
	'the map names the new segment');
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the cluster answers the truth of the whole');

# A read keeps to the side of the switch its snapshot took: the cursor
# still reads the rows of the old segment, the delete of the old copy is
# invisible to its snapshot, and a fresh statement of the same session
# reads by the new map.  The fetch of the first row left the cursor 29
# rows before it, and they are all still there after the move
my $seen = $reader->query("FETCH ALL FROM c");
is(scalar(grep { length } split(/\n/, $seen)), 29,
	'the open cursor of the reader keeps its rows through the switch');
is($reader->query('SELECT count(*) FROM t'), 30,
	'and a fresh read of the session answers by the new map');
$reader->query("ROLLBACK;\n");
$reader->quit;

# A share-row-exclusive holder of the table makes the move wait: the
# mover requests a lock the writers' own conflicts with
my $locker = $coordinator->background_psql('postgres', on_error_stop => 1);
$locker->query("BEGIN;\nLOCK TABLE t IN SHARE ROW EXCLUSIVE MODE;\n");
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', 0, 2)");
};
like($@, qr/lock timeout/, 'a share-row-exclusive holder makes the move wait');
$locker->query("ROLLBACK;\n");
$locker->quit;

# An uncommitted write holds RowExclusive and makes the move wait too:
# the rows it wrote before the copy would be lost by the switch
my $writer = $coordinator->background_psql('postgres', on_error_stop => 1);
$writer->query("BEGIN;\nINSERT INTO t VALUES (500, 'pending');\n");
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', 0, 2)");
};
like($@, qr/lock timeout/, 'an uncommitted write makes the move wait');
$writer->query("ROLLBACK;\n");
$writer->quit;
is(run('SELECT count(*) FROM t'), 30,
	'the waiting move changed nothing');

# The move answers after the waiters left, and the whole stays whole
run("SET lock_timeout = '3s'; SELECT gp.move_bucket('t', 0, 2)");
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the move after the waiters keeps the truth of the whole');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
