# vi:filetype=perl

use lib 'lib';
use Test::Nginx::LWP;

plan tests => $Test::Nginx::LWP::RepeatEach * 2 * blocks();

run_tests();

__DATA__

=== TEST 1: sanity
--- config
    location /mysql {
        drizzle;
        drizzle_host localhost;
        drizzle_user monty;
        drizzle_pass some_pass;
        drizzle_db   test;
        drizzle_sql  $request_body;
        drizzle_port  3306;
    }
--- request
POST /mysql
select * from cats;
--- response_body
