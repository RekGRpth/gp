# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# gp.default_distribution: a table that CREATE TABLE makes is distributed by
# its first column that can be a key, if the setting says so

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster(segments => 2);

sub run { return $coordinator->safe_psql('postgres', shift); }

# The key columns of a table, or "none"
sub key_of
{
	my ($table) = @_;
	return run(qq{SELECT coalesce(string_agg(a.attname, ','), 'none')
FROM gp.distribution_policy p
CROSS JOIN LATERAL unnest(p.key_columns) k
JOIN pg_attribute a ON a.attrelid = p.relid AND a.attnum = k
WHERE p.relid = '$table'::regclass});
}

# The default is the coordinator
run('CREATE TABLE t0 (a int)');
is(key_of('t0'), 'none', 'a table stays on the coordinator by default');

# By the first column
run(q{SET gp.default_distribution = first_column;
CREATE TABLE t1 (a int, b text);
CREATE TABLE t2 (p point, b int, c text);
CREATE TABLE t3 (p point);
CREATE TABLE t4 (a int GENERATED ALWAYS AS (1) STORED, b int);
CREATE TABLE t5 (id int PRIMARY KEY, v text);
CREATE TEMP TABLE tt (a int);
CREATE TABLE t6 AS SELECT 1 AS a;
CREATE TABLE pt (a int, b int) PARTITION BY RANGE (a);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (10)});
is(key_of('t1'), 'a', 'by the first column');
is(key_of('t2'), 'b', 'by the first one that has a hash function');
is(key_of('t3'), 'none', 'not at all if none has one');
is(key_of('t4'), 'b', 'not by a generated column');
is(key_of('t5'), 'id', 'a table with a primary key');
is(run(q{SELECT count(*) FROM gp.distribution_policy
WHERE relid IN (SELECT oid FROM pg_class WHERE relname = 'tt')}), 0,
	'not a temporary table');
is(key_of('t6'), 'a', 'a table of CREATE TABLE AS');
is(key_of('pt'), 'a', 'a partitioned table');
is(key_of('pt1'), 'none', 'but its partition goes with it');

# The tables work
run(q{INSERT INTO t1 VALUES (1, 'x'), (2, 'y'), (3, 'z')});
is(run('SELECT count(*), sum(a) FROM t1'), '3|6', 'a table distributed by default');
is(join(' ', map { $_->safe_psql('postgres', 'SELECT count(*) FROM t1') } @segments) =~ /^[0-3] [0-3]$/ ? 1 : 0, 1,
	'has its rows on the segments');
run(q{INSERT INTO pt SELECT g, g FROM generate_series(0, 9) g});
is(run('SELECT count(*) FROM pt'), 10, 'a partitioned one too');
run(q{INSERT INTO t3 VALUES ('(1,2)')});
is(run('SELECT count(*) FROM t3'), 1, 'and one that stays on the coordinator');

# CREATE TABLE AS fills the table on the segments
run(q{SET gp.default_distribution = first_column;
CREATE TABLE c1 AS SELECT g AS a, g * 2 AS b FROM generate_series(1, 20) g;
CREATE TABLE c2 AS SELECT * FROM c1 WITH NO DATA;
SELECT a, b INTO c3 FROM c1 WHERE a <= 5;
CREATE TABLE IF NOT EXISTS c1 AS SELECT 100 AS a, 100 AS b});
is(run('SELECT count(*), sum(b) FROM c1'), '20|420', 'CREATE TABLE AS fills a table');
is(join('+', map { $_->safe_psql('postgres', 'SELECT count(*) FROM c1') } @segments) =~ /^(\d+)\+(\d+)$/ ? $1 + $2 : -1, 20,
	'on the segments');
is(run('SELECT count(*) FROM c2'), 0, 'WITH NO DATA fills nothing');
is(key_of('c2'), 'a', 'but has a key');
is(run('SELECT count(*) FROM c3'), 5, 'SELECT INTO');
is(run('SELECT count(*) FROM c1'), 20, 'IF NOT EXISTS skips an existing table');
run(q{PREPARE q AS SELECT 1 AS a; CREATE TABLE c4 AS EXECUTE q});
is(run('SELECT count(*) FROM c4'), 1, 'CREATE TABLE AS EXECUTE stays on the coordinator');

# The setting of a database
run('ALTER DATABASE postgres SET gp.default_distribution = first_column');
run('CREATE TABLE t7 (k int)');
is(key_of('t7'), 'k', 'is that of a database too');

$_->stop foreach (@segments, $coordinator);

done_testing();
