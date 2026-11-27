/* contrib/gp/gp--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION gp" to load this file. \quit

-- Distributed queries call the functions here as the session user
GRANT USAGE ON SCHEMA gp TO PUBLIC;

-- Segments of the cluster, numbered from 1; the coordinator is 0.  The
-- table is used in the database named by gp.database on the coordinator;
-- segments add themselves to it with gp.register_segment().
CREATE TABLE gp.segments (
	segment_id int PRIMARY KEY CHECK (segment_id > 0),
	host text NOT NULL CHECK (octet_length(host) < 256),
	port int NOT NULL CHECK (port BETWEEN 1 AND 65535),
	registered timestamptz NOT NULL DEFAULT now(),	-- last registration
	UNIQUE (host, port)
);

SELECT pg_catalog.pg_extension_config_dump('gp.segments', '');

-- The coordinator keeps a copy of gp.segments in shared memory, for any
-- database.  A transaction that changes the table updates the copy when it
-- commits.
CREATE FUNCTION gp.segments_changed()
RETURNS trigger
AS 'MODULE_PATHNAME', 'gp_segments_changed'
LANGUAGE C;

CREATE TRIGGER segments_changed
AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON gp.segments
FOR EACH STATEMENT EXECUTE FUNCTION gp.segments_changed();

-- Segments of the cluster as the coordinator knows them, from any database
CREATE FUNCTION gp.cluster_segments(OUT segment_id int, OUT host text,
									OUT port int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_cluster_segments'
LANGUAGE C STRICT;

-- Register a segment, or confirm the registration of a known one.  A
-- segment keeps its number and address: a number registered at another
-- address, or an address registered under another number, is an error.
CREATE FUNCTION gp.register_segment(segment_id int, host text, port int)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	known gp.segments;
BEGIN
	IF current_setting('gp.role', true) IS DISTINCT FROM 'coordinator' THEN
		RAISE EXCEPTION 'segments register on the coordinator only';
	END IF;
	IF current_database() <> current_setting('gp.database') THEN
		RAISE EXCEPTION 'segments register in database "%" only',
			current_setting('gp.database');
	END IF;

	-- Registrations come one at a time, so the checks below hold
	LOCK TABLE gp.segments IN SHARE ROW EXCLUSIVE MODE;

	SELECT * INTO known FROM gp.segments s
	WHERE s.segment_id = register_segment.segment_id;
	IF FOUND THEN
		IF known.host <> register_segment.host OR
			known.port <> register_segment.port THEN
			RAISE EXCEPTION 'segment % is already registered at %:%',
				known.segment_id, known.host, known.port;
		END IF;
		UPDATE gp.segments s SET registered = now()
		WHERE s.segment_id = register_segment.segment_id;
		RETURN;
	END IF;

	SELECT * INTO known FROM gp.segments s
	WHERE s.host = register_segment.host AND s.port = register_segment.port;
	IF FOUND THEN
		RAISE EXCEPTION '%:% is already registered as segment %',
			known.host, known.port, known.segment_id;
	END IF;

	IF (SELECT count(*) FROM gp.segments) >=
		current_setting('gp.max_segments')::int THEN
		RAISE EXCEPTION 'cannot register more than % segments',
			current_setting('gp.max_segments')
			USING HINT = 'Raise "gp.max_segments" on the coordinator.';
	END IF;

	INSERT INTO gp.segments (segment_id, host, port)
	VALUES (register_segment.segment_id, register_segment.host,
			register_segment.port);
END
$$;

REVOKE ALL ON FUNCTION gp.register_segment(int, text, int) FROM PUBLIC;

-- Run a command on all segments at once and return the command status of
-- each; rows of a query are discarded.  Each segment runs the command in a
-- transaction of its own: a rollback on the coordinator does not undo it.
-- An error on a segment is reported after all segments have answered.
CREATE FUNCTION gp.exec_on_all(sql text, OUT segment_id int, OUT status text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_exec_on_all'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gp.exec_on_all(text) FROM PUBLIC;

-- OIDs of the new catalog rows of the current transaction on the
-- coordinator, as a segment takes them in gp.dispatch_oids
CREATE FUNCTION gp.recorded_oids()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_recorded_oids'
LANGUAGE C STRICT;

-- The actual creation OIDs of this extension, for a segment that registers
CREATE FUNCTION gp.extension_oids()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_extension_oids'
LANGUAGE C STRICT;
REVOKE ALL ON FUNCTION gp.extension_oids() FROM PUBLIC;

-- Colocation groups: tables of a group have keys of the same types and the
-- same number of buckets, so rows with equal keys are on the same segment
-- and a join of the tables by the key is local.  map[b + 1] is the segment
-- of bucket b; moving a bucket to another segment changes one element.
CREATE TABLE gp.colocation_groups (
	colocation_id serial PRIMARY KEY,
	key_types regtype[] NOT NULL CHECK (cardinality(key_types) > 0),
	nbuckets int NOT NULL CHECK (nbuckets > 0),
	map int[] NOT NULL CHECK (cardinality(map) = nbuckets)
);

-- How the tables of this database are distributed: by hash of the key
-- columns, in a colocation group, by the ranges or the values of one key
-- column, or replicated to all segments
CREATE TABLE gp.distribution_policy (
	relid regclass PRIMARY KEY,
	kind text NOT NULL CHECK (kind IN ('hash', 'replicated', 'range', 'list')),
	key_columns int2[],
	colocation_id int REFERENCES gp.colocation_groups,
	CHECK ((kind = 'hash') =
		   (key_columns IS NOT NULL AND colocation_id IS NOT NULL)),
	CHECK ((kind = 'replicated') = (key_columns IS NULL)),
	CHECK (colocation_id IS NULL OR kind = 'hash')
);

-- The rules of a range or a list distribution: the bucket of every range
-- of values, with its bounds, and of every value of a list.  The bounds
-- and the values are the texts of the type of the key column.  A range
-- keeps its bounds and no value; a list keeps values and no bounds.  A
-- range with n bounds has n + 1 buckets: the bucket of bucket index i
-- holds the values above bounds[i - 1] and up to bounds[i], the first
-- holds all below bounds[0], and the last all above the last bound
CREATE TABLE gp.distribution_rules (
	relid regclass NOT NULL REFERENCES gp.distribution_policy (relid)
			   ON DELETE CASCADE,
	bucket int NOT NULL CHECK (bucket >= 0),
	segment int NOT NULL,
	lower text,
	upper text,
	value text,
	CHECK (value IS NULL OR (lower IS NULL AND upper IS NULL))
);

SELECT pg_catalog.pg_extension_config_dump('gp.distribution_rules', '');

SELECT pg_catalog.pg_extension_config_dump('gp.colocation_groups', '');
SELECT pg_catalog.pg_extension_config_dump('gp.colocation_groups_colocation_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gp.distribution_policy', '');

-- The bucket of a key among nbuckets
CREATE FUNCTION gp.bucket(nbuckets int, VARIADIC key "any")
RETURNS int
AS 'MODULE_PATHNAME', 'gp_bucket'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- The segment of a bucket of a colocation group
CREATE FUNCTION gp.bucket_segment(colocation_id int, bucket int)
RETURNS int
LANGUAGE sql STABLE PARALLEL SAFE
BEGIN ATOMIC
	SELECT g.map[bucket + 1] FROM gp.colocation_groups g
	WHERE g.colocation_id = bucket_segment.colocation_id;
END;

CREATE FUNCTION gp.check_distribution_key(relation regclass, attnums int2[])
RETURNS void
AS 'MODULE_PATHNAME', 'gp_check_distribution_key'
LANGUAGE C STRICT;

-- Make every session read the policy of a table again
CREATE FUNCTION gp.policy_changed(relation regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_policy_changed'
LANGUAGE C STRICT;

-- The segment this server is: 0 on the coordinator, and its number on a
-- segment.  A row of a distributed table keeps its segment there, and
-- answers per row come with the pushdown of the MPP planner.
CREATE FUNCTION gp.segment_id() RETURNS int4
AS 'MODULE_PATHNAME', 'gp_segment_id_fn'
LANGUAGE C VOLATILE;

-- The number of statements and row streams of this session that have gone
-- to the segments.  A query over the tables that keep their rows on the
-- coordinator leaves it unchanged.
CREATE FUNCTION gp.dispatches()
RETURNS bigint
AS 'MODULE_PATHNAME', 'gp_dispatches_fn'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

-- Stop the server of the node the function runs on: the fast stop, the
-- same that pg_ctl -m fast sends.  The superuser alone, for the stop
-- takes the sessions of every user with it
CREATE FUNCTION gp.stop_node()
RETURNS text
AS 'MODULE_PATHNAME', 'gp_stop_node'
LANGUAGE C VOLATILE;

-- Stop the server of a segment by its number: the stop is dispatched to
-- the segment, whose answer may be lost on the way back, for the node is
-- going away
CREATE FUNCTION gp.stop_segment(segment_id int)
RETURNS text
AS 'MODULE_PATHNAME', 'gp_stop_segment'
LANGUAGE C VOLATILE;

-- The tail of the log of this node: the newest file of the logging
-- collector, of as many lines as asked.  The superuser alone, for the
-- log carries the texts of other users' statements
CREATE FUNCTION gp.log_tail(lines int DEFAULT 50)
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'gp_log_tail'
LANGUAGE C VOLATILE STRICT;

REVOKE ALL ON FUNCTION gp.log_tail(int) FROM PUBLIC;

REVOKE ALL ON FUNCTION gp.stop_node() FROM PUBLIC;
REVOKE ALL ON FUNCTION gp.stop_segment(int) FROM PUBLIC;

-- The state of the nodes of the cluster, as the coordinator sees it: a
-- short connection of its own tells a live node from a dead one.  Open to
-- anyone: the state of the cluster is no secret
CREATE FUNCTION gp.node_state(OUT segment_id int, OUT host text, OUT port int,
OUT up boolean, OUT in_recovery boolean, OUT error text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_node_state'
LANGUAGE C VOLATILE;

-- A parameter of all the nodes of the cluster, written the way ALTER
-- SYSTEM writes one node, without ssh.  The superuser alone
CREATE FUNCTION gp.alter_system(name text, value text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_alter_system'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION gp.reset_system(name text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_reset_system'
LANGUAGE C VOLATILE STRICT;

-- Reread the configuration files of every node, so a parameter written by
-- gp.alter_system() takes effect without a restart.  The superuser alone
CREATE FUNCTION gp.reload_conf()
RETURNS void
AS 'MODULE_PATHNAME', 'gp_reload_conf'
LANGUAGE C VOLATILE;

REVOKE ALL ON FUNCTION gp.alter_system(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION gp.reset_system(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION gp.reload_conf() FROM PUBLIC;

-- Run one pass of the recovery of the transactions the segments keep
-- prepared
CREATE FUNCTION gp.recovery_pass_now()
RETURNS void
AS 'MODULE_PATHNAME', 'gp_recovery_pass_now'
LANGUAGE C;

REVOKE ALL ON FUNCTION gp.recovery_pass_now() FROM PUBLIC;

-- The rows of a query from every segment of the cluster, among them the
-- segment that asks.  The query returns the columns the caller says it
-- does.  It runs on a segment, written by the planner of the coordinator
-- into the queries of a redistributed join
CREATE FUNCTION gp.peer_query(sql text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_peer_query'
LANGUAGE C VOLATILE;

-- Execute the serialized fragment of a query in this session and answer its
-- rows.  The pull of a motion asks every peer for its fragment with this
-- call in a session of its own, instead of the workers of the interconnect.
-- The executor checks the permissions of the caller as for any query, and
-- only the planner of a peer writes the calls
CREATE FUNCTION gp.fragment_exec(fragment text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_fragment_exec'
LANGUAGE C VOLATILE;

-- Attach the exported snapshots of a distributed statement to this backend,
-- so a fragment dispatched over libpq reads the same state as its nested
-- motions.  The coordinator's dispatcher calls this on the segments in the
-- transaction of the fragment; a name is an unguessable secret of the
-- exporting transaction, as in SET TRANSACTION SNAPSHOT
CREATE FUNCTION gp.attach_statement_snapshots(map text)
RETURNS void
AS 'MODULE_PATHNAME', 'gp_attach_statement_snapshots'
LANGUAGE C STRICT;

-- Distribute an empty table by hash of the key columns, or replicate it to
-- all segments if key is NULL.  A table distributed by hash joins the
-- colocation group of colocate_with, or else the first group with its key
-- types and number of buckets; a new group spreads its buckets over the
-- segments in turn.  With segment set, the table is pinned to that one
-- segment instead: every bucket of its own new group points there, so
-- all of its rows live on that segment alone.
CREATE FUNCTION gp.distribute(relation regclass, key name[],
							  colocate_with regclass DEFAULT NULL,
							  segment int DEFAULT NULL,
							  coordinator bool DEFAULT false)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	buckets constant int := 1024;
	attnums int2[];
	types regtype[];
	group_id int;
	group_types regtype[];
	group_coordinator bool;
	segments int[];
	nonempty bool;
BEGIN
	IF current_setting('gp.role', true) IS DISTINCT FROM 'coordinator' THEN
		RAISE EXCEPTION 'tables are distributed on the coordinator only';
	END IF;
	IF (SELECT c.relkind NOT IN ('r', 'p') OR c.relpersistence = 't'
		FROM pg_class c WHERE c.oid = relation) THEN
		RAISE EXCEPTION '"%" is not a permanent table', relation;
	END IF;
	IF EXISTS (SELECT FROM gp.distribution_policy p WHERE p.relid = relation)
	THEN
		RAISE EXCEPTION 'table "%" is distributed already', relation;
	END IF;
	EXECUTE format('SELECT EXISTS (SELECT FROM %s)', relation) INTO nonempty;
	IF nonempty THEN
		RAISE EXCEPTION 'table "%" is not empty', relation
			USING HINT = 'Distribute a table before filling it.';
	END IF;

	-- A segment sees its own rows only: a unique index without the whole
	-- key of the distribution cannot tell two equal rows apart when they
	-- sit on different segments
	IF key IS NOT NULL AND EXISTS (
		SELECT FROM pg_catalog.pg_index i
		WHERE i.indrelid = relation AND i.indisunique
		  AND NOT (
			SELECT array_agg(a.attnum)
			FROM unnest(key) AS k(name)
			JOIN pg_catalog.pg_attribute a
			  ON a.attrelid = relation AND a.attname = k.name
		  ) <@ i.indkey::pg_catalog.int2[]
	) THEN
		RAISE EXCEPTION
			'the unique indexes of table "%" must include the whole key of the distribution',
			relation
			USING HINT = 'Rows with equal values may sit on different segments; distribute by a column of every unique index.';
	END IF;

	IF key IS NULL THEN
		IF coordinator THEN
			RAISE EXCEPTION 'a replicated table keeps no share of the coordinator';
		END IF;
		IF colocate_with IS NOT NULL THEN
			RAISE EXCEPTION 'a replicated table has no colocation group';
		END IF;
		IF segment IS NOT NULL THEN
			RAISE EXCEPTION 'a table without a key of distribution cannot be pinned to a segment';
		END IF;
		INSERT INTO gp.distribution_policy (relid, kind)
		VALUES (relation, 'replicated');
		PERFORM gp.policy_changed(relation);
		RETURN;
	END IF;



	IF cardinality(key) = 0 THEN
		RAISE EXCEPTION 'a key has at least one column';
	END IF;
	SELECT array_agg(a.attnum ORDER BY k.ord),
		   array_agg(a.atttypid::regtype ORDER BY k.ord)
	INTO attnums, types
	FROM unnest(key) WITH ORDINALITY AS k(name, ord)
	LEFT JOIN pg_attribute a
	ON a.attrelid = relation AND a.attname = k.name AND a.attnum > 0 AND
	   NOT a.attisdropped;
	IF array_position(attnums, NULL) IS NOT NULL THEN
		RAISE EXCEPTION 'column "%" of table "%" does not exist',
			key[array_position(attnums, NULL)], relation;
	END IF;
	IF (SELECT count(DISTINCT n) FROM unnest(attnums) n) < cardinality(attnums)
	THEN
		RAISE EXCEPTION 'a key has a column more than once';
	END IF;
	PERFORM gp.check_distribution_key(relation, attnums);

	IF segment IS NOT NULL AND coordinator THEN
		RAISE EXCEPTION 'a table pinned to a segment keeps no share of the coordinator';
	END IF;
	IF segment IS NOT NULL THEN
		IF colocate_with IS NOT NULL THEN
			RAISE EXCEPTION 'a table pinned to a segment joins no colocation group of another';
		END IF;
		IF NOT EXISTS (SELECT FROM gp.cluster_segments() s
					   WHERE s.segment_id = segment) THEN
			RAISE EXCEPTION 'segment % is not registered', segment;
		END IF;

		-- Every bucket of the own new group points to the segment, so all
		-- the rows of the table live there alone
		LOCK TABLE gp.colocation_groups IN SHARE ROW EXCLUSIVE MODE;
		INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
		SELECT types, buckets, array_fill(segment, ARRAY[buckets])
		RETURNING colocation_id INTO group_id;
		INSERT INTO gp.distribution_policy (relid, kind, key_columns, colocation_id)
		VALUES (relation, 'hash', attnums, group_id);
		PERFORM gp.policy_changed(relation);
		RETURN;
	END IF;

	-- Groups are made one at a time, so the search below holds
	LOCK TABLE gp.colocation_groups IN SHARE ROW EXCLUSIVE MODE;

	IF colocate_with IS NOT NULL THEN
		SELECT p.colocation_id, g.key_types,
			   (0 = ANY (g.map)) AS group_coordinator
		INTO group_id, group_types, group_coordinator
		FROM gp.distribution_policy p
		JOIN gp.colocation_groups g USING (colocation_id)
		WHERE p.relid = colocate_with AND p.kind = 'hash';
		IF NOT FOUND THEN
			RAISE EXCEPTION 'table "%" is not distributed by hash',
				colocate_with;
		END IF;
		IF coordinator <> group_coordinator THEN
			RAISE EXCEPTION 'table "%" does not keep a share of the coordinator',
				colocate_with;
		END IF;
		IF group_types <> types THEN
			RAISE EXCEPTION 'key of table "%" is %, but key of table "%" is %',
				relation, types, colocate_with, group_types;
		END IF;
	ELSE
		SELECT g.colocation_id INTO group_id FROM gp.colocation_groups g
		WHERE g.key_types = types AND g.nbuckets = buckets AND
			  coordinator = (0 = ANY (g.map))
		ORDER BY g.colocation_id LIMIT 1;
		IF NOT FOUND THEN
			SELECT array_agg(s.segment_id ORDER BY s.segment_id) INTO segments
			FROM gp.cluster_segments() s;
			IF segments IS NULL THEN
				RAISE EXCEPTION 'the cluster has no segments';
			END IF;
			IF coordinator THEN
				segments := ARRAY[0] || segments;
			END IF;
			INSERT INTO gp.colocation_groups (key_types, nbuckets, map)
			SELECT types, buckets,
				   array_agg(segments[b % cardinality(segments) + 1] ORDER BY b)
			FROM generate_series(0, buckets - 1) b
			RETURNING colocation_id INTO group_id;
		END IF;
	END IF;

	INSERT INTO gp.distribution_policy (relid, kind, key_columns, colocation_id)
	VALUES (relation, 'hash', attnums, group_id);
	PERFORM gp.policy_changed(relation);
END
$$;

REVOKE ALL ON FUNCTION gp.distribute(regclass, name[], regclass, int, bool)
FROM PUBLIC;

-- Distribute a table by the ranges of one key column: bounds[i] ends the
-- bucket i and starts the bucket i + 1, every bucket of which goes to
-- segments[i].  The bounds are the texts of the type of the key, and must
-- ascend strictly.  A row whose key is NULL, or that no range covers
-- through its upper bounds, has no bucket: such a row cannot be inserted
CREATE FUNCTION gp.distribute_range(relation regclass, key name,
									bounds text[], segments int[])
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	attnum int2;
	typreg regtype;
	typname text;
	nonempty bool;
	n int;
	i int;
	bad bool;
BEGIN
	IF current_setting('gp.role', true) IS DISTINCT FROM 'coordinator' THEN
		RAISE EXCEPTION 'tables are distributed on the coordinator only';
	END IF;
	IF (SELECT c.relkind NOT IN ('r', 'p') OR c.relpersistence = 't'
		FROM pg_class c WHERE c.oid = relation) THEN
		RAISE EXCEPTION '"%" is not a permanent table', relation;
	END IF;
	IF EXISTS (SELECT FROM gp.distribution_policy p WHERE p.relid = relation)
	THEN
		RAISE EXCEPTION 'table "%" is distributed already', relation;
	END IF;
	EXECUTE format('SELECT EXISTS (SELECT FROM %s)', relation) INTO nonempty;
	IF nonempty THEN
		RAISE EXCEPTION 'table "%" is not empty', relation
			USING HINT = 'Distribute a table before filling it.';
	END IF;
	IF cardinality(bounds) < 1 THEN
		RAISE EXCEPTION 'a range distribution has at least one bound';
	END IF;
	IF cardinality(segments) <> cardinality(bounds) + 1 THEN
		RAISE EXCEPTION
			'a range of % bounds makes % buckets, but % segments are named',
			cardinality(bounds), cardinality(bounds) + 1,
			cardinality(segments);
	END IF;

	SELECT a.attnum, a.atttypid::regtype, format_type(a.atttypid, a.atttypmod)
	INTO attnum, typreg, typname
	FROM pg_attribute a
	WHERE a.attrelid = relation AND a.attname = key AND a.attnum > 0 AND
		  NOT a.attisdropped;
	IF attnum IS NULL THEN
		RAISE EXCEPTION 'column "%" of table "%" does not exist', key, relation;
	END IF;
	IF NOT EXISTS (
		SELECT FROM pg_opclass c, pg_am m
		WHERE c.opcmethod = m.oid AND m.amname = 'btree' AND
			  c.opcintype = typreg::oid AND c.opcdefault
	) THEN
		RAISE EXCEPTION
			'the key of a range distribution needs a type with a default opclass of btree';
	END IF;

	-- A unique index without the whole key cannot tell two equal rows
	-- apart when they sit on different segments
	IF EXISTS (
		SELECT FROM pg_index i
		WHERE i.indrelid = relation AND i.indisunique AND
			  NOT (ARRAY[attnum] <@ i.indkey::pg_catalog.int2[])
	) THEN
		RAISE EXCEPTION
			'the unique indexes of table "%" must include the key of the distribution',
			relation
			USING HINT = 'Rows with equal values may sit on different segments; distribute by a column of every unique index.';
	END IF;

	n := cardinality(bounds);
	FOR i IN 1 .. n - 1 LOOP
		EXECUTE format('SELECT %L::%s >= %L::%s',
					   bounds[i], typname, bounds[i + 1], typname)
		INTO bad;
		IF bad THEN
			RAISE EXCEPTION 'the bounds of the range distribution must ascend strictly';
		END IF;
	END LOOP;
	FOREACH i IN ARRAY segments LOOP
		IF NOT EXISTS (SELECT FROM gp.cluster_segments() s
					   WHERE s.segment_id = i) THEN
			RAISE EXCEPTION 'segment % is not registered', i;
		END IF;
	END LOOP;

	INSERT INTO gp.distribution_policy (relid, kind, key_columns)
	VALUES (relation, 'range', ARRAY[attnum]);
	INSERT INTO gp.distribution_rules (relid, bucket, segment, lower, upper)
	SELECT relation, b - 1, segments[b], bounds[b - 1],
		   CASE WHEN b <= n THEN bounds[b] END
	FROM generate_series(1, n + 1) b;
	PERFORM gp.policy_changed(relation);
END
$$;

REVOKE ALL ON FUNCTION gp.distribute_range(regclass, name, text[], int[])
FROM PUBLIC;

-- Distribute a table by the values of one key column: the value of
-- values[i] makes the bucket i, which goes to segments[i].  A row whose
-- key is NULL, or that matches no value, has no bucket: such a row cannot
-- be inserted
CREATE FUNCTION gp.distribute_list(relation regclass, key name,
								   vals text[], segments int[])
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	attnum int2;
	typreg regtype;
	typname text;
	nonempty bool;
	i int;
	dupes int;
BEGIN
	IF current_setting('gp.role', true) IS DISTINCT FROM 'coordinator' THEN
		RAISE EXCEPTION 'tables are distributed on the coordinator only';
	END IF;
	IF (SELECT c.relkind NOT IN ('r', 'p') OR c.relpersistence = 't'
		FROM pg_class c WHERE c.oid = relation) THEN
		RAISE EXCEPTION '"%" is not a permanent table', relation;
	END IF;
	IF EXISTS (SELECT FROM gp.distribution_policy p WHERE p.relid = relation)
	THEN
		RAISE EXCEPTION 'table "%" is distributed already', relation;
	END IF;
	EXECUTE format('SELECT EXISTS (SELECT FROM %s)', relation) INTO nonempty;
	IF nonempty THEN
		RAISE EXCEPTION 'table "%" is not empty', relation
			USING HINT = 'Distribute a table before filling it.';
	END IF;
	IF cardinality(vals) < 1 THEN
		RAISE EXCEPTION 'a list distribution has at least one value';
	END IF;
	IF cardinality(segments) <> cardinality(vals) THEN
		RAISE EXCEPTION
			'a list of % values makes % buckets, but % segments are named',
			cardinality(vals), cardinality(vals), cardinality(segments);
	END IF;

	SELECT a.attnum, a.atttypid::regtype, format_type(a.atttypid, a.atttypmod)
	INTO attnum, typreg, typname
	FROM pg_attribute a
	WHERE a.attrelid = relation AND a.attname = key AND a.attnum > 0 AND
		  NOT a.attisdropped;
	IF attnum IS NULL THEN
		RAISE EXCEPTION 'column "%" of table "%" does not exist', key, relation;
	END IF;
	IF NOT EXISTS (
		SELECT FROM pg_opclass c, pg_am m
		WHERE c.opcmethod = m.oid AND m.amname = 'btree' AND
			  c.opcintype = typreg::oid AND c.opcdefault
	) THEN
		RAISE EXCEPTION
			'the key of a list distribution needs a type with a default opclass of btree';
	END IF;

	IF EXISTS (
		SELECT FROM pg_index i
		WHERE i.indrelid = relation AND i.indisunique AND
			  NOT (ARRAY[attnum] <@ i.indkey::pg_catalog.int2[])
	) THEN
		RAISE EXCEPTION
			'the unique indexes of table "%" must include the key of the distribution',
			relation
			USING HINT = 'Rows with equal values may sit on different segments; distribute by a column of every unique index.';
	END IF;

	EXECUTE 'SELECT count(*) - count(DISTINCT v) '
			'FROM unnest($1::pg_catalog.text[]::' || typname || '[]) v'
	INTO dupes
	USING vals;
	IF dupes > 0 THEN
		RAISE EXCEPTION 'the values of a list distribution must differ';
	END IF;
	FOREACH i IN ARRAY segments LOOP
		IF NOT EXISTS (SELECT FROM gp.cluster_segments() s
					   WHERE s.segment_id = i) THEN
			RAISE EXCEPTION 'segment % is not registered', i;
		END IF;
	END LOOP;

	INSERT INTO gp.distribution_policy (relid, kind, key_columns)
	VALUES (relation, 'list', ARRAY[attnum]);
	INSERT INTO gp.distribution_rules (relid, bucket, segment, value)
	SELECT relation, o - 1, segments[o], vals[o]
	FROM generate_series(1, cardinality(vals)) o;
	PERFORM gp.policy_changed(relation);
END
$$;

REVOKE ALL ON FUNCTION gp.distribute_list(regclass, name, text[], int[])
FROM PUBLIC;

-- A dropped table loses its policy, lest a new table get its OID and the
-- policy with it.  Defined as SECURITY DEFINER: a user that drops a table
-- of its own has no business with the policy tables of the extension.
CREATE FUNCTION gp.drop_distribution_policy()
RETURNS event_trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
	DELETE FROM gp.distribution_policy p
	USING pg_event_trigger_dropped_objects() d
	WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND
		  p.relid = d.objid;
END
$$;

CREATE EVENT TRIGGER gp_drop_distribution_policy ON sql_drop
EXECUTE FUNCTION gp.drop_distribution_policy();
