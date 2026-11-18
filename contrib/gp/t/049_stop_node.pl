# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A node stops without ssh: the superuser asks the coordinator, which
# dispatches the stop to the segment, and the segment sends the fast
# shutdown to its own server.  The segment comes back with its own
# start and registers again.  The coordinator refuses to stop itself
# this way, and the functions are for the superuser alone

use strict;
use warnings FATAL => 'all';
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
	'SELECT count(*) = 3 FROM gp.cluster_segments()')
  or die 'segments did not register';

sub run { return $coordinator->safe_psql('postgres', shift); }
sub on_segments { my ($q) = @_; return join ' ', map { $_->safe_psql('postgres', $q) } @segments; }

run('CREATE TABLE p (k int, v text)');
run("SELECT gp.distribute('p', ARRAY['k'], segment => 1)");
run('INSERT INTO p SELECT g, \'p\' || g FROM generate_series(1, 5) g');
is(on_segments('SELECT count(*) FROM p'), '5 0 0',
	'the rows of a pinned table live on its segment alone');

# The refusals
{
	my ($ret, $stdout, $stderr);
	$ret = $coordinator->psql('postgres',
		'SELECT gp.stop_segment(0)',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/the coordinator has no number/,
		'the coordinator is not stopped by its own number');
	run('CREATE ROLE someone LOGIN');
	$ret = $coordinator->psql('postgres',
		'SET ROLE someone; SELECT gp.stop_segment(1)',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/(only a superuser can stop a node|permission denied for function stop_segment)/,
		'the stop is for the superuser alone');
	is($segments[0]->safe_psql('postgres', 'SELECT count(*) FROM p'), 5,
		'and the segment keeps serving');
}

# gp.stop_node on a segment stops that server alone; it comes back and
# registers again
{
	is($segments[2]->safe_psql('postgres', 'SELECT gp.stop_node()'), 'stopping',
		'the node reports the stop');
	for (my $i = 0; -e $segments[2]->data_dir . '/postmaster.pid' && $i < 30; $i++)
	{
		sleep 1;
	}
	ok(!-e $segments[2]->data_dir . '/postmaster.pid',
		'the segment went down');
	is(run('SELECT count(*) FROM p'), 5,
		'the cluster serves while a segment with no rows of the query is down');

	# Sync the test framework with the external stop, bring the node back
	$segments[2]->stop('immediate', fail_ok => 1);
	$segments[2]->start;
	$coordinator->poll_query_until('postgres',
		'SELECT count(*) = 3 FROM gp.cluster_segments()')
	  or die 'the stopped segment did not register again';
	is(on_segments('SELECT count(*) FROM p'), '5 0 0',
		'the rows of a pinned table stay on their segment');
}

# The stop of a segment through the coordinator: the statement is
# dispatched, the segment stops its own server
{
	is(run('SELECT gp.stop_segment(2)'), 'stopping',
		'the stop of a segment is dispatched');
	for (my $i = 0; -e $segments[1]->data_dir . '/postmaster.pid' && $i < 30; $i++)
	{
		sleep 1;
	}
	ok(!-e $segments[1]->data_dir . '/postmaster.pid',
		'the dispatched stop takes the segment down');

	# Sync the test framework with the external stop, bring the node back
	$segments[1]->stop('immediate', fail_ok => 1);
	$segments[1]->start;
	$coordinator->poll_query_until('postgres',
		'SELECT count(*) = 3 FROM gp.cluster_segments()')
	  or die 'the stopped segment did not register again';
	is(run('SELECT count(*) FROM p'), 5,
		'the restarted segment serves again');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
