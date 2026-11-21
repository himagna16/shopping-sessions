# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512"; V4="-march=skylake-avx512 -mprefer-vector-width=256"; JCC="-Wa,-mbranches-within-32B-boundaries"; else V3=""; V4=""; JCC=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread /src/main.cpp" \
 && $B -O3 $V3 -o /p \
 && $B -O3 $V3 -DSS_UNIT=128 -o /p128 \
 && $B -O3 $V3 -DSS_NO_STREAM -o /pns \
 && $B -O3 $V3 $JCC -o /pjcc \
 && $B -O2 $V3 -o /po2 \
 && $B -O3 $V4 -o /p512 \
 && $B -O3 $V3 -DSS_ID2 -o /pid2 \
 && $B -O3 $V3 $JCC -DSS_ID2 -DSS_UNIT=128 -o /pc \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && g++ -std=c++20 -O2 -o /synth /tools/synth.cpp && /synth /train 2000000 \
 && mkdir /o1 /o2 && C="g++ -std=c++20 -fno-plt -pthread -O3 $V3" \
 && (cd /o1 && $C -fprofile-generate -fprofile-update=atomic -c /src/main.cpp -o main.o && g++ -static -pthread -fprofile-generate main.o -o /g1 \
     && DATA_DIR=/train OUTPUT_DIR=/tmp/t1 /g1 && $C -fprofile-use -fprofile-correction -c /src/main.cpp -o main.o && g++ -static -pthread main.o -o /ppgo) \
 && (cd /o2 && $C $JCC -DSS_ID2 -DSS_UNIT=128 -fprofile-generate -fprofile-update=atomic -c /src/main.cpp -o main.o && g++ -static -pthread -fprofile-generate main.o -o /g2 \
     && DATA_DIR=/train OUTPUT_DIR=/tmp/t2 /g2 && $C $JCC -DSS_ID2 -DSS_UNIT=128 -fprofile-use -fprofile-correction -c /src/main.cpp -o main.o && g++ -static -pthread main.o -o /pcpgo) \
 && strip /p /p128 /pns /pjcc /po2 /p512 /pid2 /pc /ppgo /pcpgo /expand

FROM debian:bookworm-slim
COPY --from=build /p /p128 /pns /pjcc /po2 /p512 /pid2 /pc /ppgo /pcpgo /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
