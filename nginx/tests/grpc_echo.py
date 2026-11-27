#!/usr/bin/env python3
"""A gRPC origin and client for the interception tests (needs grpcio).

    grpc_echo.py server <tls port> <plaintext port> <cert> <key>
    grpc_echo.py client <host:port> <proxy> <method> [ca]

The server answers, on both ports, without generated code:
  /test.Echo/Say      unary: the request back                -> OK (0)
  /test.Echo/Missing  unary: aborts                           -> NOT_FOUND (5)
  /test.Echo/Many     server streaming: the request, 3 times  -> OK (0)
  /test.Echo/Broken   server streaming: once, then aborts     -> UNAVAILABLE (14),
                      in trailers after data

The client goes through <proxy> (http://host:port, an HTTP CONNECT proxy) the
way a gRPC app with HTTPS_PROXY does; with [ca] the target is gRPC over TLS
trusting that CA, without it plaintext HTTP/2. It prints "<code> <reply>".
"""
import sys
from concurrent import futures

import grpc


def say(request, context):
    return request


def missing(request, context):
    context.abort(grpc.StatusCode.NOT_FOUND, "no such thing")


def many(request, context):
    for _ in range(3):
        yield request


def broken(request, context):
    yield request
    context.abort(grpc.StatusCode.UNAVAILABLE, "went away")


def server(tls_port, plain_port, cert, key):
    handler = grpc.method_handlers_generic_handler("test.Echo", {
        "Say": grpc.unary_unary_rpc_method_handler(say),
        "Missing": grpc.unary_unary_rpc_method_handler(missing),
        "Many": grpc.unary_stream_rpc_method_handler(many),
        "Broken": grpc.unary_stream_rpc_method_handler(broken),
    })
    s = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    s.add_generic_rpc_handlers((handler,))
    with open(cert, "rb") as c, open(key, "rb") as k:
        creds = grpc.ssl_server_credentials([(k.read(), c.read())])
    s.add_secure_port("127.0.0.1:%s" % tls_port, creds)
    s.add_insecure_port("127.0.0.1:%s" % plain_port)
    s.start()
    s.wait_for_termination()


def client(target, proxy, method, ca=None):
    opts = [("grpc.enable_http_proxy", 1), ("grpc.http_proxy", proxy)]
    if ca:
        with open(ca, "rb") as f:
            creds = grpc.ssl_channel_credentials(root_certificates=f.read())
        channel = grpc.secure_channel(target, creds, options=opts)
    else:
        channel = grpc.insecure_channel(target, options=opts)
    path = "/test.Echo/" + method
    try:
        if method in ("Many", "Broken"):
            replies = list(channel.unary_stream(path)(b"hi", timeout=10))
            reply = b"".join(replies)
        else:
            reply = channel.unary_unary(path)(b"hi", timeout=10)
        print("0", reply.decode())
    except grpc.RpcError as e:
        print(e.code().value[0], e.details())
    channel.close()


if __name__ == "__main__":
    if sys.argv[1] == "server":
        server(*sys.argv[2:6])
    else:
        client(*sys.argv[2:])
