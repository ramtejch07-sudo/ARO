# Build stage — full compiler toolchain, only needed to produce the binary
FROM gcc:13 AS build
WORKDIR /app
COPY include/ include/
COPY src/ src/
COPY server/ server/
RUN g++ -std=c++17 -O2 -static-libgcc -static-libstdc++ -Iinclude -Iserver \
    server/ApiServer.cpp server/Database.cpp \
    src/Location.cpp src/Graph.cpp src/RouteOptimizer.cpp \
    -o ambulance_server -lpthread

# Runtime stage — small final image. The binary is statically linked against
# libstdc++/libgcc above, so it only needs glibc (libc/libm), which this
# base image always has — no "missing shared library" crash at startup.
FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/ambulance_server .
COPY web/ web/
RUN mkdir -p data
EXPOSE 8080
CMD ["./ambulance_server"]
