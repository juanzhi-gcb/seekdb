# Development validation image; final SQL entrypoint follows the protocol adapter.
FROM alpine:3.22.8@sha256:49340b346798875b5592e5d420241f2f9cfba5f47e62ebb1446a1b34734e898f AS build
RUN apk add --no-cache build-base=0.5-r3 cmake=3.31.7-r1 samurai=1.2-r7
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build /build --parallel 2
FROM build AS test
ENV MVDB_TEST_DIR=/data/core-tests
WORKDIR /build
CMD ["ctest", "--test-dir", "/build", "--output-on-failure"]
