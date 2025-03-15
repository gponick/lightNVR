#!/bin/bash

docker rm lightnvr
docker run -d \
  --name lightnvr \
  -p 8080:8080 \
  -v $(realpath .)/config/lightnvr.conf:/etc/lightnvr.conf \
  -v $(realpath .)/recordings:/var/lib/lightnvr/recordings \
  lightnvr:latest && \
  docker logs -f lightnvr