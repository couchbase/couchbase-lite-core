FROM ubuntu:22.04

ARG SG_DEB
ARG SSL=true
ARG LEGACY_MODE=false

RUN [ -z "$SG_DEB" ] && echo "SG_DEB is required" && exit 1 || true

ADD $SG_DEB /opt/sg/couchbase-sync-gateway.deb

COPY cert /opt/sg/cert
COPY config /opt/sg/config
COPY scripts /opt/sg/scripts

WORKDIR /opt/sg

RUN apt -yqq update 
RUN apt -yqq install systemctl curl
RUN apt -yqq install ./couchbase-sync-gateway.deb

EXPOSE 4984
EXPOSE 4985

ENV SSL=${SSL}
ENV LEGACY_MODE=${LEGACY_MODE}

CMD ./scripts/start.sh ${SSL} ${LEGACY_MODE}
