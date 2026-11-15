# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Differential TPC-H-shaped correctness and plan regression.  The query texts
# come from DuckDB 1.5.4, extension/tpch/dbgen/queries.  The deterministic
# data set is deliberately small: this is not a dbgen or performance run.

use strict;
use warnings FATAL => 'all';
use File::Basename;
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
	'SELECT count(*) = 3 FROM gp.cluster_segments() WHERE interconnect_port IS NOT NULL')
  or die 'the segments did not report their interconnect ports';

sub run
{
	return $coordinator->safe_psql('postgres',
		'SET search_path = tpch; SET enable_mergejoin = off; ' .
		'SET statement_timeout = 120000; ' . shift);
}

my @tables = qw(region nation supplier part partsupp customer orders lineitem);
sub local_sql
{
	my ($sql) = @_;
	foreach my $table (sort { length($b) <=> length($a) } @tables)
	{
		$sql =~ s/\b$table\b/local_$table/gi;
	}
	return $sql;
}

my $tpch_dir = dirname(__FILE__) . '/tpch';
run(slurp_file("$tpch_dir/setup.sql"));

my %slice = map { $_ => 1 }
  qw(q01 q02 q03 q04 q05 q06 q10 q11 q12 q14 q17 q18 q19 q20);
foreach my $number (1 .. 22)
{
	my $name = sprintf('q%02d', $number);
	my $sql = slurp_file("$tpch_dir/$name.sql");
	my $plan = run('EXPLAIN (COSTS OFF) ' . $sql);

	if ($slice{$name})
	{
		like($plan, qr/GpSlice/, "$name uses a distributed slice");
	}
	else
	{
		unlike($plan, qr/GpSlice/, "$name keeps the coordinator fallback");
	}
	is(run($sql), run(local_sql($sql)),
		"$name returns the same answer as on local tables");
}

$_->stop foreach (@segments, $coordinator);

done_testing();
