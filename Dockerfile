# Build stage
FROM gcc:13 AS build
WORKDIR /app
COPY include/ include/
COPY src/ src/
COPY server/ server/
RUN g++ -std=c++17 -O2 -Iinclude -Iserver \
    server/ApiServer.cpp server/Database.cpp \
    src/Location.cpp src/Graph.cpp src/RouteOptimizer.cpp \
    -o ambulance_server -lpthread

# Runtime stage — small final image, only what's needed to run
FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /app/ambulance_server .
COPY web/ web/
RUN mkdir -p data
EXPOSE 8080
CMD ["./ambulance_server"]
