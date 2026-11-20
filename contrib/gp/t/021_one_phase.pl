# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A transaction that wrote one segment alone commits with the coordinator,
# in one phase: no prepared transaction, and a segment lost in the moment
# rolls back, with a warning about rows that may be lost

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster(segments => 2);

sub run { return $coordinator->safe_psql('postgres', shift); }

# No prepared transactions anywhere, whatever the writes
sub nothing_prepared
{
	return scalar(() = grep { $_->safe_psql('postgres',
			"SELECT count(*) FROM pg_prepared_xacts") ne '0' } @segments);
}

run('CREATE TABLE t (a int)');
run("SELECT gp.distribute('t', ARRAY['a'])");

# A row that one segment takes commits in one phase
run('BEGIN; INSERT INTO t VALUES (1); COMMIT');
is(run('SELECT count(*) FROM t'), 1, 'a one-segment write commits');
is(nothing_prepared(), 0, 'no segment keeps a prepared transaction');

# A write that two segments take still prepares
run('BEGIN; INSERT INTO t VALUES (1), (2), (3), (4), (5), (6), (7); COMMIT');
is(run('SELECT count(*) FROM t'), 8, 'a two-segment write commits');
is(nothing_prepared(), 0, 'no segment keeps a prepared transaction');

# A segment lost in the one-phase moment rolls back, with a warning
SKIP:
{
	skip 'injection points are not supported by this build', 4
	  unless $ENV{enable_injection_points} eq 'yes';

	run('TRUNCATE t');

	# A key that the first segment takes
	my $key = $coordinator->safe_psql('postgres',
		"SELECT k FROM generate_series(1, 100) k, gp.distribution_policy p
		 WHERE p.relid = 't'::regclass
		   AND gp.bucket_segment(p.colocation_id,
								  gp.bucket(1024, k)) = 1 LIMIT 1");
	$coordinator->safe_psql('postgres',
		"CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-before-one-phase-commit', 'wait')");
	my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/start/, "\\echo start\nINSERT INTO t VALUES ($key);\n");
	$coordinator->wait_for_event('client backend', 'gp-before-one-phase-commit');

	is( $segments[0]->safe_psql('postgres', 'SELECT count(*) FROM t')
		  + $segments[1]->safe_psql('postgres', 'SELECT count(*) FROM t'),
		0, 'the segments do not have the row yet');
	is(nothing_prepared(), 0, 'no segment keeps a prepared transaction');

	$segments[0]->stop;
	$coordinator->safe_psql('postgres',
		"SELECT injection_points_detach('gp-before-one-phase-commit');
SELECT injection_points_wakeup('gp-before-one-phase-commit')");
	$session->query('SELECT 1');
	like(
		$session->{stderr},
		qr/WARNING:  could not run COMMIT on segment 1 at .*\nDETAIL:  .*\nHINT:  The segment rolled back, and the rows of the transaction may be lost; run the command again\./s,
		'a lost segment warns about rows that may be lost');
	$session->quit;

	$segments[0]->start;
	is(run('SELECT count(*) FROM t'), 0, 'the row is lost with the segment');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
