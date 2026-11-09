# Build stage — full compiler toolchain, only needed to produce the binary
FROM gcc:13 AS build
WORKDIR /app
COPY include/ include/
COPY src/ src/
COPY server/ server/
# Core sources = everything in src/ except the console demo's main.cpp
RUN g++ -std=c++17 -O2 -static-libgcc -static-libstdc++ -Iinclude -Iserver \
    server/ApiServer.cpp server/ApiRoutes.cpp server/Database.cpp server/OsrmRoutingProvider.cpp \
    $(ls src/*.cpp | grep -v '/main.cpp') \
    -o ambulance_server -lpthread

# Runtime stage — small final image. The binary is statically linked against
# libstdc++/libgcc above, so it only needs glibc (libc/libm), which this
# base image always has — no "missing shared library" crash at startup.
FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/ambulance_server .
COPY web/ web/
RUN mkdir -p data
# Render and most hosts inject PORT; the server reads it and falls back to 8080.
ENV PORT=8080
# Real-road routing server. The public demo is plain http (this build has no TLS). Use "off" for graph-only routing.
ENV OSRM_URL=http://router.project-osrm.org
EXPOSE 8080
# The slim image has no curl/wget, so the server binary probes itself (GET /api/health).
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD ["./ambulance_server", "--healthcheck"]
CMD ["./ambulance_server"]
