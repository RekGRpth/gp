# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The segments end the transactions of DDL by two-phase commit

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

# Segment 2 cannot prepare transactions at first
my ($coordinator, @segments) = start_cluster(
	segments => 2,
	segment_conf => sub {
		return "max_prepared_transactions = " .
		  ($_[0] == 1 ? 10 : 0) . "\n";
	});
my ($seg1, $seg2) = @segments;

sub where
{
	my ($relname) = @_;
	return join ' ',
	  map { $_->safe_psql('postgres', "SELECT to_regclass('$relname') IS NOT NULL") }
	  ($coordinator, @segments);
}

sub prepared
{
	return join ' ',
	  map { $_->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts') }
	  @segments;
}

# A segment that cannot prepare fails the commit everywhere
my ($ret, $stdout, $stderr) =
  $coordinator->psql('postgres', 'CREATE TABLE t1 (a int)');
like(
	$stderr,
	qr/prepared transactions are disabled\n.*CONTEXT:  preparing the transaction on segment 2/s,
	'a segment that cannot prepare fails the commit');
is(scalar(() = $stderr =~ /segment 2\b/g), 1, 'the error has one context');
is(where('t1'), 'f f f', 'the table is nowhere');
is(prepared(), '0 0', 'the prepared transaction of segment 1 is rolled back');

$seg2->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$seg2->restart;
$coordinator->poll_query_until('postgres',
	'SELECT count(*) = 2 FROM gp.cluster_segments()')
  or die 'segment 2 did not register again';

# Commit and rollback leave no prepared transactions
$coordinator->safe_psql('postgres', 'CREATE TABLE t2 (a int)');
is(where('t2'), 't t t', 'the segments commit the DDL');
is(prepared(), '0 0', 'a commit leaves no prepared transactions');
$coordinator->safe_psql('postgres', 'BEGIN; CREATE TABLE t3 (a int); ROLLBACK');
is(where('t3'), 'f f f', 'the segments roll back the DDL');
is(prepared(), '0 0', 'a rollback leaves no prepared transactions');

# A segment lost between the commit of the coordinator and COMMIT PREPARED
SKIP:
{
	skip 'injection points are not supported by this build', 6
	  unless $ENV{enable_injection_points} eq 'yes';

	# The recovery worker of the coordinator would finish the commit below
	# before the test does it by hand; push its next pass past the test
	$coordinator->safe_psql('postgres',
		"ALTER SYSTEM SET gp.recovery_interval = 3600;
SELECT pg_reload_conf();");

	$coordinator->safe_psql('postgres',
		"CREATE EXTENSION injection_points;
SELECT injection_points_attach('gp-before-commit-prepared', 'wait')");
	my $session = $coordinator->background_psql('postgres', on_error_stop => 0);
	$session->query_until(qr/start/, "\\echo start\nCREATE TABLE t4 (a int);\n");
	$coordinator->wait_for_event('client backend', 'gp-before-commit-prepared');
	$seg2->stop;
	$coordinator->safe_psql('postgres',
		"SELECT injection_points_detach('gp-before-commit-prepared');
SELECT injection_points_wakeup('gp-before-commit-prepared')");
	$session->query('SELECT 1');
	like(
		$session->{stderr},
		qr/WARNING:  could not run COMMIT PREPARED 'gp_\d+' on segment 2 at .*\nHINT:  The transaction stays prepared on the segment; run COMMIT PREPARED 'gp_\d+' there\./s,
		'a segment that cannot commit gets a warning');
	$session->quit;

	$seg2->start;
	is(where('t4'), 't t f', 'segment 2 has not committed');
	my $gid = $seg2->safe_psql('postgres', 'SELECT gid FROM pg_prepared_xacts');
	like($gid, qr/^gp_\d+$/, 'segment 2 keeps the transaction prepared');
	my ($xid) = $gid =~ /^gp_(\d+)$/;
	is($coordinator->safe_psql('postgres', "SELECT pg_xact_status('$xid')"),
		'committed', 'the coordinator tells the transaction committed');
	$seg2->safe_psql('postgres', "COMMIT PREPARED '$gid'");
	is(where('t4'), 't t t', 'COMMIT PREPARED by hand completes the commit');
	is(prepared(), '0 0', 'no prepared transactions are left');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
