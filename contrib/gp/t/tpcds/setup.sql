\set ON_ERROR_STOP on
DROP SCHEMA IF EXISTS tpcds CASCADE;
CREATE SCHEMA tpcds;
SET search_path = tpcds;

CREATE FUNCTION rnd(i bigint, salt int) RETURNS int
LANGUAGE sql IMMUTABLE AS $$ SELECT abs(hashint8(i * 1000003 + salt)) $$;

CREATE FUNCTION tpcds_date(k bigint) RETURNS date
LANGUAGE sql IMMUTABLE AS $$ SELECT date '1998-01-01' + (rnd(k, 40) % 2191) $$;

CREATE TABLE store_sales (
    ss_sold_date_sk integer, ss_sold_time_sk integer, ss_item_sk integer,
    ss_customer_sk integer, ss_cdemo_sk integer, ss_hdemo_sk integer,
    ss_addr_sk integer, ss_store_sk integer, ss_promo_sk integer,
    ss_ticket_number bigint, ss_quantity integer, ss_wholesale_cost numeric(7,2),
    ss_list_price numeric(7,2), ss_sales_price numeric(7,2),
    ss_ext_discount_amt numeric(7,2), ss_ext_sales_price numeric(7,2),
    ss_ext_wholesale_cost numeric(7,2), ss_ext_list_price numeric(7,2),
    ss_ext_tax numeric(7,2), ss_coupon_amt numeric(7,2), ss_net_paid numeric(7,2),
    ss_net_paid_inc_tax numeric(7,2), ss_net_profit numeric(7,2)
);
SELECT gp.distribute('store_sales', ARRAY['ss_item_sk']);
CREATE TABLE store_returns (
    sr_item_sk integer, sr_returned_date_sk integer, sr_return_time_sk integer,
    sr_customer_sk integer, sr_cdemo_sk integer, sr_hdemo_sk integer,
    sr_addr_sk integer, sr_store_sk integer, sr_reason_sk integer,
    sr_ticket_number bigint, sr_return_quantity integer, sr_return_amt numeric(7,2),
    sr_return_tax numeric(7,2), sr_return_amt_inc_tax numeric(7,2),
    sr_fee numeric(7,2), sr_return_ship_cost numeric(7,2),
    sr_refunded_cash numeric(7,2), sr_reversed_charge numeric(7,2),
    sr_store_credit numeric(7,2), sr_net_loss numeric(7,2)
);
SELECT gp.distribute('store_returns', ARRAY['sr_item_sk']);
CREATE TABLE catalog_sales (
    cs_sold_date_sk integer, cs_sold_time_sk integer, cs_ship_date_sk integer,
    cs_bill_customer_sk integer, cs_bill_cdemo_sk integer, cs_bill_hdemo_sk integer,
    cs_bill_addr_sk integer, cs_ship_customer_sk integer, cs_ship_cdemo_sk integer,
    cs_ship_hdemo_sk integer, cs_ship_addr_sk integer, cs_call_center_sk integer,
    cs_catalog_page_sk integer, cs_ship_mode_sk integer, cs_warehouse_sk integer,
    cs_item_sk integer, cs_promo_sk integer, cs_order_number bigint,
    cs_quantity integer, cs_wholesale_cost numeric(7,2),
    cs_list_price numeric(7,2), cs_sales_price numeric(7,2),
    cs_ext_discount_amt numeric(7,2), cs_ext_sales_price numeric(7,2),
    cs_ext_wholesale_cost numeric(7,2), cs_ext_list_price numeric(7,2),
    cs_ext_tax numeric(7,2), cs_coupon_amt numeric(7,2), cs_ext_ship_cost numeric(7,2),
    cs_net_paid numeric(7,2), cs_net_paid_inc_tax numeric(7,2),
    cs_net_paid_inc_ship numeric(7,2), cs_net_paid_inc_ship_tax numeric(7,2),
    cs_net_profit numeric(7,2)
);
SELECT gp.distribute('catalog_sales', ARRAY['cs_item_sk']);
CREATE TABLE web_sales (
    ws_sold_date_sk integer, ws_sold_time_sk integer, ws_ship_date_sk integer,
    ws_item_sk integer,
    ws_bill_customer_sk integer, ws_bill_cdemo_sk integer, ws_bill_hdemo_sk integer,
    ws_bill_addr_sk integer, ws_ship_customer_sk integer, ws_ship_cdemo_sk integer,
    ws_ship_hdemo_sk integer, ws_ship_addr_sk integer, ws_web_page_sk integer,
    ws_web_site_sk integer, ws_ship_mode_sk integer, ws_warehouse_sk integer,
    ws_promo_sk integer, ws_order_number bigint, ws_quantity integer,
    ws_wholesale_cost numeric(7,2),
    ws_list_price numeric(7,2), ws_sales_price numeric(7,2),
    ws_ext_discount_amt numeric(7,2), ws_ext_sales_price numeric(7,2),
    ws_ext_wholesale_cost numeric(7,2), ws_ext_list_price numeric(7,2),
    ws_ext_tax numeric(7,2), ws_coupon_amt numeric(7,2), ws_ext_ship_cost numeric(7,2),
    ws_net_paid numeric(7,2), ws_net_paid_inc_tax numeric(7,2),
    ws_net_paid_inc_ship numeric(7,2), ws_net_paid_inc_ship_tax numeric(7,2),
    ws_net_profit numeric(7,2)
);
SELECT gp.distribute('web_sales', ARRAY['ws_item_sk']);
CREATE TABLE customer (
    c_customer_sk integer, c_customer_id char(16),
    c_current_cdemo_sk integer, c_current_hdemo_sk integer, c_current_addr_sk integer,
    c_first_shipto_date_sk integer, c_first_sales_date_sk integer,
    c_salutation char(10), c_first_name char(20), c_last_name char(30),
    c_preferred_cust_flag char(1), c_birth_day integer, c_birth_month integer,
    c_birth_year integer, c_birth_country varchar(20), c_login char(13),
    c_email_address char(50), c_last_review_date char(10)
);
SELECT gp.distribute('customer', ARRAY['c_customer_sk']);
CREATE TABLE customer_address (
    ca_address_sk integer, ca_address_id char(16),
    ca_street_number char(10), ca_street_name varchar(60), ca_street_type char(15),
    ca_suite_number char(10), ca_city varchar(60), ca_county varchar(30),
    ca_state char(2), ca_zip char(10), ca_country varchar(20),
    ca_gmt_offset numeric(5,2), ca_location_type char(20)
);
SELECT gp.distribute('customer_address', ARRAY['ca_address_sk']);
CREATE TABLE customer_demographics (
    cd_demo_sk integer, cd_gender char(1), cd_marital_status char(1),
    cd_education_status char(20), cd_purchase_estimate integer,
    cd_credit_rating char(10), cd_dep_count integer, cd_dep_employed_count integer,
    cd_dep_college_count integer
);
SELECT gp.distribute('customer_demographics', NULL);
CREATE TABLE item (
    i_item_sk integer, i_item_id char(16),
    i_rec_start_date date, i_rec_end_date date, i_item_desc varchar(200),
    i_current_price numeric(7,2), i_wholesale_cost numeric(7,2), i_brand_id char(11),
    i_brand char(50), i_class_id integer, i_class char(50), i_category_id integer,
    i_category char(50), i_manufact_id integer, i_manufact char(50),
    i_size char(20), i_formulation char(20), i_color char(20), i_units char(10),
    i_container char(10), i_manager_id integer, i_product_name char(50)
);
SELECT gp.distribute('item', ARRAY['i_item_sk']);
CREATE TABLE store (
    s_store_sk integer, s_store_id char(16),
    s_rec_start_date date, s_rec_end_date date, s_closed_date_sk integer,
    s_store_name varchar(50), s_number_employees integer, s_floor_space integer,
    s_hours char(20), s_store_manager varchar(40), s_market_id integer,
    s_geography_class varchar(100), s_market_manager varchar(40),
    s_division_id integer, s_division_name varchar(50), s_company_id integer,
    s_company_name varchar(50), s_street_number char(10), s_street_name varchar(60),
    s_street_type char(15), s_suite_number char(10), s_city varchar(60),
    s_county varchar(30), s_state char(2), s_zip char(10), s_country varchar(20),
    s_gmt_offset numeric(5,2), s_tax_precentage numeric(5,2)
);
SELECT gp.distribute('store', ARRAY['s_store_sk']);
CREATE TABLE date_dim (
    d_date_sk integer, d_date_id char(16), d_date date,
    d_month_seq integer, d_week_seq integer, d_quarter_seq integer, d_year integer,
    d_dow integer, d_moy integer, d_dom integer, d_qoy integer, d_fy_year integer,
    d_fy_quarter_seq integer, d_fy_week_seq integer, d_day_name char(9),
    d_quarter_name char(6), d_holiday char(1), d_weekend char(1),
    d_following_holiday char(1), d_first_dom integer, d_last_dom integer,
    d_same_day_lq integer, d_same_day_ly integer, d_current_day char(1),
    d_current_week char(1), d_current_month char(1), d_current_quarter char(1),
    d_current_year char(1)
);
SELECT gp.distribute('date_dim', NULL);
CREATE TABLE promotion (
    p_promo_sk integer, p_promo_id char(16), p_item_sk integer,
    p_start_date_sk integer, p_end_date_sk integer, p_cost numeric(15,2),
    p_response_target integer, p_promo_name char(50), p_channel_dmail char(1),
    p_channel_email char(1), p_channel_catalog char(1), p_channel_tv char(1),
    p_channel_radio char(1), p_channel_press char(1), p_channel_event char(1),
    p_channel_demo char(1), p_channel_details varchar(100), p_purpose char(15),
    p_discount_active char(1)
);
SELECT gp.distribute('promotion', NULL);

