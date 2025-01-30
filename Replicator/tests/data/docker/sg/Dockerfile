FROM golang:1.23 AS build
RUN apt update
RUN git clone -b release/anemone https://github.com/couchbase/sync_gateway.git
RUN SG_EDITION=CE ./sync_gateway/build.sh

FROM ubuntu:22.04

ARG SG_DEB_ARM64
ARG SG_DEB_AMD64
ARG SSL=true
ARG LEGACY_MODE=false

RUN [ -z "$SG_DEB_ARM64" ] && echo "SG_DEB_ARM64 is required" && exit 1 || true
RUN [ -z "$SG_DEB_AMD64" ] && echo "SG_DEB_AMD64 is required" && exit 1 || true

RUN apt -yqq update
RUN apt -yqq install curl

RUN mkdir -p /opt/sg && \
    ARCHITECTURE="$(dpkg --print-architecture)" && \
    if [ "$ARCHITECTURE" = "amd64" ]; then \
    curl -o /opt/sg/couchbase-sync-gateway.deb $SG_DEB_AMD64; \
    elif [ "$ARCHITECTURE" = "arm64" ]; then \
    curl -o /opt/sg/couchbase-sync-gateway.deb $SG_DEB_ARM64; \
    else \
    echo "Unsupported architecture"; \
    exit 1; \
    fi

COPY cert /opt/sg/cert
COPY config /opt/sg/config
COPY scripts /opt/sg/scripts

WORKDIR /opt/sg

RUN apt update
RUN apt -yqq install systemctl
RUN dpkg -i ./couchbase-sync-gateway.deb

COPY --from=build /go/sync_gateway/bin/sync_gateway_ce /opt/couchbase-sync-gateway/bin/sync_gateway

EXPOSE 4984
EXPOSE 4985
EXPOSE 4884
EXPOSE 4885

ENV SSL=${SSL}
ENV LEGACY_MODE=${LEGACY_MODE}

CMD ./scripts/start.sh ${SSL} ${LEGACY_MODE}
