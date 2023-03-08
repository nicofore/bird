FROM ubuntu:latest

WORKDIR /root


RUN apt-get update && apt-get install -qy git autoconf libtool gawk make \
flex bison libncurses-dev libreadline6-dev iproute2

ADD src src

RUN cd src && autoreconf -i && ./configure && make && make install

RUN tail -f /dev/null
