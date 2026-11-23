# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# Differential TPC-DS-shaped correctness and plan regression.  The query
# texts come from the TPC-DS kit, rendered for PostgreSQL with the
# parameters of the kit substituted.  The deterministic data set is
# deliberately small: this is not a dsdgen or performance run.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(same_answers start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run
{
	return $coordinator->safe_psql('postgres',
		'SET search_path = tpcds; SET statement_timeout = 120000; ' . shift);
}

my @tables = qw(store_sales store_returns catalog_sales catalog_returns
				inventory web_sales web_returns
				customer customer_address customer_demographics
				household_demographics income_band
				item store date_dim promotion call_center catalog_page
				reason ship_mode time_dim warehouse web_page web_site);
sub local_sql
{
	my ($sql) = @_;

	# mask string literals so a table name inside a literal (q05 builds
	# ids as 'catalog_page' || cp_catalog_page_id) is not renamed
	my @lit;
	$sql =~ s/'(?:[^']|'')*'/push(@lit, $&); sprintf("\x00%d\x00", $#lit)/ge;
	foreach my $table (sort { length($b) <=> length($a) } @tables)
	{
		$sql =~ s/\b$table\b/local_$table/gi;
	}
	$sql =~ s/\x00(\d+)\x00/$lit[$1]/g;
	return $sql;
}

my $tpcds_dir = dirname(__FILE__) . '/tpcds';
run(slurp_file("$tpcds_dir/setup.sql"));

my %slice = map { $_ => 1 }
  qw(query06 query09 query10 query15 query19 query25 query28 query29
     query32 query34 query40 query45 query48 query50 query53 query55
     query69 query72 query73 query79 query84 query89
     query91 query92 query96);

# Answers that currently disagree with the local computation; each entry
# names the defect.  The comparison runs under TODO, so a fix flips the
# subtest to an unexpected pass instead of hiding a regression.
my %known_defect = (
	query02 =>
	  'the coordinator fallback does not gather a hashed table under a UNION ALL (Append), so the coordinator reads its empty local copy',
	query05 =>
	  'the coordinator fallback does not gather a hashed table under a UNION ALL (Append), so the coordinator reads its empty local copy',
	query09 =>
	  'a global aggregate pushed into the slice is not finalized on the coordinator: one row per segment is concatenated',
	query28 =>
	  'a global aggregate pushed into the slice is not finalized on the coordinator: one row per segment is concatenated',
	query69 =>
	  'a global aggregate pushed into the slice is not finalized on the coordinator: one row per segment is concatenated',
);

# query14 orders its answer by a sort key with ties; the distributed and
# the local plan break the ties differently, so compare the sorted rows.
my %sort_answer = (query14 => 1);

foreach my $number (1 .. 99)
{
	my $name = sprintf("query%02d", $number);
	my $sql = slurp_file("$tpcds_dir/$name.sql");
	# query14, query23, query24 and query39 run in two parts; explain each
	# of its statements
	my $plan = join('', map { run('EXPLAIN (COSTS OFF) ' . $_) }
		grep { /\S/ } split(/;/, $sql));

	if ($slice{$name})
	{
		like($plan, qr/GpSlice/, "$name uses a distributed slice");
	}
	else
	{
		unlike($plan, qr/GpSlice/, "$name keeps the coordinator fallback");
	}
	if ($known_defect{$name})
	{
		# same_answers calls ok() from package GpTest, where a local $TODO
		# would not be seen; the builder API marks the test globally
		my $builder = Test::More->builder;
		$builder->todo_start($known_defect{$name});
		same_answers($coordinator, \@segments, $sql, local_sql($sql),
			"$name returns the same answer as on local tables",
			'SET search_path = tpcds; SET statement_timeout = 120000; ');
		$builder->todo_end();
	}
	elsif ($sort_answer{$name})
	{
		my $settings =
		  'SET search_path = tpcds; SET statement_timeout = 120000; ';
		my $distributed = join("\n",
			sort split(/\n/, $coordinator->safe_psql('postgres', $settings . $sql)));
		my $expected = join("\n",
			sort split(/\n/, $coordinator->safe_psql('postgres', $settings . local_sql($sql))));
		is($distributed, $expected,
			"$name returns the same answer as on local tables");
	}
	else
	{
		same_answers($coordinator, \@segments, $sql, local_sql($sql),
			"$name returns the same answer as on local tables",
			'SET search_path = tpcds; SET statement_timeout = 120000; ');
	}
}

$_->stop foreach (@segments, $coordinator);

done_testing();
