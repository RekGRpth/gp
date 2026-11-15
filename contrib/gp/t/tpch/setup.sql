\set ON_ERROR_STOP on
DROP SCHEMA IF EXISTS tpch CASCADE;
CREATE SCHEMA tpch;
SET search_path = tpch;

CREATE TABLE region (
    r_regionkey int NOT NULL,
    r_name char(25) NOT NULL,
    r_comment varchar(152)
);
SELECT gp.distribute('region', NULL);
CREATE TABLE nation (
    n_nationkey int NOT NULL,
    n_name char(25) NOT NULL,
    n_regionkey int NOT NULL,
    n_comment varchar(152)
);
SELECT gp.distribute('nation', NULL);
CREATE TABLE supplier (
    s_suppkey int NOT NULL,
    s_name char(25) NOT NULL,
    s_address varchar(40) NOT NULL,
    s_nationkey int NOT NULL,
    s_phone char(15) NOT NULL,
    s_acctbal numeric(12,2) NOT NULL,
    s_comment varchar(101) NOT NULL
);
SELECT gp.distribute('supplier', ARRAY['s_suppkey']);
CREATE TABLE part (
    p_partkey int NOT NULL,
    p_name varchar(55) NOT NULL,
    p_mfgr char(25) NOT NULL,
    p_brand char(10) NOT NULL,
    p_type varchar(25) NOT NULL,
    p_size int NOT NULL,
    p_container char(10) NOT NULL,
    p_retailprice numeric(12,2) NOT NULL,
    p_comment varchar(23) NOT NULL
);
SELECT gp.distribute('part', ARRAY['p_partkey']);
CREATE TABLE partsupp (
    ps_partkey int NOT NULL,
    ps_suppkey int NOT NULL,
    ps_availqty int NOT NULL,
    ps_supplycost numeric(12,2) NOT NULL,
    ps_comment varchar(199) NOT NULL
);
SELECT gp.distribute('partsupp', ARRAY['ps_partkey']);
CREATE TABLE customer (
    c_custkey int NOT NULL,
    c_name char(25) NOT NULL,
    c_address varchar(40) NOT NULL,
    c_nationkey int NOT NULL,
    c_phone char(15) NOT NULL,
    c_acctbal numeric(12,2) NOT NULL,
    c_mktsegment char(10) NOT NULL,
    c_comment varchar(117) NOT NULL
);
SELECT gp.distribute('customer', ARRAY['c_custkey']);
CREATE TABLE orders (
    o_orderkey bigint NOT NULL,
    o_custkey int NOT NULL,
    o_orderstatus char(1) NOT NULL,
    o_totalprice numeric(12,2) NOT NULL,
    o_orderdate date NOT NULL,
    o_orderpriority char(15) NOT NULL,
    o_clerk char(15) NOT NULL,
    o_shippriority int NOT NULL,
    o_comment varchar(79) NOT NULL
);
SELECT gp.distribute('orders', ARRAY['o_orderkey']);
CREATE TABLE lineitem (
    l_orderkey bigint NOT NULL,
    l_partkey int NOT NULL,
    l_suppkey int NOT NULL,
    l_linenumber int NOT NULL,
    l_quantity numeric(10,2) NOT NULL,
    l_extendedprice numeric(12,2) NOT NULL,
    l_discount numeric(4,2) NOT NULL,
    l_tax numeric(4,2) NOT NULL,
    l_returnflag char(1) NOT NULL,
    l_linestatus char(1) NOT NULL,
    l_shipdate date NOT NULL,
    l_commitdate date NOT NULL,
    l_receiptdate date NOT NULL,
    l_shipinstruct char(25) NOT NULL,
    l_shipmode char(10) NOT NULL,
    l_comment varchar(44) NOT NULL
);
SELECT gp.distribute('lineitem', ARRAY['l_orderkey']);

CREATE FUNCTION rnd(i bigint, salt int) RETURNS int
LANGUAGE sql IMMUTABLE AS $$ SELECT abs(hashint8(i * 1000003 + salt)) $$;
CREATE FUNCTION odate(k bigint) RETURNS date
LANGUAGE sql IMMUTABLE AS $$ SELECT date '1992-01-01' + (rnd(k, 40) % 2406) $$;

