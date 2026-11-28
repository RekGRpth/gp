# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The gate of the writes of a move: a write of a bucket takes the
# advisory lock of its bucket, and the move of the bucket waits for it
# while the writes of the other buckets answer; a write that runs on the
# segments may touch any bucket of the group, and takes the lock of the
# whole group; the move itself takes the exclusive locks of its bucket
# and of the group, and waits for every holder of them.  The writes of
# the other buckets answer under the copy of a move too, while the
# window of the switch waits for their transactions to end

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

# The group of the tables and the bucket of a key are visible in SQL,
# by the same advisory locks the gate takes
my $group = run("SELECT colocation_id FROM gp.distribution_policy "
				. "WHERE relid = 't'::regclass");
my $bk = run('SELECT gp.bucket(1024, 500)');
my $other = ($bk + 1) % 1024;

# A routed insert holds the gate of its bucket: the move of the bucket
# waits for it
my $writer = $coordinator->background_psql('postgres', on_error_stop => 1);
$writer->query("BEGIN;\nINSERT INTO t VALUES (500, 'w');\n");
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', $bk, 2)");
};
like($@, qr/lock timeout/,
	'the move of the bucket of a routed insert waits for it');

# A write that runs on the segments may touch any bucket of the group,
# and holds the whole group: the move of any bucket waits for it
$writer->query("UPDATE t SET b = 'u' WHERE a <= 30;\n");
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', $other, 2)");
};
like($@, qr/lock timeout/,
	'the move of any bucket waits for a write that runs on the segments');
$writer->query("ROLLBACK;\n");
$writer->quit;

# The tables came through the writes whole
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the rolled-back writes left the whole as it was');

# The move takes the exclusive locks of the gate: a shared holder of the
# bucket makes the move of it wait, and the moves of the others answer
my $holder = $coordinator->background_psql('postgres', on_error_stop => 1);
$holder->query("BEGIN;\nSELECT pg_advisory_xact_lock_shared($group, $bk);\n");
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', $bk, 2)");
};
like($@, qr/lock timeout/, 'the move waits for a shared holder of its bucket');
run("SET lock_timeout = '3s'; SELECT gp.move_bucket('t', $other, 2)");
pass('the move of another bucket answers beside a holder of one bucket');

# A shared holder of the whole group makes every move of it wait.  The
# move asks for a segment the bucket is not on: a move to the segment
# the map already names stays put before the locks
$holder->query("SELECT pg_advisory_xact_lock_shared($group, -1);\n");
eval {
	run("SET lock_timeout = '1s'; SELECT gp.move_bucket('t', $other, 3)");
};
like($@, qr/lock timeout/, 'the move waits for a holder of the whole group');
$holder->query("ROLLBACK;\n");
$holder->quit;

# Under the copy of a move the writes of the other buckets answer, and
# the window of the switch waits for their transaction to end
if ($ENV{enable_injection_points} eq 'yes')
{
	run("CREATE EXTENSION injection_points;\n"
	   . "SELECT injection_points_attach('gp-move-copied', 'wait')");
	my $mover = $coordinator->background_psql('postgres', on_error_stop => 1);
	$mover->query_until(qr/go/,
		"\\echo go\nSET statement_timeout = '120s';\n"
		. "SELECT gp.move_bucket('t', $other, 3);\n");
	$coordinator->wait_for_event('client backend', 'gp-move-copied');

	# A key whose bucket takes no part in the move
	my $free = 501;
	$free++ while run("SELECT gp.bucket(1024, $free)") == $other;

	my $copier = $coordinator->background_psql('postgres', on_error_stop => 1);
	$copier->query("BEGIN;\nINSERT INTO t VALUES ($free, 'c');\n");
	pass('a write of another bucket answers under the copy of a move');
	$copier->query("ROLLBACK;\n");
	$copier->quit;

	run("SELECT injection_points_detach('gp-move-copied');\n"
		. "SELECT injection_points_wakeup('gp-move-copied');\n");
	$mover->quit;
	is(run('SELECT sum(a), count(*) FROM t'), '465|30',
		'and the move after it keeps the truth of the whole');
}
else
{
	pass('injection points are not supported by this build');
	pass('injection points are not supported by this build');
}

# The moves after the holders left answer, and the whole stays whole
run("SET lock_timeout = '3s'; SELECT gp.move_bucket('t', $bk, 2)");
run("SET lock_timeout = '3s'; SELECT gp.move_bucket('u', $bk, 2)");
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the move after the holders keeps the truth of the whole');
is(run('SELECT sum(d), count(*) FROM u'), '697.5|30',
	'and the second table of the group moved with it');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
