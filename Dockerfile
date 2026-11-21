# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512"; V4="-march=skylake-avx512 -mprefer-vector-width=256"; else V3=""; V4=""; fi \
 && g++ -std=c++20 -O3 $V3 -fno-plt -static -pthread -o /pipeline /src/main.cpp \
 && g++ -std=c++20 -O3 $V4 -fno-plt -static -pthread -o /pipeline512 /src/main.cpp \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /pipeline /pipeline512 /expand

FROM debian:bookworm-slim
COPY --from=build /pipeline /pipeline512 /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
