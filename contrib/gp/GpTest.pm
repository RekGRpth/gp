# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Helpers for the TAP tests of contrib/gp

package GpTest;

use strict;
use warnings FATAL => 'all';

use Exporter 'import';
use Test::More;

our @EXPORT_OK = qw(same_answers);

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
