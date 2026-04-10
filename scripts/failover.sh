#!/bin/bash

MASTER_NODE=$1
BACKUP_NODE=$2

if [ -z "$MASTER_NODE" ] || [ -z "$BACKUP_NODE" ]; then
    echo "Usage: $0 <master_node> <backup_node>"
    exit 1
fi

echo "Checking health of master node $MASTER_NODE..."

curl -s --connect-timeout 5 "http://$MASTER_NODE:8000/api/health" > /dev/null

if [ $? -ne 0 ]; then
    echo "Master node $MASTER_NODE is down, triggering failover to $BACKUP_NODE..."

    # Update DNS or load balancer configuration
    # This is a placeholder - actual implementation depends on your infrastructure
    echo "Update load balancer configuration to point to $BACKUP_NODE"

    # Send notification
    echo "Failover completed from $MASTER_NODE to $BACKUP_NODE" | mail -s "IPTV Cluster Failover Alert" admin@example.com

    exit 0
else
    echo "Master node $MASTER_NODE is healthy, no action needed"
    exit 0
fi
