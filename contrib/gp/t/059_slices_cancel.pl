# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The deep scenario of the slices: the dispatched sessions of a slice
# park at the reader of their fragments, the statement of the
# coordinator takes a cancel, and every parked session unwinds: the
# statement answers with the error of the cancel, the session of the
# coordinator answers the next statement with no command of a segment
# left in progress, and a fresh statement answers over the same slice

use strict;
use warnings FATAL => 'all';
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }

run('CREATE TABLE t1 (a int, b text)');
run("SELECT gp.distribute('t1', ARRAY['a'])");
run('CREATE TABLE t2 (a int, c text)');
run("SELECT gp.distribute('t2', ARRAY['a'])");
run("INSERT INTO t1 VALUES (1, 'root-old')");
run("INSERT INTO t2 VALUES (1, 'peer-old')");

# Park every dispatched session at the reader of its fragment
foreach my $segment (@segments)
{
	$segment->safe_psql('postgres',
		"SET gp.allow_local_ddl = on;
CREATE EXTENSION injection_points");
	$segment->safe_psql('postgres',
		"SELECT injection_points_attach('gp-interconnect-reader', 'wait')");
}

my ($stdin, $stdout, $stderr) = ('', '', '');
my $timer = IPC::Run::timer(300);
my $session = IPC::Run::start(
	['psql', '-X', '-A', '-t', '-d',
	 $coordinator->connstr('postgres') . ' application_name=gp_deep_slice'],
	'<', \$stdin, '>', \$stdout, '2>', \$stderr, $timer);
$stdin .= "SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2;\n";
$session->pump_nb while length $stdin;
$session->pump_nb for 1 .. 50;

# Every segment parks a session of its fragment
foreach my $segment (@segments)
{
	$segment->poll_query_until('postgres',
		"SELECT count(*) >= 1 FROM pg_stat_activity
		 WHERE application_name = 'gp motion'
		   AND wait_event = 'gp-interconnect-reader'")
	  or BAIL_OUT('the fragment session did not park: '
		. $segment->safe_psql('postgres',
			'SELECT pid || \':\' || coalesce(wait_event, \'?\') '
			. '|| \':\' || application_name FROM pg_stat_activity')
		. " stdout=$stdout stderr=$stderr");
}
pass('the dispatched sessions of the segments park at the reader');

# A cancel of the statement unwinds the puller of every fragment: the
# statement answers with the error of the cancel, and the session answers
# the next statement, with no command of a segment left in progress
my $pid = $coordinator->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'gp_deep_slice'");
ok($pid, 'the session of the coordinator is there');
$coordinator->safe_psql('postgres', "SELECT pg_cancel_backend($pid)");

$coordinator->poll_query_until('postgres',
	"SELECT state = 'idle' FROM pg_stat_activity
	 WHERE application_name = 'gp_deep_slice'")
  or BAIL_OUT('the cancelled statement did not finish');
for (1 .. 100)
{
	$session->pump_nb;
	last if $stderr =~ /canceling statement/;
	select undef, undef, undef, 0.05;
}
like($stderr, qr/canceling statement due to user request/,
	'the statement answers with the cancel');
$stdin .= "SELECT 1;\n";
$session->pump_nb while length $stdin;
for (1 .. 100)
{
	$session->pump_nb;
	last if $stdout =~ /^1\n\z/;
	select undef, undef, undef, 0.05;
}
is($stdout, "1\n", 'the session answers the next statement');

# The point still holds the wait: a session parked at it cannot see that
# its puller is gone, so the test detaches the point and wakes every
# session it parked, to have each find its puller gone
foreach my $segment (@segments)
{
	$segment->safe_psql('postgres',
		"SELECT injection_points_detach('gp-interconnect-reader')");
}
foreach my $segment (@segments)
{
	for (1 .. 20)
	{
		my $parked = $segment->safe_psql('postgres',
			"SELECT count(*) FROM pg_stat_activity
			 WHERE application_name = 'gp motion'");
		last if $parked == 0;
		$segment->safe_psql('postgres',
			"SELECT injection_points_wakeup('gp-interconnect-reader');
SELECT pg_sleep(0.2)");
	}
	$segment->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity
		 WHERE application_name = 'gp motion'")
	  or BAIL_OUT('a parked session did not unwind: '
		. $segment->safe_psql('postgres',
			'SELECT pid || \':\' || coalesce(wait_event, \'?\') '
			. '|| \':\' || application_name FROM pg_stat_activity'));
}
pass('the parked sessions unwind when their pulls are gone');

is(run("SELECT t1.b, t2.c FROM ONLY t1 CROSS JOIN ONLY t2"),
	'root-old|peer-old', 'the cluster answers the slice after the cancel');

$session->kill_kill;
$_->stop foreach (@segments, $coordinator);

done_testing();
