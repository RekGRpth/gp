# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Helpers for the TAP tests of contrib/gp

package GpTest;

use strict;
use warnings FATAL => 'all';

use Exporter 'import';
use Test::More;

use PostgreSQL::Test::Cluster;

our @EXPORT_OK = qw(same_answers start_cluster);

# Start a coordinator and the given number of segments (three by default),
# with the standard settings of the gp tests.  A segment is a node named
# segment-N-PORT, the name it gets as cluster_name.  Extra configuration
# of the coordinator and of every segment joins the standard one; the
# segment extra may use $segment_id, which the helper substitutes, or be
# a code reference it calls with the number of the segment.  Returns the
# coordinator and the segments.
sub start_cluster
{
	my (%opts) = @_;
	my $nsegments = $opts{segments} // 3;
	my $coordinator_conf = $opts{coordinator_conf} // '';
	my $segment_conf = $opts{segment_conf} // '';

	my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
	$coordinator->init;
	$coordinator->append_conf(
		'postgresql.conf',
		"shared_preload_libraries = 'gp'
gp.role = 'coordinator'
" . $coordinator_conf);
	$coordinator->start;
	$coordinator->safe_psql('postgres', 'CREATE EXTENSION gp');

	my @segments;
	foreach my $segment_id (1 .. $nsegments)
	{
		my $port = PostgreSQL::Test::Cluster::get_free_port();
		my $node = PostgreSQL::Test::Cluster->new(
			"segment-$segment_id-$port", port => $port);
		my $extra = $opts{segment_conf} // '';
		if (ref $extra eq 'CODE')
		{
			$extra = $extra->($segment_id);
		}
		else
		{
			$extra =~ s/\$segment_id/$segment_id/g;
		}

		$node->init;
		$node->append_conf(
			'postgresql.conf', "shared_preload_libraries = 'gp'
gp.role = 'segment'
gp.segment_id = $segment_id
gp.coordinator = 'host=" . $coordinator->host . ' port=' .
			  $coordinator->port . "'
gp.host = '" . $node->host . "'
gp.register_retry_interval = 1
max_prepared_transactions = 10
" . $extra);
		$node->start;
		push @segments, $node;
	}
	$coordinator->poll_query_until('postgres',
		"SELECT count(*) = $nsegments FROM gp.cluster_segments()"
	  ) or die 'the segments did not register';
	return ($coordinator, @segments);
}

# Compare the answer of a distributed query with the answer of its local
# copy.  On a mismatch the facts go into the log before the cluster of the
# test goes away: the two answers, a fresh repeat of the query, the answer
# computed on every segment over its own rows, the state of the nodes and
# the dispatch counters.  A racing slice once answered with a group counted
# twice and another one missing; a bare is() loses the facts that tell the
# lost side from the duplicated one.  $settings is an optional SQL prefix
# (session settings such as a search_path) for every execution.
sub same_answers
{
	my ($coordinator, $segments, $query, $local, $name, $settings) = @_;
	$settings //= '';

	my $distributed =
	  $coordinator->safe_psql('postgres', $settings . $query);
	my $expected = $coordinator->safe_psql('postgres', $settings . $local);
	if ($distributed eq $expected)
	{
		ok(1, $name);
		return;
	}

	diag("mismatch: $name");
	diag("query: $query");
	diag("distributed answer:\n$distributed");
	diag("local answer:\n$expected");
	my $repeat = $coordinator->safe_psql('postgres', $settings . $query);
	diag('a fresh repeat ' .
		 ($repeat eq $distributed ? 'reproduces it' :
		  "answers differently:\n$repeat"));
	for my $segment (@$segments)
	{
		my $part = eval {
			$segment->safe_psql('postgres', $settings . $query)
		};
		diag('segment ' . $segment->name . ' (' . $segment->host . ':' .
			 $segment->port . ") over its own rows:\n" .
			 (defined $part ? $part : "error: $@"));
	}
	diag('node state: ' .
		 $coordinator->safe_psql('postgres', 'SELECT * FROM gp.node_state()'));
	diag('dispatches: ' .
		 $coordinator->safe_psql('postgres', 'SELECT gp.dispatches()'));
	ok(0, $name);
}

1;