CREATE TABLE web_returns (
    wr_returned_date_sk integer, wr_returned_time_sk integer, wr_item_sk integer,
    wr_refunded_customer_sk integer, wr_refunded_cdemo_sk integer,
    wr_refunded_hdemo_sk integer, wr_refunded_addr_sk integer,
    wr_returning_customer_sk integer, wr_returning_cdemo_sk integer,
    wr_returning_hdemo_sk integer, wr_returning_addr_sk integer,
    wr_web_page_sk integer, wr_reason_sk integer, wr_order_number bigint,
    wr_return_quantity integer, wr_return_amt numeric(7,2),
    wr_return_tax numeric(7,2), wr_return_amt_inc_tax numeric(7,2),
    wr_fee numeric(7,2), wr_return_ship_cost numeric(7,2),
    wr_refunded_cash numeric(7,2), wr_reversed_charge numeric(7,2),
    wr_account_credit numeric(7,2), wr_net_loss numeric(7,2)
);
SELECT gp.distribute('web_returns', ARRAY['wr_item_sk']);

CREATE TABLE catalog_returns (
    cr_returned_date_sk integer, cr_returned_time_sk integer, cr_item_sk integer,
    cr_refunded_customer_sk integer, cr_refunded_cdemo_sk integer,
    cr_refunded_hdemo_sk integer, cr_refunded_addr_sk integer,
    cr_returning_customer_sk integer, cr_returning_cdemo_sk integer,
    cr_returning_hdemo_sk integer, cr_returning_addr_sk integer,
    cr_call_center_sk integer, cr_catalog_page_sk integer, cr_ship_mode_sk integer,
    cr_warehouse_sk integer, cr_reason_sk integer, cr_order_number bigint,
    cr_return_quantity integer, cr_return_amount numeric(7,2),
    cr_return_tax numeric(7,2), cr_return_amt_inc_tax numeric(7,2),
    cr_fee numeric(7,2), cr_return_ship_cost numeric(7,2),
    cr_refunded_cash numeric(7,2), cr_reversed_charge numeric(7,2),
    cr_store_credit numeric(7,2), cr_net_loss numeric(7,2)
);
SELECT gp.distribute('catalog_returns', ARRAY['cr_item_sk']);

