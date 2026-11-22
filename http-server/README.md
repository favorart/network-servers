# HTTP server 

Technosphere.mail.ru 2015

using libevent

console arguments:
  + -h/--ip   [server ip adress]
  + -p/--port [listening port]
  + -d/--dir  [folder with files]
  + -w/--workers [number of worker processes, 1..16]
  + -v/--version [print the version and exit]

supports

 1. requests:
    +  GET (html/jpeg files),
    +  HEAD,
    +  POST (just 200 answer)
 2. responses:
    +  200
    +  404

build (Linux, libevent):

    make            # build/http-server
    make SAN=1      # build-san/http-server with ASan and UBSan
    make test       # cmocka unit tests and an integration test (needs cmocka)
    make test SAN=1 # the same under ASan and UBSan
