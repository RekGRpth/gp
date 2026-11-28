# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The gathered reads answer by the owner of the rows: the map of the
# group rides in every gathered read, and a row answers from the
# segment the map names.  The rows that a broken move left on a foreign
# segment keep out of the gathered reads while they sit there, and a
# keyed read answers the row of the owner once

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub seg { return $segments[$_[0] - 1]; }

run(<<'DDL');
CREATE TABLE t (a int, b text);
CREATE TABLE u (a int, d numeric);
SELECT gp.distribute('t', ARRAY['a']);
SELECT gp.distribute('u', ARRAY['a'], 't');
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g);
INSERT INTO u SELECT g % 4, g * 1.5 FROM generate_series(1, 30) gs(g);
DDL

# The server every segment runs as
is(seg(1)->safe_psql('postgres', 'SELECT gp.self_segment()'), 1,
	'segment 1 runs as segment 1');
is(seg(3)->safe_psql('postgres', 'SELECT gp.self_segment()'), 3,
	'and segment 3 as segment 3');

# A bucket with rows, and the segment that owns it
my $bk = run('SELECT gp.bucket(1024, 7)');
my $group = run("SELECT colocation_id FROM gp.distribution_policy "
				. "WHERE relid = 't'::regclass");
my $owner = run("SELECT gp.bucket_segment($group, $bk)");
my $foreign = $owner == 1 ? 2 : 1;

# The rows a broken move would leave on the foreign segment
my $vals = run("SELECT string_agg('(' || a || ',' || quote_literal(b) "
			   . "|| ')', ', ') FROM t "
			   . "WHERE gp.bucket(1024, a) = $bk");
ok(length($vals) > 0, 'the bucket has rows to leave behind');
my ($key, $bval) = $vals =~ /\((\d+),'([^']+)'\)/;
seg($foreign)->safe_psql('postgres', "INSERT INTO t (a, b) VALUES $vals");
my $uvals = run("SELECT string_agg('(' || a || ',' || d::text || ')', ', ') "
				. "FROM u WHERE gp.bucket(1024, a) = $bk");
seg($foreign)->safe_psql('postgres', "INSERT INTO u (a, d) VALUES $uvals")
  if length($uvals) > 0;

# The gathered read answers the rows of the owner: the foreign copy of
# the bucket keeps out of it
my @rows = split(/\n/, run('SELECT a FROM t'));
is(scalar(@rows), 30,
	'the gather keeps the rows of the foreign segment out of the answer');

# A keyed read answers the row of the owner once, and the foreign copy
# of the row keeps out of it
my $got = run("SELECT b FROM t WHERE a = $key");
is($got, $bval, 'a keyed read answers the row of the owner');

# The reads of the second table of the group keep to the owner too
my @urows = split(/\n/, run('SELECT d FROM u'));
is(scalar(@urows), 30, 'and the second table reads by the owner');

# The raw rows of the segments hold the doubles
my $raw = 0;
foreach my $segment (@segments)
{
	$raw += $segment->safe_psql('postgres', 'SELECT count(*) FROM t');
}
ok($raw > 30, 'the raw rows of the segments still hold the doubles');

# A move of the bucket keeps the gathered reads owned, and the planted
# rows of the foreign segment stay out of them.  The target is a third
# segment: a move to the owner stays put before the locks
my ($target) = grep { $_ != $owner && $_ != $foreign } (1, 2, 3);
run("SELECT gp.move_bucket('t', $bk, $target)");
@rows = split(/\n/, run('SELECT a FROM t'));
is(scalar(@rows), 30, 'the move keeps the gathered reads owned');
is(run("SELECT b FROM t WHERE a = $key"), $bval,
	'and the keyed read answers the owner row once');

# The move onto the planted rows starts clean, and the shares of the
# segments add up again
run("SELECT gp.move_bucket('t', $bk, $foreign)");
@rows = split(/\n/, run('SELECT a FROM t'));
is(scalar(@rows), 30, 'the move onto the planted rows starts clean');
my $sum = 0;
foreach my $segment (@segments)
{
	$sum += $segment->safe_psql('postgres', 'SELECT sum(a) FROM t');
}
is($sum, 465, 'and the shares of the segments add up');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
