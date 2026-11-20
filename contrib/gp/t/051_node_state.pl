# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The state of the nodes of the cluster is read through SQL: the
# coordinator asks every segment with a short connection of its own and
# tells a live node from a dead one.  Anyone may read the state

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

is(run('SELECT count(*) FROM gp.node_state()'), '3',
	'the state answers a row per registered segment');
is(run('SELECT bool_and(up) FROM gp.node_state()'), 't',
	'all segments answer');
is(run('SELECT count(*) FROM gp.node_state() WHERE in_recovery'), '0',
	'no segment is in recovery');
is(run("SELECT string_agg(host || ':' || port, ' ' ORDER BY segment_id)
		 FROM gp.node_state()"),
	run("SELECT string_agg(host || ':' || port, ' ' ORDER BY segment_id)
		 FROM gp.cluster_segments()"),
	'the addresses are the ones the cluster knows');

# A segment that went away keeps its registration but answers no more;
# it comes back with its own start
{
	$segments[1]->stop('immediate');
	$coordinator->poll_query_until('postgres',
		"SELECT NOT up FROM gp.node_state() WHERE segment_id = 2")
	  or die 'the dead segment never showed as dead';
	is(run('SELECT count(*) FROM gp.node_state() WHERE up'), '2',
		'the dead segment is the only one down');
	isnt(run('SELECT error FROM gp.node_state() WHERE segment_id = 2'), '',
		'the dead segment carries the reason');

	$segments[1]->start;
	$coordinator->poll_query_until('postgres',
		"SELECT up FROM gp.node_state() WHERE segment_id = 2")
	  or die 'the restarted segment never showed as live';
	is(run('SELECT bool_and(up) FROM gp.node_state()'), 't',
		'the cluster is whole again');
}

# The state is no secret: an ordinary role reads it too
run('CREATE ROLE someone LOGIN');
is($coordinator->safe_psql('postgres',
		'SET ROLE someone; SELECT count(*) FROM gp.node_state()'),
	'3', 'an ordinary role reads the state');

$_->stop foreach (@segments, $coordinator);

done_testing();
