# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The log of any node of the cluster is read through SQL: the tail of the
# newest log file of the logging collector, of as many lines as asked,
# for the superuser alone

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $coordinator = PostgreSQL::Test::Cluster->new('coordinator');
$coordinator->init;
$coordinator->append_conf('postgresql.conf',
	"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n" .
	"logging_collector = on\nlog_destination = 'stderr'\n");
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
logging_collector = on
log_destination = 'stderr'
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

# An error of the session lands in the log of the node, and the tail of
# the log answers it
{
	my ($ret, $stdout, $stderr);
	$ret = $coordinator->psql('postgres',
		'SELECT * FROM gp_log_tail_missing_table',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	$coordinator->poll_query_until('postgres',
		"SELECT bool_or(log_tail LIKE '%gp_log_tail_missing_table%')
		 FROM gp.log_tail(50)") or die 'the log never got the row';
	like(run("SELECT log_tail FROM gp.log_tail(50)
			  WHERE log_tail LIKE '%gp_log_tail_missing_table%' LIMIT 1"),
		qr/gp_log_tail_missing_table/,
		'an error of the session is read from the log of the coordinator');
}

# The tail keeps no more lines than asked
{
	my @lines = split /\n/, run('SELECT * FROM gp.log_tail(3)');
	cmp_ok(scalar @lines, '<=', 3, 'the tail answers the lines it was asked');
	cmp_ok(scalar @lines, '>=', 1, 'and at least one line is there');
}

# A run without the collector is refused
{
	my $off = PostgreSQL::Test::Cluster->new('off');
	$off->init;
	$off->append_conf('postgresql.conf',
		"shared_preload_libraries = 'gp'\ngp.role = 'coordinator'\n");
	$off->start;
	$off->safe_psql('postgres', 'CREATE EXTENSION gp');
	my ($ret, $stdout, $stderr);
	$ret = $off->psql('postgres', 'SELECT * FROM gp.log_tail(5)',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/logging_collector is off/,
		'a node without the collector refuses the read');
	$off->stop('immediate');
}

# The log of a segment is read on the segment itself
{
	my $seg = $segments[0];
	my ($ret, $stdout, $stderr);
	$ret = $seg->psql('postgres', 'SELECT * FROM segment_log_tail_missing',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	$seg->poll_query_until('postgres',
		"SELECT bool_or(log_tail LIKE '%segment_log_tail_missing%')
		 FROM gp.log_tail(50)") or die 'the segment log never got the row';
	is(($seg->safe_psql('postgres',
		'SELECT count(*) FROM (SELECT * FROM gp.log_tail(2)) x')),
		'2', 'the tail of a segment answers the lines it was asked');
}

# The read is for the superuser alone
run('CREATE ROLE someone LOGIN');
{
	my ($ret, $stdout, $stderr);
	$ret = $coordinator->psql('postgres',
		'SET ROLE someone; SELECT * FROM gp.log_tail(5)',
		stdout => \$stdout, stderr => \$stderr,
		on_error_stop => 0, on_error_die => 0);
	like($stderr, qr/(only a superuser can read|permission denied for function log_tail)/,
		'the log is for the superuser alone');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
