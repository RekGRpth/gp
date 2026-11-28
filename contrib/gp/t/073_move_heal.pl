# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A move repeats after a crash: the copy starts clean on the new
# segment, so the rows of the bucket that the broken move left there go
# away with the copy of this one, and the bucket never doubles -- the
# rows of a move that broke before the switch would sit on the new
# segment while the map still named the old one, and a later move of
# the bucket to that segment would have copied the bucket beside them

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use GpTest qw(start_cluster);

my ($coordinator, @segments) = start_cluster();

sub run { return $coordinator->safe_psql('postgres', shift); }
sub seg { return $segments[$_[0] - 1]; }

sub shares
{
	return join(' / ', map { $_->safe_psql('postgres', $_[0]) } @segments);
}

run(<<'DDL');
CREATE TABLE t (a int, b text);
CREATE TABLE u (a int, d numeric);
SELECT gp.distribute('t', ARRAY['a']);
SELECT gp.distribute('u', ARRAY['a'], 't');
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 30) gs(g);
INSERT INTO u SELECT g % 4, g * 1.5 FROM generate_series(1, 30) gs(g);
DDL

# Two buckets with rows, by the keys that fall into them
my @pairs = split /\n/, run("SELECT DISTINCT ON (gp.bucket(1024, a)) "
	. "gp.bucket(1024, a) || ':' || a FROM t "
	. "ORDER BY gp.bucket(1024, a)");
my ($b1, $k1, $b2, $k2);
foreach my $line (@pairs)
{
	my ($bk, $key) = split /:/, $line;
	if (!defined $b1)
	{
		($b1, $k1) = ($bk, $key);
	}
	elsif ($bk != $b1)
	{
		($b2, $k2) = ($bk, $key);
		last;
	}
}
ok(defined $b2, "two buckets with rows: $b1 and $b2");

# The ordinary move of the first bucket keeps the whole
run("SELECT gp.move_bucket('t', $b1, 3)");
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the ordinary move keeps the truth of the whole');

# A move that a crash broke before the switch left the rows of the
# second bucket on segment 2.  The rows of the bucket on the owner go
# by the same keys: what the move left is what the owner holds
my $vals = run("SELECT string_agg('(' || a || ',' || quote_literal(b) "
			   . "|| ')', ', ') FROM t "
			   . "WHERE gp.bucket(1024, a) = $b2");
ok(length($vals) > 0, 'the second bucket has rows to leave behind');
my ($lsum, $ln) = split /\|/, run("SELECT coalesce(sum(a), 0) || '|' "
								  . "|| count(*) FROM t "
								  . "WHERE gp.bucket(1024, a) = $b2");
seg(2)->safe_psql('postgres', "INSERT INTO t (a, b) VALUES $vals");
my $uvals = run("SELECT string_agg('(' || a || ',' || d::text || ')', ', ') "
				. "FROM u WHERE gp.bucket(1024, a) = $b2");
seg(2)->safe_psql('postgres', "INSERT INTO u (a, d) VALUES $uvals")
  if length($uvals) > 0;
ok(seg(2)->safe_psql('postgres', "SELECT count(*) > 0 FROM t "
					 . "WHERE gp.bucket(1024, a) = $b2"),
	'segment 2 holds the rows the broken move left');

# The reads answer every row on every segment while the map still
# names the old owner: the rows the broken move left double the bucket,
# until the move of the bucket lands on their segment and starts clean
is(run('SELECT sum(a), count(*) FROM t'), (465 + $lsum) . '|' . (30 + $ln),
	'the rows the broken move left double the bucket until the heal');

# The move of the bucket to a third segment does not take the doubles
# away, for their segment is not the one the move lands on
run("SELECT gp.move_bucket('t', $b2, 3)");
is(run('SELECT sum(a), count(*) FROM t'), (465 + $lsum) . '|' . (30 + $ln),
	'and they double it still, for the move went to another segment');

# The move to the segment that holds the leftovers starts clean: the
# rows go away with the copy, and the bucket never doubles
run("SELECT gp.move_bucket('t', $b2, 2)");
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the move onto the leftovers starts clean');
is(run('SELECT sum(d), count(*) FROM u'), '697.5|30',
	'and the second table of the group healed with it');

# The shares of the segments add up again: no leftover of the bucket is
# on any segment beside its owner, and no row went away
my $sum = 0;
foreach my $segment (@segments)
{
	$sum += $segment->safe_psql('postgres', 'SELECT sum(a) FROM t');
}
is($sum, 465, 'the shares of the segments add up after the heal');

# The move repeats once more, and the whole stays whole
run("SELECT gp.move_bucket('t', $b2, 3)");
is(run('SELECT sum(a), count(*) FROM t'), '465|30',
	'the repeated move keeps the truth of the whole');

$coordinator->stop;
foreach my $segment (@segments)
{
	$segment->stop;
}
done_testing();
