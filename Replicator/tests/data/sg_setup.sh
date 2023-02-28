# Starts SG and runs necessary setup for ReplicatorCollectionSGTest.
# DB must be set up before this with appropriate user and collection.
# Enter the bucket name (raw, not enclosed by quotes) as the argument.
# If you wish to disable TLS (between SG and CBL), provide the second 
# argument as "-notls".

#!/bin/bash
set -m # Enable bash job management

# If the first argument is "-h" or there are 0 arguments
if [ $# -eq 0 ] || [ $1 == "-h" ]; then
  echo 'Usage: sg_setup.sh <bucket name> [-n][-e <exec name>]'
  echo '-n|--notls: Disables TLS for CBL > SG communication'
  echo '-e <exec name>: Provide the executable name for SG (must still be available on the PATH)'
  exit 0
fi

BUCKET_NAME=$1
shift
PROTOCOL="https"
SG_EXEC="sync_gateway"
CONFIG="config.json"

# Handle optional arguments
while test $# -gt 0
do
  case $1 in
    -n|--notls) 
      PROTOCOL="http"
      CONFIG="config-no-tls.json"
      ;;
    -e)
      SG_EXEC="$2"
      shift
      ;;
    *)
      echo "UNKNOWN ARGUMENT"
      exit 1
      ;;
  esac
  shift
done


CURL_DB="curl -kL -X PUT '$PROTOCOL://localhost:4985/scratch/' \
 -H 'Content-Type: application/json' \
 -H 'Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==' \
 --data-raw '{\"num_index_replicas\": 0, \"bucket\": \"$BUCKET_NAME\"}'"

# Start SG and put the process in background
$SG_EXEC $CONFIG &

# Wait for SG to go up
attempts=20
while [ $attempts -gt 0 ]
do
  sleep 1
  # Attempt to get DB (waiting for SG DB to be UP)
  sgup=$(curl -sIkL -X GET "$PROTOCOL://localhost:4985/scratch/" \
 -H "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" | head -n1 | cut -d$' ' -f2)

  code=${sgup:-0}

  # If db is up, exit loop
  # If db does not exist ('403'), create it

  if [ $code == 200 ]; then
    break
  elif [ $code == 403 ]; then
    eval $CURL_DB
  fi

  attempts=$((attempts-1))
done


# If we couldn't connect to SG, exit and kill SG
if [ $attempts -eq 0 ] && [ $code -ne 200 ]; then
  echo "ERROR: Could not connect to SG after 20 attempts!" 
  kill %%
  exit 1
fi


echo '---------- Sync Gateway setup complete! ----------'
fg # Bring SG back to the foreground
