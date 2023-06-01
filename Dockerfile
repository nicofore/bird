FROM ubuntu:latest

WORKDIR /root


RUN apt-get update && apt-get install -qy git autoconf libtool gawk make \
flex bison libncurses-dev libreadline6-dev iproute2

RUN apt-get install -qy gdb

ADD bird bird

RUN cd bird && autoreconf -i && ./configure && make && make install

CMD tail -f /dev/null
