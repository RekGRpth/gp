# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A move of a bucket copies while the readers answer: the rows of the
# copy wait uncommitted on the new segment, and the cluster reads the
# old side, once each.  Then a short window holds the readers while the
# copy commits, the map switches, and the rows go away from the old
# segment.  The writers wait at the locks of the gate: a
# share-row-exclusive holder of the table, and an uncommitted write of
# the moving bucket, each makes the move wait

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

# The copy goes under an open reader, and the window of the switch
# waits for the reader's transaction to end
if ($ENV{enable_injection_points} eq 'yes')
{
	run("CREATE EXTENSION injection_points;\n"
	   . "SELECT injection_points_attach('gp-move-copied', 'wait')");
	my $reader = $coordinator->background_psql('postgres', on_error_stop => 1);
	$reader->query("BEGIN;\nDECLARE c CURSOR FOR SELECT * FROM t;\n"
				   . "FETCH 1 FROM c;\n");
	my $mover = $coordinator->background_psql('postgres', on_error_stop => 1);
	$mover->query_until(qr/go/, "\\echo go\nSET statement_timeout = '120s';\n"
		. "SELECT gp.move_bucket('t', 0, 3);\n");
	$coordinator->wait_for_event('client backend', 'gp-move-copied');
	is(run('SELECT sum(a), count(*) FROM t'), '465|30',
		'the cluster reads the old side once while the copy waits');
	my $seen = $reader->query("FETCH ALL FROM c");
	is(scalar(grep { length } split(/\n/, $seen)), 29,
		'the open cursor of the reader keeps its rows through the copy');
	$reader->query("ROLLBACK;\n");
	$reader->quit;
	run("SELECT injection_points_detach('gp-move-copied');\n"
		. "SELECT injection_points_wakeup('gp-move-copied');\n");
	$mover->quit;
}
else
{
	run("SELECT gp.move_bucket('t', 0, 3)");
	pass('injection points are not supported by this build');
	pass('injection points are not supported by this build');
}

# The rows moved: the map names the new segment, and the answers of the
# cluster are the ones of the whole
is(run("SELECT map[1] FROM gp.colocation_groups g "
	   . "JOIN gp.distribution_policy p USING (colocation_id) "
	   . "WHERE p.relid = 't'::regclass"), 3,
	'the map names the new segment');
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the cluster answers the truth of the whole');

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

# An uncommitted write holds the gate of its bucket, and the move of
# that bucket waits for it: the row it wrote before the copy would be
# lost by the switch
my $writer = $coordinator->background_psql('postgres', on_error_stop => 1);
$writer->query("BEGIN;\nINSERT INTO t VALUES (500, 'pending');\n");
my $wbucket = run('SELECT gp.bucket(1024, 500)');
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', $wbucket, 2)");
};
like($@, qr/lock timeout/, 'an uncommitted write makes the move of its bucket wait');
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
