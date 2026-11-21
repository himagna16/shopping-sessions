# Shopping Sessions entry: one static binary on an empty image.
# The scorers are linux/amd64 Intel Xeon, Cascade Lake or newer. On any other
# architecture (an Apple-silicon laptop) the same file builds a portable binary.
FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then ARCH="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else ARCH=""; fi \
 && g++ -std=c++20 -O3 $ARCH -fno-plt -static -pthread -o /pipeline /src/main.cpp \
 && strip /pipeline

FROM scratch
COPY --from=build /pipeline /pipeline
CMD ["/pipeline"]
