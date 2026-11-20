# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A fragment of a slice that moves rows between the segments runs on a plain
# backend of a segment, with motions nested inside.  Without the statement
# snapshots attached to the fragment, each motion worker takes its own fresh
# READ COMMITTED snapshot, and a segment-local commit that lands between the
# scans of the fragment and its workers mixes both states into one answer:
# rows appear neither in the all-before nor in the all-after result.
#
# Park the pull workers of one segment before they open their transactions,
# change the joined rows with a segment-local commit, wake them: the answer
# of the parked statement must still be exactly the state before the commit,
# and the next statement must see the state after it.

use strict;
use warnings FATAL => 'all';
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
$coordinator->start;
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

my @segments;
foreach my $segment_id (1 .. 3)
{
	my $node = PostgreSQL::Test::Cluster->new("seg$segment_id");
	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
max_prepared_transactions = 10
");
	$node->start;
	push @segments, $node;
}
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 3 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

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

# The aggregate runs on the segments, moving the first table between them
my $query = q{SELECT o.orderkey, sum(l.quantity) FROM c JOIN o ON c.custkey = o.custkey JOIN l ON l.orderkey = o.orderkey GROUP BY o.orderkey ORDER BY o.orderkey};
like(plan_for($query), qr/GpSlice/,
	'the aggregate over the broadcast join runs as a slice');

# Both changed rows live on one segment: both tables distribute key value 1
# into the same bucket
my $segment_id = run(<<'SQL');
SELECT gp.bucket_segment(colocation_id, gp.bucket(1024, 1))
FROM gp.distribution_policy WHERE relid = 'c'::regclass
SQL
my $segment = $segments[$segment_id - 1];
$segment->safe_psql('postgres',
	"SET gp.allow_local_ddl = on;
CREATE EXTENSION injection_points");

my $expected_before = run($query);

# Park every pull worker of this segment before it opens its transaction
$segment->safe_psql('postgres',
	"SELECT injection_points_attach('gp-interconnect-fragment', 'wait')");

my ($stdin, $stdout, $stderr) = ('', '', '');
my $timer = IPC::Run::timer(300);
my $session = IPC::Run::start(
	['psql', '-X', '-A', '-t', '-d',
	 $coordinator->connstr('postgres') . ' application_name=gp_snapshot_hunt'],
	'<', \$stdin, '>', \$stdout, '2>', \$stderr, $timer);
$stdin .= "$query;\n";
$session->pump_nb while length $stdin;
$session->pump_nb for 1 .. 50;

# Three parents move the table, so wait for all three workers to park: a
# worker that reaches the point after the wakeups but before the detach
# parks forever
$segment->poll_query_until('postgres',
	"SELECT count(*) >= 3 FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-fragment'")
  or BAIL_OUT('three fragment workers did not park: '
	. $segment->safe_psql('postgres',
		"SELECT pid || ':' || coalesce(wait_event,'?') || ':' || state FROM pg_stat_activity")
	. " stdout=$stdout stderr=$stderr");

# A segment-local commit outside the DTX lock of the coordinator: every
# consistent answer of the parked statement is either all-before or all-after
$segment->safe_psql('postgres',
	"UPDATE c SET custkey = 999 WHERE custkey = 1;
UPDATE l SET quantity = quantity + 1000 WHERE orderkey = 1");

# injection_points_wakeup() releases exactly one waiter per call, so bump
# the point until none is left
for my $attempt (1 .. 20)
{
	my $parked = $segment->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'gp interconnect request' AND wait_event = 'gp-interconnect-fragment'");
	last if $parked == 0;
	$segment->safe_psql('postgres',
		"SELECT injection_points_wakeup('gp-interconnect-fragment');
SELECT pg_sleep(0.5)");
}
$segment->safe_psql('postgres',
	"SELECT injection_points_detach('gp-interconnect-fragment')");
$session->pump
  until $stdout ne '' || $stderr =~ /ERROR/ || $timer->is_expired;
my $expected_after = run($query);

is($stderr, '', 'the parked aggregate query has no error');
is($stdout, $expected_before . "\n",
	'the parked aggregate sees only the state before the segment-local commit');
isnt($stdout, $expected_after . "\n",
	'its answer is not the state after the commit either');

$session->finish;

# The machinery recovers: the next statement sees the committed rows
is(run($query), $expected_after,
	'the next statement sees the segment-local commit');

$_->stop foreach (@segments, $coordinator);

done_testing();
