#!/bin/bash
set -eux -o pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)
COUCHBASE_CONTAINER_NAME=couchbase

cd $SCRIPT_DIR
$SCRIPT_DIR/start_server.sh ${COUCHBASE_CONTAINER_NAME}

mkdir -p build

if [[ ! -d build/sync_gateway ]]; then
    cd build && git clone https://github.com/couchbase/sync_gateway && cd -
fi

BUCKET_NAME=testBucket

SCOPE="flowers"
COLLECTIONS=(roses tulips lavenders)
docker exec ${COUCHBASE_CONTAINER_NAME} couchbase-cli bucket-create --cluster "http://127.0.0.1" --username Administrator --password password --bucket ${BUCKET_NAME} --bucket-type couchbase --bucket-ramsize 200
docker exec ${COUCHBASE_CONTAINER_NAME} couchbase-cli collection-manage --cluster "http://127.0.0.1" --username Administrator --password password --bucket ${BUCKET_NAME} --create-scope ${SCOPE}
for collection in ${COLLECTIONS}; do
    docker exec ${COUCHBASE_CONTAINER_NAME} couchbase-cli collection-manage --cluster "http://127.0.0.1" --username Administrator --password password --bucket ${BUCKET_NAME} --create-collection ${SCOPE}.${collection}
done

cd build/sync_gateway && go build -tags cb_sg_enterprise -o build ./...

cd ${SCRIPT_DIR} && cp build/sync_gateway/build/sync_gateway .

cd $(git rev-parse --show-toplevel)
mkdir -p build_cmake/unix
cd build_cmake/unix
# Use whatever compiler you have installed
cmake ../..
# And a reasonable number (# of cores?) for the j flag
make -j8 CppTests

cd ${SCRIPT_DIR}

pkill sync_gateway || true
${SCRIPT_DIR}/sg_setup.sh ${BUCKET_NAME} --no-tls

cd $(git rev-parse --show-toplevel)
env NOTLS=true ./build_cmake/unix/LiteCore/tests/CppTests "[.SyncServerCollection]"
