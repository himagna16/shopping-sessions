IMAGE    ?= shopping-sessions:latest
PLATFORM ?= linux/amd64
CXX      ?= c++

.PHONY: native slow image

## Native build for local testing (any CPU)
native:
	mkdir -p build
	$(CXX) -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread src/main.cpp -o build/pipeline

## Same program with every fast path disabled; its output must match `native` exactly
slow:
	mkdir -p build
	$(CXX) -std=c++20 -O2 -g -DSS_FORCE_SLOW -pthread src/main.cpp -o build/pipeline-slow

## The image the scorer builds
image:
	docker buildx build --platform $(PLATFORM) --load -t $(IMAGE) .