CREATE TABLE inventory (
    inv_date_sk integer, inv_item_sk integer, inv_warehouse_sk integer,
    inv_quantity_on_hand integer
);
SELECT gp.distribute('inventory', ARRAY['inv_item_sk']);

CREATE TABLE call_center (
    cc_call_center_sk integer, cc_call_center_id char(16),
    cc_rec_start_date date, cc_rec_end_date date, cc_closed_date_sk integer,
    cc_open_date_sk integer, cc_name varchar(50), cc_class varchar(50),
    cc_employees integer, cc_sq_ft integer, cc_hours char(20),
    cc_manager varchar(40), cc_mkt_id integer, cc_mkt_class char(50),
    cc_mkt_desc varchar(100), cc_market_manager varchar(40),
    cc_division integer, cc_division_name varchar(50), cc_company integer,
    cc_company_name char(50), cc_street_number char(10), cc_street_name varchar(60),
    cc_street_type char(15), cc_suite_number char(10), cc_city varchar(60),
    cc_county varchar(30), cc_state char(2), cc_zip char(10), cc_country varchar(20),
    cc_gmt_offset numeric(5,2), cc_tax_percentage numeric(5,2)
);
SELECT gp.distribute('call_center', NULL);

CREATE TABLE catalog_page (
    cp_catalog_page_sk integer, cp_catalog_page_id char(16),
    cp_start_date_sk integer, cp_end_date_sk integer, cp_department varchar(50),
    cp_catalog_number integer, cp_catalog_page_number integer,
    cp_description varchar(100), cp_type varchar(100)
);
SELECT gp.distribute('catalog_page', ARRAY['cp_catalog_page_sk']);

CREATE TABLE household_demographics (
    hd_demo_sk integer, hd_income_band_sk integer, hd_buy_potential char(15),
    hd_dep_count integer, hd_vehicle_count integer
);
SELECT gp.distribute('household_demographics', NULL);

CREATE TABLE income_band (
    ib_income_band_sk integer, ib_lower_bound integer, ib_upper_bound integer
);
SELECT gp.distribute('income_band', NULL);

CREATE TABLE reason (
    r_reason_sk integer, r_reason_id char(16), r_reason_desc char(100)
);
SELECT gp.distribute('reason', NULL);

CREATE TABLE ship_mode (
    sm_ship_mode_sk integer, sm_ship_mode_id char(16),
    sm_type char(30), sm_code char(10), sm_carrier char(20), sm_contract char(20)
);
SELECT gp.distribute('ship_mode', NULL);

CREATE TABLE time_dim (
    t_time_sk integer, t_time_id char(16), t_time integer,
    t_hour integer, t_minute integer, t_second integer, t_am_pm char(2),
    t_shift char(20), t_sub_shift char(20), t_meal_time char(20)
);
SELECT gp.distribute('time_dim', NULL);

CREATE TABLE warehouse (
    w_warehouse_sk integer, w_warehouse_id char(16),
    w_warehouse_name varchar(20), w_warehouse_sq_ft integer,
    w_street_number char(10), w_street_name varchar(60), w_street_type char(15),
    w_suite_number char(10), w_city varchar(60), w_county varchar(30),
    w_state char(2), w_zip char(10), w_country varchar(20), w_gmt_offset numeric(5,2)
);
SELECT gp.distribute('warehouse', NULL);

CREATE TABLE web_page (
    wp_web_page_sk integer, wp_web_page_id char(16),
    wp_rec_start_date date, wp_rec_end_date date, wp_creation_date_sk integer,
    wp_access_date_sk integer, wp_autogen_flag char(1), wp_customer_sk integer,
    wp_url varchar(100), wp_type char(50), wp_char_count integer,
    wp_link_count integer, wp_image_count integer, wp_max_ad_count integer
);
SELECT gp.distribute('web_page', NULL);

CREATE TABLE web_site (
    web_site_sk integer, web_site_id char(16),
    web_rec_start_date date, web_rec_end_date date, web_name varchar(50),
    web_open_date_sk integer, web_close_date_sk integer, web_class varchar(50),
    web_manager varchar(40), web_mkt_id integer, web_mkt_class varchar(50),
    web_mkt_desc varchar(100), web_market_manager varchar(40),
    web_company_id integer, web_company_name char(50),
    web_street_number char(10), web_street_name varchar(60),
    web_street_type char(15), web_suite_number char(10),
    web_city varchar(60), web_county varchar(30), web_state char(2),
    web_zip char(10), web_country varchar(20), web_gmt_offset numeric(5,2),
    web_tax_percentage numeric(5,2)
);
SELECT gp.distribute('web_site', NULL);

