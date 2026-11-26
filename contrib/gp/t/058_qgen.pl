# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The queries of the qgen tool of TPC-H run over the cluster: the samples
# below come from tpch-kit qgen 2.17.3, the seed 42.  The dialect of qgen
# carries three things the server takes no, and a benchmark drops them on
# the way in: the precision of an interval field, `day (3)`, a negative
# rowcount, `limit -1`, which meant no rowcount at all, and the rowcount
# set off by its own semicolon.  The answers must stay the ones of the
# tables of the coordinator, and the queries must ride the segments the
# way the queries of the suite do

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

run(slurp_file(dirname(__FILE__) . '/tpch/setup.sql'));

# qgen -r 42 1: the interval precision is dropped, the rowcount of -1
# meant no rowcount
my $q01 = <<'SQL';
select
	l_returnflag,
	l_linestatus,
	sum(l_quantity) as sum_qty,
	sum(l_extendedprice) as sum_base_price,
	sum(l_extendedprice * (1 - l_discount)) as sum_disc_price,
	sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) as sum_charge,
	avg(l_quantity) as avg_qty,
	avg(l_extendedprice) as avg_price,
	avg(l_discount) as avg_disc,
	count(*) as count_order
from
	lineitem
where
	l_shipdate <= date '1998-12-01' - interval '91' day
group by
	l_returnflag,
	l_linestatus
order by
	l_returnflag,
	l_linestatus
SQL

# qgen -r 42 6: the rowcount of -1 meant no rowcount
my $q06 = <<'SQL';
select
	sum(l_extendedprice * l_discount) as revenue
from
	lineitem
where
	l_shipdate >= date '1997-01-01'
	and l_shipdate < date '1997-01-01' + interval '1' year
	and l_discount between 0.06 - 0.01 and 0.06 + 0.01
	and l_quantity < 25
SQL

# qgen -r 42 18: the rowcount rides inside the query
my $q18 = <<'SQL';
select
	c_name,
	c_custkey,
	o_orderkey,
	o_orderdate,
	o_totalprice,
	sum(l_quantity)
from
	customer,
	orders,
	lineitem
where
	o_orderkey in (
		select
			l_orderkey
		from
			lineitem
		group by
			l_orderkey having
				sum(l_quantity) > 312
	)
	and c_custkey = o_custkey
	and o_orderkey = l_orderkey
group by
	c_name,
	c_custkey,
	o_orderkey,
	o_orderdate,
	o_totalprice
order by
	o_totalprice desc,
	o_orderdate
limit 100
SQL

foreach my $query ([$q01, 'q01'], [$q06, 'q06'], [$q18, 'q18'])
{
	my ($sql, $name) = @$query;
	my $plan = run('EXPLAIN (COSTS OFF) ' . $sql);

	like($plan, qr/GpSlice/, "$name rides the segments");
	same_answers($coordinator, \@segments, $sql, local_sql($sql),
		"$name returns the same answer as on local tables",
		'SET search_path = tpch; SET enable_mergejoin = off; ' .
		'SET statement_timeout = 120000; ');
}

$_->stop foreach (@segments, $coordinator);

done_testing();