INSERT INTO region VALUES
 (0, 'AFRICA', 'region comment'),
 (1, 'AMERICA', 'region comment'),
 (2, 'ASIA', 'region comment'),
 (3, 'EUROPE', 'region comment'),
 (4, 'MIDDLE EAST', 'region comment');
INSERT INTO nation
SELECT i, n, r, 'nation ' || n FROM (VALUES
 (0,'ALGERIA',0),(1,'ARGENTINA',1),(2,'BRAZIL',1),(3,'CANADA',1),(4,'EGYPT',4),
 (5,'ETHIOPIA',0),(6,'FRANCE',3),(7,'GERMANY',3),(8,'INDIA',2),(9,'INDONESIA',2),
 (10,'IRAN',4),(11,'IRAQ',4),(12,'JAPAN',2),(13,'JORDAN',4),(14,'KENYA',0),
 (15,'MOROCCO',0),(16,'MOZAMBIQUE',0),(17,'PERU',1),(18,'CHINA',2),(19,'ROMANIA',3),
 (20,'SAUDI ARABIA',4),(21,'VIETNAM',2),(22,'RUSSIA',3),(23,'UNITED KINGDOM',3),(24,'UNITED STATES',1)
) AS v(i, n, r);
INSERT INTO supplier
SELECT i, 'Supplier#' || lpad(i::text, 9, '0'), md5(i::text || 's'), rnd(i, 1) % 25,
       lpad((rnd(i, 2) % 999999999)::text, 15, '0'),
       ((rnd(i, 3) % 1099999) - 99999) / 100.0,
       left(md5(i::text || 'sc') || md5(i::text || 'sd'), 101)
FROM generate_series(1, 100) i;
INSERT INTO part
SELECT i,
       (ARRAY['almond','antique','aquamarine','azure','beige','bisque','black','blanched','blue','blush'])[rnd(i, 10) % 10 + 1] || ' ' ||
       (ARRAY['brown','burlywood','burnished','chartreuse','chiffon','chocolate','coral','cornflower','cornsilk','cream'])[rnd(i, 11) % 10 + 1] || ' ' ||
       (ARRAY['cyan','dark','deep','dim','dodger','drab','firebrick','floral','forest','frosted'])[rnd(i, 12) % 10 + 1],
       'Manufacturer#' || (rnd(i, 13) % 5 + 1),
       'Brand#' || (rnd(i, 13) % 5 + 1) || (rnd(i, 14) % 5 + 1),
       (ARRAY['STANDARD','SMALL','MEDIUM','LARGE','ECONOMY','PROMO'])[rnd(i, 15) % 6 + 1] || ' ' ||
       (ARRAY['ANODIZED','BURNISHED','PLATED','POLISHED','BRUSHED'])[rnd(i, 16) % 5 + 1] || ' ' ||
       (ARRAY['TIN','NICKEL','BRASS','STEEL','COPPER'])[rnd(i, 17) % 5 + 1],
       rnd(i, 18) % 50 + 1,
       (ARRAY['SM','LG','MED','JUMBO','WRAP'])[rnd(i, 19) % 5 + 1] || ' ' ||
       (ARRAY['CASE','BOX','BAG','JAR','PKG','PACK','CAN','DRUM'])[rnd(i, 20) % 8 + 1],
       (90000 + ((i / 10) % 20001) + 100 * (i % 1000)) / 100.0,
       left(md5(i::text || 'p'), 23)
FROM generate_series(1, 200) i;
INSERT INTO partsupp
SELECT (i - 1) / 4 + 1,
       (((i - 1) / 4 + 1) + ((i - 1) % 4) * 25) % 100 + 1,
       rnd(i, 21) % 9999 + 1, (rnd(i, 22) % 99900 + 100) / 100.0,
       left(md5(i::text || 'ps') || md5(i::text || 'pt'), 199)
