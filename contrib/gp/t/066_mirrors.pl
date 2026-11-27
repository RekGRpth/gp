# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The mirror of a segment: a physical standby kept by the replication of
# the server itself.  The coordinator watches the pairs, and a primary
# that stops answering hands the identity of its segment to its mirror:
# the mirror promotes, takes the extension of the segments, and takes the
# address of the segment in the copy, so the sessions of the cluster go
# on with the data of the segment on the promoted server

use strict;
use warnings FATAL => 'all';
use File::Basename qw(basename);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster(
	segment_conf => "max_wal_senders = 4\nwal_level = replica\n");
my @mirrors;

sub run { return $coordinator->safe_psql('postgres', shift); }

# The table and its rows, distributed over the primaries, before the
# mirrors exist: every mirror then starts from a backup that holds the
# whole table already
run('CREATE TABLE t (a int, b text)');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g)");
is(run('SELECT sum(a) FROM t'), 465, 'the rows are in the cluster');

# A mirror of every segment: a copy of the data directory of the primary,
# started as a standby of it.  The standby runs with the extension idle
# (gp.role = none), for a recovery server reads no writes of its own
foreach my $segment (@segments)
{
	my $id = $segment->safe_psql('postgres',
		'SELECT current_setting(\'gp.segment_id\')');
	my $mirror = PostgreSQL::Test::Cluster->new("mirror-$id",
												port => $segment->port + 50);
	$segment->backup('segbackup');
	$mirror->init_from_backup($segment, 'segbackup',
							  has_streaming => 1);
	$mirror->append_conf('postgresql.conf',
		"port = " . $mirror->port . "\n" .
		"shared_preload_libraries = 'gp'\n" .
		"gp.role = 'none'\n" .
		"gp.segment_id = $id\n" .
		"gp.coordinator = 'host=" . $segment->host . " port=" .
		$segment->port . "'\n" .
		"wal_receiver_timeout = 2s\n");
	$mirror->start;
	run("SELECT gp.register_mirror($id, '" .
		$mirror->host . "', " . $mirror->port . ")");
	push(@mirrors, $mirror);
}

is(run('SELECT count(*) FROM gp.mirrors'), scalar(@segments),
	'the mirrors registered');

# The mirrors have caught up with their primaries: a switch of the WAL
# marks a point every standby reaches within the wait
run('SELECT pg_switch_wal()');
foreach my $mirror (@mirrors)
{
	$mirror->poll_query_until('postgres',
		'SELECT pg_is_in_recovery() AND ' .
		'pg_last_wal_replay_lsn() IS NOT NULL') or die 'mirrors did not catch up';
}

# The share of the dead segment, read on its mirror beforehand: a mirror
# replicates its primary, and its primary keeps its own share of the rows
my $seg1_share = $segments[0]->safe_psql('postgres', 'SELECT count(*) FROM t');

# The primary of segment 1 stops answering; the pass of the recovery
# promotes its mirror within the interval
my $dead = $segments[0];
$dead->stop('immediate');

my $promoted;
foreach my $wait (1 .. 40)
{
	sleep(1);
	my $row = run('SELECT port FROM gp.segments WHERE segment_id = 1');
	my $mirror_port = $segments[0]->port + 50;
	if ($row eq $mirror_port)
	{
		$promoted = $mirror_port;
		last;
	}
}
ok(defined $promoted, 'the mirror took the segment within the interval');

# The cluster answers with the data of the segment on the promoted server
is(run('SELECT sum(a) FROM t'), 465, 'the data of the segment is whole');
run("INSERT INTO t VALUES (100, 'after')");
is(run('SELECT b FROM t WHERE a = 100'), 'after',
	'and the writes land on the promoted server');
my $newrow = PostgreSQL::Test::Cluster->new("promoted-1",
											port => $promoted);
is($newrow->safe_psql('postgres',
		"SELECT count(*) FROM pg_extension WHERE extname = 'gp'"), 1,
	'the promoted server took the extension');
is($newrow->safe_psql('postgres', 'SELECT count(*) FROM t'),
	$seg1_share, 'its rows are the ones of the segment');

# The other segments and the mirror rows of the whole pair
is(run('SELECT count(*) FROM gp.mirrors WHERE segment_id = 1'), 0,
	'the row of the mirror is gone with the promotion');
is(run('SELECT count(*) FROM gp.mirrors'), scalar(@segments) - 1,
	'and the mirrors of the live primaries stay');

$coordinator->stop;
foreach my $segment (@segments[1 .. $#segments])
{
	$segment->stop;
}
done_testing();
