FROM ubuntu:18.04 as build-deps

RUN apt-get update
RUN apt-get install -y build-essential libopus-dev libortp-dev libasound2-dev alsa

RUN mkdir trx
COPY . trx/
WORKDIR trx
RUN make