INSERT INTO date_dim
SELECT i, 'AAAAAAA' || (1000000 + i)::char(7), date '1998-01-01' + (i - 1),
       (extract(year from date '1998-01-01' + (i - 1))::int - 1998) * 12 +
           extract(month from date '1998-01-01' + (i - 1))::int,
       (i - 1) / 7,
       (extract(year from date '1998-01-01' + (i - 1))::int - 1998) * 4 +
           extract(quarter from date '1998-01-01' + (i - 1))::int,
       1998 + (i - 1) / 365,
       extract(isodow from date '1998-01-01' + (i - 1))::int,
       extract(month from date '1998-01-01' + (i - 1))::int,
       extract(day from date '1998-01-01' + (i - 1))::int,
       extract(quarter from date '1998-01-01' + (i - 1))::int,
       1998 + (i - 1) / 365,
       (rnd(i, 3) % 24), (rnd(i, 2) % 366),
       to_char(date '1998-01-01' + (i - 1), 'Day'),
       extract(year from date '1998-01-01' + (i - 1))::char(4) || 'Q' ||
           extract(quarter from date '1998-01-01' + (i - 1))::int,
       CASE WHEN rnd(i, 5) % 20 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN extract(isodow from date '1998-01-01' + (i - 1)) > 5 THEN 'Y' ELSE 'N' END,
       'N', i, i, i, i, 'Y', 'Y', 'Y', 'Y', 'Y'
FROM generate_series(1, 2191) i;

INSERT INTO customer
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       rnd(i, 10) % 300 + 1, NULL, rnd(i, 11) % 500 + 1,
       rnd(i, 12) % 2191 + 1, rnd(i, 13) % 2191 + 1,
       CASE rnd(i, 14) % 3 WHEN 0 THEN 'Mr.' WHEN 1 THEN 'Ms.' ELSE 'Dr.' END,
       'First' || (rnd(i, 15) % 100), 'Last' || (rnd(i, 16) % 200),
       CASE WHEN rnd(i, 17) % 10 = 0 THEN 'Y' ELSE 'N' END,
       rnd(i, 18) % 28 + 1, rnd(i, 19) % 12 + 1, 1930 + rnd(i, 20) % 60,
       'United States', 'login' || i, 'customer' || i || '@example.com',
       '2000-01-01'
FROM generate_series(1, 1500) i;

INSERT INTO customer_address
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       (rnd(i, 21) % 9999)::char(4), 'Street' || (rnd(i, 22) % 300),
       CASE rnd(i, 23) % 3 WHEN 0 THEN 'St' WHEN 1 THEN 'Ave' ELSE 'Blvd' END,
       NULL,
       'City' || (rnd(i, 24) % 100), 'County' || (rnd(i, 25) % 30),
       (ARRAY['TN', 'CA', 'NY', 'TX', 'FL', 'WA', 'IL', 'AZ'])[rnd(i, 26) % 8 + 1],
       lpad((rnd(i, 27) % 89999 + 10000)::text, 5, '0') || '000',
       'United States', -6.00,
       CASE rnd(i, 28) % 2 WHEN 0 THEN 'residential' ELSE 'business' END
FROM generate_series(1, 500) i;

INSERT INTO customer_demographics
SELECT i,
       CASE rnd(i, 29) % 2 WHEN 0 THEN 'M' ELSE 'F' END,
       CASE rnd(i, 30) % 3 WHEN 0 THEN 'M' WHEN 1 THEN 'S' ELSE 'D' END,
       (ARRAY['Primary', 'Secondary', 'College', 'Advanced Degree', 'Unknown'])[rnd(i, 31) % 5 + 1],
       rnd(i, 32) % 10000, (ARRAY['Good', 'High Risk', 'Low Risk'])[rnd(i, 33) % 3 + 1],
       rnd(i, 34) % 6, rnd(i, 35) % 4, rnd(i, 36) % 3
FROM generate_series(1, 300) i;

INSERT INTO item
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       date '1998-01-01', NULL,
       'Item description number ' || i,
       (rnd(i, 37) % 29000 + 100) / 100.0,
       (rnd(i, 38) % 19000 + 50) / 100.0,
       'BRAND' || lpad((rnd(i, 39) % 40 + 1)::text, 4, '0'),
       'Brand' || (rnd(i, 39) % 40 + 1),
       rnd(i, 41) % 20, 'Class' || (rnd(i, 41) % 20),
       rnd(i, 42) % 10, (ARRAY['Apparel', 'Electronics', 'Home', 'Sports', 'Books',
                               'Jewelry', 'Music', 'Toys', 'Automotive', 'Beauty'])[rnd(i, 42) % 10 + 1],
       rnd(i, 43) % 100 + 1, 'Manufacturer' || (rnd(i, 43) % 100 + 1),
       (ARRAY['S', 'M', 'L', 'XL'])[rnd(i, 44) % 4 + 1],
       'standard', (ARRAY['Black', 'White', 'Red', 'Blue', 'Green'])[rnd(i, 45) % 5 + 1],
       (ARRAY['Each', 'Box', 'Case'])[rnd(i, 46) % 3 + 1],
       (ARRAY['Container', 'Box', 'Wrap'])[rnd(i, 47) % 3 + 1],
       rnd(i, 48) % 40 + 1, 'Product' || i
FROM generate_series(1, 1000) i;

