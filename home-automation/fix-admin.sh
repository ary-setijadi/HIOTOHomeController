#!/bin/sh
set -e
echo "== creating admin user =="
rabbitmqctl add_user admin '123456Aa!'
rabbitmqctl set_user_tags admin administrator
rabbitmqctl set_permissions -p / admin '.*' '.*' '.*'
echo "== user list =="
rabbitmqctl list_users
echo "== done =="
