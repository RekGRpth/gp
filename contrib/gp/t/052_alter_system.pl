# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A parameter of the whole cluster is set through SQL, without ssh:
# gp.alter_system() writes postgresql.auto.conf of every node the way
# ALTER SYSTEM writes one node, gp.reset_system() undoes it, and
# gp.reload_conf() makes it take effect without a restart.  The
# functions are for the superuser alone, and a segment refuses them,
# for the cluster is configured on the coordinator only

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub on_segments { my ($q) = @_; return join ' ', map { $_->safe_psql('postgres', $q) } @segments; }

# A parameter reaches every node, and reload makes it take effect
{
	run("SELECT gp.alter_system('log_min_duration_statement', '1234')");
	is(run('SHOW log_min_duration_statement'), '-1',
		'the write alone does not change the running value');
	run('SELECT gp.reload_conf()');
	is(run('SHOW log_min_duration_statement'), '1234ms',
		'the coordinator serves the new value');
	is(on_segments('SHOW log_min_duration_statement'), '1234ms 1234ms 1234ms',
		'every segment serves the new value');

	run("SELECT gp.reset_system('log_min_duration_statement')");
	run('SELECT gp.reload_conf()');
	is(run('SHOW log_min_duration_statement'), '-1',
		'the coordinator serves the default again');
	is(on_segments('SHOW log_min_duration_statement'), '-1 -1 -1',
		'every segment serves the default again');
}

# A value that needs its quotes goes as it is
{
	run("SELECT gp.alter_system('log_line_prefix', 'cluster wide')");
	run('SELECT gp.reload_conf()');
	is(run("SELECT setting FROM pg_settings WHERE name = 'log_line_prefix'"),
		'cluster wide',
		'a value with a space survives the trip');
	run("SELECT gp.reset_system('log_line_prefix')");
	run('SELECT gp.reload_conf()');
}

# An unknown parameter fails, and the nodes keep what they had
{
	my ($ret, $stdout, $stderr);
	$ret = $coordinator->psql('postgres',
		"SELECT gp.alter_system('no_such_parameter', '1')",
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/unrecognized configuration parameter/,
		'an unknown parameter is refused');
	is(run('SHOW log_min_duration_statement'), '-1',
		'the refusal changes nothing');
}

# The functions are for the superuser alone
{
	my ($ret, $stdout, $stderr);
	run('CREATE ROLE someone LOGIN');
	$ret = $coordinator->psql('postgres',
		"SET ROLE someone; SELECT gp.alter_system('log_min_duration_statement', '1')",
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/(only a superuser can configure|permission denied for function alter_system)/,
		'the set is for the superuser alone');
	$ret = $coordinator->psql('postgres',
		'SET ROLE someone; SELECT gp.reload_conf()',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/(only a superuser can reload|permission denied for function reload_conf)/,
		'the reload is for the superuser alone');
}

# A segment refuses: the cluster is configured on the coordinator only
{
	my ($ret, $stdout, $stderr);
	$ret = $segments[0]->psql('postgres',
		"SELECT gp.alter_system('log_min_duration_statement', '1')",
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/configured on the coordinator only/,
		'a segment refuses the set');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
