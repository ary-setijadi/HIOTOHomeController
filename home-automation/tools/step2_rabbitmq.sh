#!/bin/sh
# Step 2: make the new board's RabbitMQ match the HIOTO board (vhost /smarthome + user smarthome).
rabbitmqctl add_vhost /smarthome 2>&1
rabbitmqctl add_user smarthome 'Ssm4rt2!' 2>&1
rabbitmqctl set_permissions -p /smarthome smarthome '.*' '.*' '.*' 2>&1
echo "--- vhosts ---"
rabbitmqctl list_vhosts
echo "--- perms on /smarthome ---"
rabbitmqctl list_permissions -p /smarthome