INSERT INTO store
SELECT i, 'AAAAAAA' || lpad(i::text, 9, '0'),
       date '1998-01-01', NULL, NULL,
       'Store' || i, rnd(i, 49) % 200 + 10, rnd(i, 50) % 20000 + 1000,
       '8AM-10PM', 'Manager' || i, rnd(i, 51) % 10,
       'rural', 'MarketManager' || (rnd(i, 52) % 20),
       rnd(i, 53) % 5, 'Division' || (rnd(i, 53) % 5), rnd(i, 54) % 10,
       'Company' || (rnd(i, 54) % 10),
       (rnd(i, 55) % 999)::char(3), 'Main Street', 'St', 'Suite 1',
       'City' || (rnd(i, 56) % 100), 'County' || (rnd(i, 57) % 30),
       (ARRAY['TN', 'CA', 'NY', 'TX', 'FL', 'WA', 'IL', 'AZ'])[rnd(i, 58) % 8 + 1],
       lpad((rnd(i, 59) % 89999 + 10000)::text, 5, '0'),
       'United States', -6.00, (rnd(i, 60) % 800) / 100.0
FROM generate_series(1, 50) i;

INSERT INTO promotion
SELECT i, 'AAAAAAA' || lpad(i::text, 9, '0'), rnd(i, 61) % 1000 + 1,
       rnd(i, 62) % 2191 + 1, rnd(i, 63) % 2191 + 1,
       (rnd(i, 64) % 100000) / 100.0, rnd(i, 65) % 5000 + 100,
       'Promotion' || i,
       CASE WHEN rnd(i, 66) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 67) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 68) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 69) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 70) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 71) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 72) % 2 = 0 THEN 'Y' ELSE 'N' END,
       CASE WHEN rnd(i, 75) % 2 = 0 THEN 'Y' ELSE 'N' END,
       'Channel details ' || i,
       (ARRAY['Sale', 'Promotional', 'Resale', 'Unknown'])[rnd(i, 73) % 4 + 1],
       CASE WHEN rnd(i, 74) % 4 = 0 THEN 'Y' ELSE 'N' END
FROM generate_series(1, 100) i;

INSERT INTO store_sales
SELECT rnd(i, 75) % 2191 + 1, rnd(i, 234) % 1440 + 1, rnd(i, 76) % 1000 + 1,
       rnd(i, 77) % 1500 + 1, rnd(i, 78) % 300 + 1, NULL,
       rnd(i, 79) % 500 + 1, rnd(i, 80) % 50 + 1, rnd(i, 81) % 100 + 1,
       i, rnd(i, 82) % 10 + 1,
       (rnd(i, 83) % 9000 + 100) / 100.0,
       (rnd(i, 84) % 29000 + 200) / 100.0,
       (rnd(i, 85) % 29000 + 200) / 100.0,
       0.00,
       (rnd(i, 85) % 29000 + 200) / 100.0 * (rnd(i, 82) % 10 + 1),
       (rnd(i, 83) % 9000 + 100) / 100.0 * (rnd(i, 82) % 10 + 1),
       (rnd(i, 84) % 29000 + 200) / 100.0 * (rnd(i, 82) % 10 + 1),
       (rnd(i, 86) % 800) / 100.0,
       (rnd(i, 87) % 500) / 100.0,
       (rnd(i, 85) % 29000 + 200) / 100.0 * (rnd(i, 82) % 10 + 1) - (rnd(i, 86) % 800) / 100.0,
       (rnd(i, 85) % 29000 + 200) / 100.0 * (rnd(i, 82) % 10 + 1) + (rnd(i, 88) % 300) / 100.0,
       (rnd(i, 85) % 29000 + 200) / 100.0 * (rnd(i, 82) % 10 + 1) * 0.3 - (rnd(i, 83) % 9000 + 100) / 100.0
FROM generate_series(1, 8000) i;

INSERT INTO store_returns
SELECT rnd(i, 89) % 1000 + 1, rnd(i, 90) % 2191 + 1, NULL,
       rnd(i, 91) % 1500 + 1, rnd(i, 92) % 300 + 1, NULL,
       rnd(i, 93) % 500 + 1, rnd(i, 94) % 50 + 1, NULL,
       i, rnd(i, 95) % 10 + 1,
       (rnd(i, 96) % 2900 + 100) / 100.0,
       (rnd(i, 97) % 280) / 100.0,
       (rnd(i, 96) % 2900 + 100) / 100.0 + (rnd(i, 97) % 280) / 100.0,
       (rnd(i, 98) % 100) / 100.0,
       (rnd(i, 99) % 100) / 100.0,
       (rnd(i, 96) % 2900 + 100) / 100.0,
       (rnd(i, 96) % 2900 + 100) / 100.0,
       (rnd(i, 96) % 2900 + 100) / 100.0,
       (rnd(i, 96) % 2900 + 100) / 100.0
FROM generate_series(1, 2000) i;

INSERT INTO catalog_sales
    (cs_sold_date_sk, cs_sold_time_sk, cs_ship_date_sk,
     cs_bill_customer_sk, cs_bill_cdemo_sk, cs_bill_hdemo_sk, cs_bill_addr_sk,
     cs_ship_customer_sk, cs_ship_cdemo_sk, cs_ship_hdemo_sk, cs_ship_addr_sk,
     cs_call_center_sk, cs_catalog_page_sk, cs_ship_mode_sk, cs_warehouse_sk,
     cs_item_sk, cs_promo_sk, cs_order_number, cs_quantity,
     cs_wholesale_cost, cs_list_price, cs_sales_price,
     cs_ext_discount_amt, cs_ext_sales_price,
     cs_ext_wholesale_cost, cs_ext_list_price,
     cs_ext_tax, cs_coupon_amt, cs_ext_ship_cost,
     cs_net_paid, cs_net_paid_inc_tax,
     cs_net_paid_inc_ship, cs_net_paid_inc_ship_tax, cs_net_profit)
