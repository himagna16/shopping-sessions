# Shopping Sessions entry: one static binary on an empty image.
# The scorers are linux/amd64 Intel Xeon (Cascade Lake or newer).
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ \
 && rm -rf /var/lib/apt/lists/*
COPY src/ /src/
RUN g++ -std=c++20 -O3 -march=x86-64-v3 -mtune=skylake-avx512 -fno-plt -static -pthread \
        -o /pipeline /src/main.cpp \
 && strip /pipeline

FROM scratch
COPY --from=build /pipeline /pipeline
CMD ["/pipeline"]
