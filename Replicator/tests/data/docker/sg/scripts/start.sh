#!/bin/sh

if [ "$#" -lt 2 ]; then
  echo "Usage: start.sh <SSL: true | false> <LEGACY_MODE: true | false>" >&2
  exit 1
fi

wait_for_uri() {
  expected=$1
  shift
  uri=$1
  echo "Waiting for $uri to be available ..."
  while true; do
    status=$(curl -k -s -w "%{http_code}" -o /dev/null $*)
    if [ "x$status" = "x$expected" ]; then
      break
    fi
    echo "$uri not up yet, waiting 5 seconds ..."
    sleep 5
  done
  echo "$uri is ready ..."
}

SSL=$1
LEGACY_MODE=$2

SCRIPT=$(readlink -f "$0")
SCRIPT_DIR=$(dirname "${SCRIPT}")
CONFIG_BASE_DIR=$SCRIPT_DIR/../config

SG_URL_SCHEME="https"
BOOTSTRAP_CONFIG_FILE="bootstrap.json" 
if [ "${SSL}" = "false" ]; then
  SG_URL_SCHEME="http" 
  BOOTSTRAP_CONFIG_FILE="bootstrap-nonssl.json" 
fi

SG_PORT=4984
SG_ADMIN_PORT=4985
SG_DB_NAME="scratch"
CONFIG_DIR="collections"
if [ "${LEGACY_MODE}" = "true" ]; then
  SG_PORT=4884
  SG_ADMIN_PORT=4885  
  SG_DB_NAME="scratch-30"
  CONFIG_DIR="legacy"
fi

# Stop current SG service:
echo "Stop running sync-gateway service ..."
systemctl stop sync_gateway

# Start SG in background:
BOOTSTRAP_CONFIG=${CONFIG_BASE_DIR}/${CONFIG_DIR}/${BOOTSTRAP_CONFIG_FILE}
echo "Start sync-gateway with config : ${BOOTSTRAP_CONFIG} ..."

nohup /opt/couchbase-sync-gateway/bin/sync_gateway "${BOOTSTRAP_CONFIG}" &

# Wait for SG to be ready:
echo "Wait for sync-gateway to be ready ..."
sleep 10
wait_for_uri 200 ${SG_URL_SCHEME}://localhost:${SG_PORT}

# Configure SG:
SG_CONFIG=${CONFIG_BASE_DIR}/${CONFIG_DIR}/config.json
echo "Configure sync-gateway with config : ${SG_CONFIG}"

curl -k --silent --location --request PUT ${SG_URL_SCHEME}://localhost:${SG_ADMIN_PORT}/${SG_DB_NAME}/ \
--user "admin:password" \
--header "Content-Type: application/json" \
--data @"${SG_CONFIG}"

# Keep the container alive
tail -f /dev/null