SELECT rnd(i, 100) % 2191 + 1, rnd(i, 235) % 1440 + 1, rnd(i, 141) % 2191 + 1,
       rnd(i, 102) % 1500 + 1, rnd(i, 103) % 300 + 1, NULL,
       rnd(i, 104) % 500 + 1, rnd(i, 105) % 1500 + 1, rnd(i, 106) % 300 + 1, NULL,
       rnd(i, 107) % 500 + 1, rnd(i, 142) % 20 + 1, rnd(i, 143) % 500 + 1,
       rnd(i, 144) % 20 + 1, rnd(i, 145) % 20 + 1,
       rnd(i, 101) % 1000 + 1, rnd(i, 146) % 100 + 1,
       i, rnd(i, 108) % 10 + 1,
       (rnd(i, 109) % 9000 + 100) / 100.0,
       (rnd(i, 110) % 29000 + 200) / 100.0,
       (rnd(i, 111) % 29000 + 200) / 100.0,
       0.00,
       (rnd(i, 111) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1),
       (rnd(i, 109) % 9000 + 100) / 100.0 * (rnd(i, 108) % 10 + 1),
       (rnd(i, 110) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1),
       (rnd(i, 112) % 800) / 100.0,
       (rnd(i, 113) % 500) / 100.0,
       (rnd(i, 114) % 600) / 100.0,
       (rnd(i, 111) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1) - (rnd(i, 114) % 600) / 100.0,
       (rnd(i, 111) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1) + (rnd(i, 115) % 300) / 100.0,
       (rnd(i, 111) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1),
       (rnd(i, 111) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1) + (rnd(i, 115) % 300) / 100.0,
       (rnd(i, 111) % 29000 + 200) / 100.0 * (rnd(i, 108) % 10 + 1) * 0.3 - (rnd(i, 109) % 9000 + 100) / 100.0
FROM generate_series(1, 4000) i;

INSERT INTO web_sales
    (ws_sold_date_sk, ws_sold_time_sk, ws_ship_date_sk, ws_item_sk,
     ws_bill_customer_sk, ws_bill_cdemo_sk, ws_bill_hdemo_sk, ws_bill_addr_sk,
     ws_ship_customer_sk, ws_ship_cdemo_sk, ws_ship_hdemo_sk, ws_ship_addr_sk,
     ws_web_page_sk, ws_web_site_sk, ws_ship_mode_sk, ws_warehouse_sk,
     ws_promo_sk, ws_order_number, ws_quantity,
     ws_wholesale_cost, ws_list_price, ws_sales_price,
     ws_ext_discount_amt, ws_ext_sales_price,
     ws_ext_wholesale_cost, ws_ext_list_price,
     ws_ext_tax, ws_coupon_amt, ws_ext_ship_cost,
     ws_net_paid, ws_net_paid_inc_tax,
     ws_net_paid_inc_ship, ws_net_paid_inc_ship_tax, ws_net_profit)
SELECT rnd(i, 116) % 2191 + 1, rnd(i, 236) % 1440 + 1, rnd(i, 147) % 2191 + 1,
       rnd(i, 117) % 1000 + 1,
       rnd(i, 118) % 1500 + 1, rnd(i, 119) % 300 + 1, NULL,
       rnd(i, 120) % 500 + 1, rnd(i, 121) % 1500 + 1, rnd(i, 122) % 100 + 1,
       rnd(i, 237) % 100 + 1,
       rnd(i, 123) % 500 + 1, rnd(i, 148) % 100 + 1, rnd(i, 149) % 50 + 1,
       rnd(i, 150) % 20 + 1, rnd(i, 151) % 20 + 1,
       rnd(i, 152) % 100 + 1,
       i, rnd(i, 124) % 10 + 1,
       (rnd(i, 125) % 9000 + 100) / 100.0,
       (rnd(i, 126) % 29000 + 200) / 100.0,
       (rnd(i, 127) % 29000 + 200) / 100.0,
       0.00,
       (rnd(i, 127) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1),
       (rnd(i, 125) % 9000 + 100) / 100.0 * (rnd(i, 124) % 10 + 1),
       (rnd(i, 126) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1),
       (rnd(i, 128) % 800) / 100.0,
       (rnd(i, 129) % 500) / 100.0,
       (rnd(i, 130) % 600) / 100.0,
       (rnd(i, 127) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1),
       (rnd(i, 127) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1) + (rnd(i, 131) % 300) / 100.0,
       (rnd(i, 127) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1) + (rnd(i, 130) % 600) / 100.0,
       (rnd(i, 127) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1) + (rnd(i, 131) % 300) / 100.0 + (rnd(i, 130) % 600) / 100.0,
       (rnd(i, 127) % 29000 + 200) / 100.0 * (rnd(i, 124) % 10 + 1) * 0.3 - (rnd(i, 125) % 9000 + 100) / 100.0
FROM generate_series(1, 4000) i;

