# ChatConnect: C++17, PostgreSQL client, OpenSSL password hashing.
FROM gcc:14-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends libpq-dev libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server.cpp socket.hpp ./
COPY web/static_assets.hpp web/static_assets.hpp
COPY tests/ ./tests/
RUN mkdir -p /out \
    && g++ -std=c++17 -O2 -pthread -I/usr/include/postgresql server.cpp -o /out/ChatConnectWeb -lpq -lcrypto \
    && g++ -std=c++17 -O2 -pthread -I/usr/include/postgresql tests/auth_tests.cpp -o /out/ChatConnectTests -lpq -lcrypto \
    && /out/ChatConnectTests
# Match compiler runtime to avoid GLIBCXX_3.4.32 issues on older Debian images.
FROM gcc:14-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl libpq5 libssl3 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /out/ChatConnectWeb ./ChatConnectWeb
COPY web/ ./web/
RUN mkdir -p /app/uploads && chown 10001:10001 /app/uploads
USER 10001:10001
EXPOSE 8085
CMD ["./ChatConnectWeb"]
