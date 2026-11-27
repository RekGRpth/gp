# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The cluster expands: a new segment starts from a copy of the
# coordinator -- the copy holds the right catalog, the empty local
# copies of the distributed tables, and the whole of the replicated
# ones -- registers, and takes its share of the buckets.  The rows of
# the coordinator's own share of the coordinator-share tables truncate
# on the new segment, for they belong to the coordinator alone.  After
# the expansion the queries of the cluster answer the truth of the
# whole, and the writes land on the owners of their buckets

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster(
	coordinator_conf => "max_wal_senders = 4\nwal_level = replica\n",
	segment_conf => "max_wal_senders = 4\nwal_level = replica\n");

sub run { return $coordinator->safe_psql('postgres', shift); }

# The tables of every kind, with rows, before the new segment joins
run(<<'DDL');
CREATE TABLE t (a int, b text);
SELECT gp.distribute('t', ARRAY['a']);
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g);
CREATE TABLE cs (a int, b text);
SELECT gp.distribute('cs', ARRAY['a'], NULL, NULL, true);
INSERT INTO cs SELECT g, 'c' || g FROM generate_series(1, 20) gs(g);
CREATE TABLE r (a int);
SELECT gp.distribute('r', NULL);
INSERT INTO r VALUES (1), (2);
DDL

# The new segment starts from a copy of the coordinator
$coordinator->backup('coordbackup');
my $new = PostgreSQL::Test::Cluster->new('segment-7',
										 port => $coordinator->port + 60);
$new->init_from_backup($coordinator, 'coordbackup');
$new->append_conf('postgresql.conf',
	"port = " . $new->port . "\n" .
	"max_prepared_transactions = 100\n" .
	"shared_preload_libraries = 'gp'\n" .
	"gp.role = 'segment'\n" .
	"gp.segment_id = 7\n" .
	"gp.coordinator = 'host=" . $coordinator->host . " port=" .
	$coordinator->port . "'\n" .
	"gp.host = '" . $new->host . "'\n");
$new->start;

# The register worker of the new segment registers it at startup
$new->poll_query_until('postgres',
	"SELECT EXISTS (SELECT FROM pg_extension WHERE extname = 'gp')")
	or die 'the new segment took no extension';

my $registered = 0;
foreach my $wait (1 .. 30)
{
	$registered = run('SELECT count(*) FROM gp.cluster_segments()');
	last if $registered == 4;
	sleep(1);
}
is($registered, 4, 'the new segment registered');

# The expansion: the garbage of the coordinator's share truncates on the
# new segment, and the buckets of every hash group spread over the
# segments
ok(run('SELECT gp.expand_segment(7)') >= 1,
	'the expansion moves the buckets to the new segment');

# The queries of the cluster answer the truth of the whole
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the distributed rows are whole after the expansion');
is(run('SELECT sum(a), count(*) FROM cs'), '210|20',
	'the coordinator-share rows answer once, not twice');
is(run('SELECT count(*) FROM r'), 2,
	'the replicated table answers once');
is(run("SELECT count(*) FROM cs WHERE b = 'c3'"), 1,
	'and the coordinator-share rows stay whole');

# A write after the expansion lands by its bucket, and the whole
# answers
run("INSERT INTO t VALUES (500, 'after')");
is(run('SELECT sum(a) FROM t'), 465 + 500,
	'the cluster adds the new write to the whole');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
$new->stop;
done_testing();