INSERT INTO web_returns
SELECT rnd(i, 132) % 2191 + 1, NULL, rnd(i, 133) % 1000 + 1,
       rnd(i, 134) % 1500 + 1, rnd(i, 135) % 300 + 1, NULL,
       rnd(i, 136) % 500 + 1, rnd(i, 137) % 1500 + 1, rnd(i, 138) % 300 + 1, NULL,
       rnd(i, 139) % 500 + 1, NULL, NULL,
       i, rnd(i, 140) % 10 + 1,
       (rnd(i, 141) % 2900 + 100) / 100.0,
       (rnd(i, 142) % 280) / 100.0,
       (rnd(i, 141) % 2900 + 100) / 100.0 + (rnd(i, 142) % 280) / 100.0,
       (rnd(i, 143) % 100) / 100.0,
       (rnd(i, 144) % 100) / 100.0,
       (rnd(i, 141) % 2900 + 100) / 100.0,
       (rnd(i, 141) % 2900 + 100) / 100.0,
       (rnd(i, 141) % 2900 + 100) / 100.0,
       (rnd(i, 141) % 2900 + 100) / 100.0
FROM generate_series(1, 1500) i;

INSERT INTO catalog_returns
SELECT rnd(i, 153) % 2191 + 1, NULL, rnd(i, 154) % 1000 + 1,
       rnd(i, 155) % 1500 + 1, rnd(i, 156) % 300 + 1, NULL,
       rnd(i, 157) % 500 + 1, rnd(i, 158) % 1500 + 1, rnd(i, 159) % 300 + 1, NULL,
       rnd(i, 160) % 500 + 1, rnd(i, 161) % 20 + 1, rnd(i, 162) % 500 + 1,
       rnd(i, 163) % 20 + 1, rnd(i, 164) % 20 + 1, rnd(i, 165) % 35 + 1,
       i, rnd(i, 166) % 10 + 1,
       (rnd(i, 167) % 2900 + 100) / 100.0,
       (rnd(i, 168) % 280) / 100.0,
       (rnd(i, 167) % 2900 + 100) / 100.0 + (rnd(i, 168) % 280) / 100.0,
       (rnd(i, 169) % 100) / 100.0,
       (rnd(i, 170) % 100) / 100.0,
       (rnd(i, 167) % 2900 + 100) / 100.0,
       (rnd(i, 167) % 2900 + 100) / 100.0,
       (rnd(i, 167) % 2900 + 100) / 100.0,
       (rnd(i, 167) % 2900 + 100) / 100.0
FROM generate_series(1, 2000) i;

INSERT INTO inventory
SELECT rnd(i, 171) % 2191 + 1, rnd(i, 172) % 1000 + 1,
       rnd(i, 173) % 20 + 1, rnd(i, 174) % 1000
FROM generate_series(1, 12000) i;

INSERT INTO call_center
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       date '1998-01-01', NULL, NULL, rnd(i, 175) % 2191 + 1,
       'CallCenter' || i, 'class' || (rnd(i, 176) % 5),
       rnd(i, 177) % 200 + 10, rnd(i, 178) % 20000 + 1000, '8AM-8PM',
       'Manager' || i, rnd(i, 179) % 10, 'mktclass' || (rnd(i, 176) % 5),
       'Market description ' || i, 'MarketManager' || (rnd(i, 180) % 20),
       rnd(i, 181) % 5, 'Division' || (rnd(i, 181) % 5), rnd(i, 182) % 10,
       'Company' || (rnd(i, 182) % 10),
       (rnd(i, 183) % 999)::char(3), 'CallCenter Street', 'St', 'Suite 1',
       'City' || (rnd(i, 184) % 100), 'County' || (rnd(i, 185) % 30),
       (ARRAY['TN', 'CA', 'NY', 'TX', 'FL', 'WA', 'IL', 'AZ'])[rnd(i, 186) % 8 + 1],
       lpad((rnd(i, 187) % 89999 + 10000)::text, 5, '0'),
       'United States', -6.00, (rnd(i, 188) % 800) / 100.0
FROM generate_series(1, 20) i;

INSERT INTO catalog_page
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       rnd(i, 189) % 2191 + 1, rnd(i, 190) % 2191 + 1,
       'Department' || (rnd(i, 191) % 20),
       rnd(i, 192) % 100 + 1, rnd(i, 193) % 300 + 1,
       'Catalog page description ' || i,
       (ARRAY['regular', 'campaign', 'clearance'])[rnd(i, 194) % 3 + 1]
FROM generate_series(1, 500) i;

INSERT INTO household_demographics
SELECT i, rnd(i, 195) % 20 + 1,
       (ARRAY['1000-2999', '3000-4999', '5000-6999', '7000-8999', '9000+'])[rnd(i, 196) % 5 + 1],
       rnd(i, 197) % 9, rnd(i, 198) % 3
FROM generate_series(1, 100) i;

INSERT INTO income_band
SELECT i, (i - 1) * 5000, i * 5000 - 1
FROM generate_series(1, 20) i;

INSERT INTO reason
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       'Reason description ' || i
FROM generate_series(1, 35) i;

INSERT INTO ship_mode
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       (ARRAY['STANDARD', 'EXPRESS', 'OVERNIGHT', 'ECONOMY', 'AIR', 'TRUCK'])[rnd(i, 199) % 6 + 1],
       'CODE' || (rnd(i, 200) % 100),
       (ARRAY['UPS', 'FEDEX', 'DHL', 'USPS'])[rnd(i, 201) % 4 + 1],
       (ARRAY['REG', 'LTL', 'FCL'])[rnd(i, 202) % 3 + 1]
FROM generate_series(1, 20) i;

