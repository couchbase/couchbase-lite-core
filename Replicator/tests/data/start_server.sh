#!/bin/bash

set -eux

DOCKER_IMAGE=couchbase/server:enterprise-7.1.3

COUCHBASE_CONTAINER_NAME=$1

# kill couchbase if it exists
docker kill ${COUCHBASE_CONTAINER_NAME} || true
docker volume rm ${COUCHBASE_CONTAINER_NAME} || true

docker run --rm -d --name ${COUCHBASE_CONTAINER_NAME} -p 8091-8096:8091-8096 -p 11207:11207 -p 11210:11210 -p 11211:11211 -p 18091-18094:18091-18094 --volume 'type=volume,src=couchbase_data,/var/couchbase/var' $DOCKER_IMAGE

# Each retry min wait 5s, max 10s. Retry 20 times with exponential backoff (delay 0), fail at 120s
curl --retry-all-errors --connect-timeout 5 --max-time 10 --retry 20 --retry-delay 0 --retry-max-time 120 'http://127.0.0.1:8091'

# Set up CBS
docker exec ${COUCHBASE_CONTAINER_NAME} couchbase-cli cluster-init --cluster "http://127.0.0.1" --cluster-username Administrator --cluster-password password --cluster-ramsize 3072 --cluster-index-ramsize 3072 --cluster-fts-ramsize 256 --services data,index,query
docker exec ${COUCHBASE_CONTAINER_NAME} couchbase-cli setting-index --cluster "http://127.0.0.1" --username Administrator --password password --index-max-rollback-points 10 --index-threads 4 --index-storage-setting default --index-memory-snapshot-interval 150 --index-stable-snapshot-interval 40
