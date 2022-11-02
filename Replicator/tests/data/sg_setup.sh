# Starts SG and runs necessary setup for ReplicatorCollectionSGTest
# DB must be set up before this with appropriate user and collection
# Enter the bucket name (raw, not enclosed by quotes) as the argument
#!/bin/bash
set -m # Enable bash job management

if [ $# -lt 1 ]
then
  echo 'You must provide the bucket name as an argument.'
  exit 1
fi

sync_gateway config.json & # Run SG in background

# Give SG time to start before submitting curl requests
sleep 10

curl -k --location --request PUT "https://localhost:4985/scratch/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw "{\"num_index_replicas\": 0, \"bucket\": \"$1\", \"scopes\": {\"flowers\": {\"collections\":{\"roses\":{}}}}}"

curl -k --location --request POST "https://localhost:4985/scratch/_user/" \
 --header "Content-Type: application/json" \
 --header "Authorization: Basic QWRtaW5pc3RyYXRvcjpwYXNzd29yZA==" \
 --data-raw '{"name": "sguser", "password": "password", "admin_channels": ["*"]}'

echo '---------- Sync Gateway setup complete! ----------'
fg # Bring SG back to the foreground