INSERT INTO time_dim
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       (i - 1) * 60,
       ((i - 1) * 60) / 3600,
       (((i - 1) * 60) % 3600) / 60,
       0,
       CASE WHEN ((i - 1) * 60) / 3600 < 12 THEN 'AM' ELSE 'PM' END,
       (ARRAY['first', 'second', 'third'])[rnd(i, 203) % 3 + 1],
       (ARRAY['sub1', 'sub2'])[rnd(i, 204) % 2 + 1],
       (ARRAY['breakfast', 'lunch', 'dinner'])[rnd(i, 205) % 3 + 1]
FROM generate_series(1, 1440) i;

INSERT INTO warehouse
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       'Warehouse' || i, rnd(i, 206) % 90000 + 10000,
       (rnd(i, 207) % 999)::char(3), 'Warehouse Street', 'St', 'Suite 1',
       'City' || (rnd(i, 208) % 100), 'County' || (rnd(i, 209) % 30),
       (ARRAY['TN', 'CA', 'NY', 'TX', 'FL', 'WA', 'IL', 'AZ'])[rnd(i, 210) % 8 + 1],
       lpad((rnd(i, 211) % 89999 + 10000)::text, 5, '0'),
       'United States', -6.00
FROM generate_series(1, 20) i;

INSERT INTO web_page
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       date '1998-01-01', NULL, rnd(i, 212) % 2191 + 1, rnd(i, 213) % 2191 + 1,
       CASE WHEN rnd(i, 214) % 2 = 0 THEN 'Y' ELSE 'N' END,
       rnd(i, 215) % 1500 + 1,
       'http://www.example.com/page' || i,
       (ARRAY['static', 'dynamic', 'portal'])[rnd(i, 216) % 3 + 1],
       rnd(i, 217) % 5000 + 100, rnd(i, 218) % 100,
       rnd(i, 219) % 50, rnd(i, 220) % 10 + 1
FROM generate_series(1, 100) i;

INSERT INTO web_site
SELECT i, 'AAAAAAAA' || lpad(i::text, 8, '0'),
       date '1998-01-01', NULL, 'Site' || i,
       rnd(i, 221) % 2191 + 1, rnd(i, 222) % 2191 + 1,
       'class' || (rnd(i, 223) % 5), 'Manager' || i,
       rnd(i, 224) % 10, 'mktclass' || (rnd(i, 225) % 5),
       'Market description ' || i, 'MarketManager' || (rnd(i, 226) % 20),
       rnd(i, 227) % 10, 'Company' || (rnd(i, 227) % 10),
       (rnd(i, 228) % 999)::char(3), 'Web Street', 'St', 'Suite 1',
       'City' || (rnd(i, 229) % 100), 'County' || (rnd(i, 230) % 30),
       (ARRAY['TN', 'CA', 'NY', 'TX', 'FL', 'WA', 'IL', 'AZ'])[rnd(i, 231) % 8 + 1],
       lpad((rnd(i, 232) % 89999 + 10000)::text, 5, '0'),
       'United States', -6.00, (rnd(i, 233) % 800) / 100.0
FROM generate_series(1, 50) i;

ANALYZE store_sales; ANALYZE store_returns; ANALYZE catalog_sales;
ANALYZE web_returns; ANALYZE catalog_returns; ANALYZE inventory;
ANALYZE web_sales; ANALYZE customer; ANALYZE customer_address;
ANALYZE customer_demographics; ANALYZE item; ANALYZE store;
ANALYZE date_dim; ANALYZE promotion; ANALYZE call_center;
ANALYZE catalog_page; ANALYZE household_demographics; ANALYZE income_band;
ANALYZE reason; ANALYZE ship_mode; ANALYZE time_dim; ANALYZE warehouse;
ANALYZE web_page; ANALYZE web_site;

CREATE TABLE local_store_sales AS SELECT * FROM store_sales;
CREATE TABLE local_store_returns AS SELECT * FROM store_returns;
CREATE TABLE local_catalog_sales AS SELECT * FROM catalog_sales;
CREATE TABLE local_catalog_returns AS SELECT * FROM catalog_returns;
CREATE TABLE local_inventory AS SELECT * FROM inventory;
CREATE TABLE local_web_sales AS SELECT * FROM web_sales;
CREATE TABLE local_customer AS SELECT * FROM customer;
CREATE TABLE local_customer_address AS SELECT * FROM customer_address;
CREATE TABLE local_customer_demographics AS SELECT * FROM customer_demographics;
CREATE TABLE local_item AS SELECT * FROM item;
CREATE TABLE local_store AS SELECT * FROM store;
CREATE TABLE local_date_dim AS SELECT * FROM date_dim;
CREATE TABLE local_promotion AS SELECT * FROM promotion;
CREATE TABLE local_call_center AS SELECT * FROM call_center;
CREATE TABLE local_catalog_page AS SELECT * FROM catalog_page;
CREATE TABLE local_household_demographics AS SELECT * FROM household_demographics;
CREATE TABLE local_income_band AS SELECT * FROM income_band;
CREATE TABLE local_reason AS SELECT * FROM reason;
CREATE TABLE local_ship_mode AS SELECT * FROM ship_mode;
CREATE TABLE local_time_dim AS SELECT * FROM time_dim;
CREATE TABLE local_warehouse AS SELECT * FROM warehouse;
CREATE TABLE local_web_page AS SELECT * FROM web_page;
CREATE TABLE local_web_site AS SELECT * FROM web_site;
CREATE TABLE local_web_returns AS SELECT * FROM web_returns;
