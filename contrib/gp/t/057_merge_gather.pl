# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The sorted gather: the segments sort their rows by the order of the
# query, and the gather merges their sorted answers, so the coordinator
# never sorts the whole table.  The plan carries the path keys of the
# query, and the Remote SQL carries the ORDER BY.  NULLS FIRST and NULLS
# LAST ride along, so do the multiple keys of one order

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub plan_for { return $coordinator->safe_psql('postgres', "EXPLAIN (COSTS OFF) $_[0]"); }
sub remote_sql { return $coordinator->safe_psql('postgres', "EXPLAIN (VERBOSE, COSTS OFF) $_[0]"); }

run('CREATE TABLE t (a int, b int, c int, s text, n numeric(10, 2))');
run("SELECT gp.distribute('t', ARRAY['a'])");
run("INSERT INTO t SELECT g,
		CASE WHEN g % 13 = 0 THEN NULL ELSE g % 13 END,
		g % 11, 'g' || (g % 5), g * 1.5 FROM generate_series(1, 1000) g");
# The same rows on the coordinator, to compare the answers with
run('CREATE TABLE l AS SELECT * FROM t');

my $ordered = 'SELECT a, b FROM t ORDER BY b LIMIT 7';
my $plan = plan_for($ordered);
unlike($plan, qr/\bSort\b/, 'the coordinator does not sort the gathered rows');
like($plan, qr/GpGather/, 'the gather answers the ordered query');
like(remote_sql($ordered), qr/ORDER BY b/, 'the segments sort their own rows');

foreach my $order (
	'SELECT b FROM t ORDER BY b LIMIT 7',
	'SELECT b FROM t ORDER BY b DESC LIMIT 7',
	'SELECT b FROM t ORDER BY b NULLS FIRST LIMIT 7',
	'SELECT b FROM t ORDER BY b NULLS LAST',
	'SELECT a, b, c FROM t ORDER BY b DESC, a LIMIT 20',
	'SELECT n FROM t ORDER BY n DESC NULLS LAST LIMIT 5',
	'SELECT s, a FROM t ORDER BY s DESC, a NULLS FIRST LIMIT 9',
	'SELECT a FROM t ORDER BY a, b, c LIMIT 4',
	'SELECT a FROM t ORDER BY a LIMIT 3 OFFSET 4',
	'SELECT b FROM t ORDER BY a LIMIT 7 OFFSET 2',
)
{
	is(run($order), run($order =~ s/\bFROM t\b/FROM l/r),
		"the merge keeps the order of the query: $order");
}

# The limit rides in the remote SQL: the segments answer a few rows, not
# the whole table
like(remote_sql('SELECT a FROM t ORDER BY a LIMIT 3'),
	qr/LIMIT 3/, 'the limit of the query rides to the segments');
like(remote_sql('SELECT a FROM t ORDER BY a LIMIT 3 OFFSET 4'),
	qr/LIMIT 7/, 'the offset rides with the limit, added to it');

# A numeric one carries its own type; NULLS FIRST and NULLS LAST are
# honored per key, and the null rows come first in this order
is(run('SELECT a FROM t ORDER BY n DESC NULLS FIRST, a LIMIT 15'),
	run('SELECT a FROM l ORDER BY n DESC NULLS FIRST, a LIMIT 15'),
	'the merge keeps a mixed nulls-first order');
like(run('SELECT a FROM t WHERE b IS NULL ORDER BY a LIMIT 3'),
	qr/\d/, 'the null rows of the merge answer');

# The transport of libpq answers the merge in ordinary sessions of the
# segments.  The remote SQL carries no WHERE: the filters of the query read
# the answers above the gather, so the segments must not cut their answers
# to the limit of the query, or the filters would read rows that were
# dropped before they saw them

my $filtered = remote_sql('SELECT a FROM t WHERE b IS NULL ORDER BY a LIMIT 3');
like($filtered, qr/ORDER BY a/, 'the filtered ordered query still sorts remotely');
unlike($filtered, qr/LIMIT/, 'a filtered query keeps the whole answers of the segments');

foreach my $query (
	'SELECT a FROM t WHERE b IS NULL ORDER BY a LIMIT 3',
	'SELECT a, n FROM t WHERE n > 1000 ORDER BY n LIMIT 5',
	'SELECT a FROM t WHERE c = 7 ORDER BY a DESC LIMIT 4',
	'SELECT s, a FROM t WHERE s = \'g2\' ORDER BY a LIMIT 6',
)
{
	is(run($query), run($query =~ s/\bFROM t\b/FROM l/r),
		"the filtered merge keeps the whole answer over libpq: $query");
}

# The correlated subplans rescan the merge of the inner gather once per
# outer row: the minmax answers must survive the rescan over libpq
like(plan_for('SELECT a, (SELECT max(t2.b) FROM t t2 WHERE t2.a = t.a) FROM t ORDER BY a LIMIT 15'),
	qr/GpGather/, 'the correlated aggregate rides the merge');
is(run('SELECT a, (SELECT max(t2.b) FROM t t2 WHERE t2.a = t.a) FROM t ORDER BY a LIMIT 15'),
	run('SELECT a, (SELECT max(l2.b) FROM l l2 WHERE l2.a = l.a) FROM l ORDER BY a LIMIT 15'),
	'a correlated scalar aggregate answers over the libpq merge');
is(run('SELECT a, (SELECT t2.c FROM t t2 WHERE t2.a = t.a AND t2.c IS NOT NULL ORDER BY t2.c DESC LIMIT 1) FROM t ORDER BY a LIMIT 15'),
	run('SELECT a, (SELECT l2.c FROM l l2 WHERE l2.a = l.a AND l2.c IS NOT NULL ORDER BY l2.c DESC LIMIT 1) FROM l ORDER BY a LIMIT 15'),
	'a correlated minmax without an aggregate answers over the libpq merge');


$_->stop foreach (@segments, $coordinator);

done_testing();