FROM generate_series(1, 800) i;
INSERT INTO customer
SELECT i, 'Customer#' || lpad(i::text, 9, '0'), md5(i::text || 'c'), rnd(i, 30) % 25,
       lpad((rnd(i, 31) % 999999999)::text, 15, '0'),
       ((rnd(i, 32) % 1099999) - 99999) / 100.0,
       (ARRAY['AUTOMOBILE','BUILDING','FURNITURE','MACHINERY','HOUSEHOLD'])[rnd(i, 33) % 5 + 1],
       left(md5(i::text || 'cc') || md5(i::text || 'cd') ||
       CASE WHEN rnd(i, 34) % 100 = 0 THEN ' special requests ' ELSE '' END, 117)
FROM generate_series(1, 150) i;
INSERT INTO orders
SELECT i,
       rnd(i, 41) % 150 + 1,
       CASE WHEN odate(i) < date '1995-06-17' THEN 'F' WHEN rnd(i, 42) % 5 = 0 THEN 'P' ELSE 'O' END,
       (rnd(i, 43) % 45000000 + 100000) / 100.0,
       odate(i),
       (ARRAY['1-URGENT','2-HIGH','3-MEDIUM','4-NOT SPECIFIED','5-LOW'])[rnd(i, 44) % 5 + 1],
       'Clerk#' || lpad((rnd(i, 45) % 1000 + 1)::text, 9, '0'),
       0,
       left(md5(i::text || 'oc') || CASE WHEN rnd(i, 46) % 50 = 0 THEN ' special requests ' ELSE '' END, 79)
FROM generate_series(1, 1500) i;
INSERT INTO lineitem
SELECT (i - 1) / 4 + 1,
       rnd(i, 50) % 200 + 1,
       rnd(i, 51) % 100 + 1,
       (i - 1) % 4 + 1,
       rnd(i, 52) % 100 + 1,
       (rnd(i, 52) % 100 + 1) * ((90000 + ((rnd(i, 50) % 200 + 1) % 20001) + 100 * (rnd(i, 50) % 1000)) / 100.0),
       (rnd(i, 53) % 11) / 100.0,
       (rnd(i, 54) % 9) / 100.0,
       CASE WHEN odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1) + (rnd(i, 57) % 30 + 1) <= date '1995-06-17'
            THEN (ARRAY['R','A'])[rnd(i, 58) % 2 + 1] ELSE 'N' END,
       CASE WHEN odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1) > date '1995-06-17' THEN 'O' ELSE 'F' END,
       odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1),
       odate((i - 1) / 4 + 1) + (rnd(i, 56) % 61 + 30),
       odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1) + (rnd(i, 57) % 30 + 1),
       (ARRAY['DELIVER IN PERSON','COLLECT COD','NONE','TAKE BACK RETURN'])[rnd(i, 59) % 4 + 1],
       (ARRAY['REG AIR','AIR','RAIL','SHIP','TRUCK','MAIL','FOB'])[rnd(i, 60) % 7 + 1],
       left(md5(i::text || 'l'), 27)
FROM generate_series(1, 6000) i;
ANALYZE region; ANALYZE nation; ANALYZE supplier; ANALYZE part;
ANALYZE partsupp; ANALYZE customer; ANALYZE orders; ANALYZE lineitem;

CREATE TABLE local_region AS SELECT * FROM region;
CREATE TABLE local_nation AS SELECT * FROM nation;
CREATE TABLE local_supplier AS SELECT * FROM supplier;
CREATE TABLE local_part AS SELECT * FROM part;
CREATE TABLE local_partsupp AS SELECT * FROM partsupp;
CREATE TABLE local_customer AS SELECT * FROM customer;
CREATE TABLE local_orders AS SELECT * FROM orders;
CREATE TABLE local_lineitem AS SELECT * FROM lineitem;
ANALYZE local_region; ANALYZE local_nation; ANALYZE local_supplier; ANALYZE local_part;
ANALYZE local_partsupp; ANALYZE local_customer; ANALYZE local_orders; ANALYZE local_lineitem;
