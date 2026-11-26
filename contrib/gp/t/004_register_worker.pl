# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A segment registers itself on the coordinator in the background, retrying
# until the coordinator is up and has the extension

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");

sub new_segment
{
	my ($segment_id) = @_;
	my $port = PostgreSQL::Test::Cluster::get_free_port();
	my $node = PostgreSQL::Test::Cluster->new("segment-$segment_id-$port",
		port => $port);

	$node->init;
	$node->append_conf(
		'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=${\$coordinator->host} port=${\$coordinator->port}'
gp.host = '${\$node->host}'
gp.register_retry_interval = 1
");
	return $node;
}

sub segments
{
	return $coordinator->safe_psql('postgres',
		'SELECT segment_id, host, port FROM gp.segments ORDER BY 1');
}

# Segments start before the coordinator
my @segments = (new_segment(1), new_segment(2));
$_->start foreach @segments;
my ($seg1, $seg2) = @segments;

$seg1->wait_for_log(qr/could not connect to the coordinator/);
pass('a segment retries while the coordinator is down');

$coordinator->start;
$seg1->wait_for_log(
	qr/could not register as segment 1 on the coordinator: .*does not exist/);
pass('a segment retries while the coordinator has no extension');

$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.segments')
  or die 'segments did not register';
is( segments(),
	join("\n",
		map { $_ + 1 . '|' . $segments[$_]->host . '|' . $segments[$_]->port }
		  0 .. 1),
	'both segments registered');
$seg1->wait_for_log(
	qr/registered as segment 1 on the coordinator at \Q${\$seg1->host}\E:${\$seg1->port}/
);
pass('a segment logs its registration');

# The worker exits for good once the segment is registered
$seg1->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'gp register segment'"
) or die 'the worker is still running';
pass('the worker exits after the registration');

# A restarted segment registers again
my $before = $coordinator->safe_psql('postgres',
	'SELECT registered FROM gp.segments WHERE segment_id = 1');
$seg1->restart;
$coordinator->poll_query_until('postgres',
	"SELECT registered > '$before' FROM gp.segments WHERE segment_id = 1")
  or die 'segment 1 did not register again';
pass('a restarted segment registers again');

# The extension of the coordinator is recreated: a restarting segment
# drops its copy with the OIDs of the older creation and creates it again
# with the OIDs the coordinator has now
$coordinator->safe_psql('postgres', 'DROP EXTENSION gp');
$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');
my $coordinator_ext_oid = $coordinator->safe_psql('postgres',
	'SELECT oid FROM pg_extension WHERE extname = \'gp\'');
my $stale_ext_oid = $seg1->safe_psql('postgres',
	'SELECT oid FROM pg_extension WHERE extname = \'gp\'');
isnt($stale_ext_oid, $coordinator_ext_oid,
	'the recreation left the segment with stale OIDs');
$_->restart foreach @segments;
foreach my $segment (@segments)
{
	$segment->poll_query_until('postgres',
		"SELECT oid = $coordinator_ext_oid FROM pg_extension WHERE extname = 'gp'")
	  or die 'the segment did not reconcile its extension';
}
pass('a restarting segment recreates its extension with the OIDs of the coordinator');
is( $seg1->safe_psql('postgres', "SELECT 'gp.segments'::regclass::oid"),
	$coordinator->safe_psql('postgres', "SELECT 'gp.segments'::regclass::oid"),
	'the tables of the extension share the OIDs of the coordinator');
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.segments')
  or die 'the reconciled segments did not register again';
pass('a reconciled segment registers again');

# The coordinator has gp, but its OID file was lost: do not register a
# segment that would have no SQL functions with matching OIDs
my $oidfile = $coordinator->data_dir . '/gp_extension_oids';
rename $oidfile, "$oidfile.bak";
my $seg4 = new_segment(4);
$seg4->start;
$seg4->wait_for_log(qr/could not read extension OIDs on the coordinator/);
is( segments(),
	join("\n",
		map { $_ + 1 . '|' . $segments[$_]->host . '|' . $segments[$_]->port }
		  0 .. 1),
	'a missing OID file prevents registration');
$seg4->stop;
rename "$oidfile.bak", $oidfile;

# A segment with the number of another one does not take its place
my $seg3 = new_segment(1);
$seg3->start;
$seg3->wait_for_log(
	qr/could not register as segment 1 on the coordinator: .*segment 1 is already registered at/
);
is( segments(),
	join("\n",
		map { $_ + 1 . '|' . $segments[$_]->host . '|' . $segments[$_]->port }
		  0 .. 1),
	'a number registered at another address is refused');

$_->stop foreach ($seg3, @segments, $coordinator);

done_testing();
