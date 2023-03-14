# Starts SG and runs necessary setup for ReplicatorCollectionSGTest.
# DB must be set up before this with appropriate user and collection.
# Enter the bucket name (raw, not enclosed by quotes) as the argument.
# If you wish to disable TLS (between SG and CBL), provide the second 
# argument as "-notls".

#!/bin/bash
set -m # Enable bash job management

# If the first argument is "-h" or there are 0 arguments
if [ $# -eq 0 ] || [ $1 == "-h" ]
then
  echo 'Usage: sg_setup.sh <bucket name> [-notls]'
  echo '-notls: Disables TLS for CBL > SG communication'
  exit
fi

# Option to disable TLS by providing a second argument, "-notls"
if [ $2 == "-notls" ]
then
  sync_gateway config-no-tls.json & # Run SG in background with no TLS
  PROTOCOL="http"
else
  sync_gateway config.json & # Run SG in background with TLS
  PROTOCOL="https"
fi

# Give SG time to start before submitting curl requests
sleep 10

echo $PROTOCOL

sync='"sync":"function(doc,olddoc){channel(doc.channels)}"'

curl -k --location --request PUT "$PROTOCOL://localhost:4985/scratch/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw "{\"num_index_replicas\": 0, \"bucket\": \"$1\", \"scopes\": 
{\"flowers\": {\"collections\":{\"roses\":{${sync}}, \"tulips\":{${sync}}, 
\"lavenders\":{${sync}}}}}}"

curl -k --location --request POST "$PROTOCOL://localhost:4985/scratch/_user/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw '{"name": "sguser", "password": "password", "collection_access": 
{"flowers": {"roses": {"admin_channels": ["*"]}, "tulips": 
{"admin_channels": ["*"]}, "lavenders": {"admin_channels": ["*"]}}}}'

echo '---------- Sync Gateway setup complete! ----------'
fg # Bring SG back to the foreground
