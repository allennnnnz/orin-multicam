# orin-multicam - build on the Jetson itself (aarch64, JetPack 5.x)
MMAPI   ?= /usr/src/jetson_multimedia_api
CUDA    ?= /usr/local/cuda
TEGRA   := /usr/lib/aarch64-linux-gnu/tegra

CC      := gcc
CXX     := g++
NVCC    := $(CUDA)/bin/nvcc

INC     := -I$(MMAPI)/include -I$(CUDA)/include -Isrc
CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 $(INC)
CXXFLAGS:= -O2 -g -Wall -std=c++14 $(INC)
NVFLAGS := -O2 -g -std=c++14 -arch=sm_87 $(INC)
LDLIBS  := -L$(TEGRA) -L$(CUDA)/lib64 -Wl,-rpath,$(TEGRA) \
           -lnvv4l2 -lnvbufsurface -lnvbufsurftransform -lEGL \
           -lnvinfer -lcuda -lcudart -lpthread -lm -lstdc++

BUILD   := build
CORE    := h264_index v4l2_dec v4l2_enc cuda_map postprocess stats
CORE_O  := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE))) \
           $(BUILD)/kernels.o $(BUILD)/trt_infer.o

all: $(BUILD)/omc $(BUILD)/dec_test $(BUILD)/membw_hog $(BUILD)/build_engine

$(BUILD)/omc: $(BUILD)/main.o $(CORE_O)
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/dec_test: $(BUILD)/dec_test.o $(BUILD)/h264_index.o $(BUILD)/v4l2_dec.o
	$(CC) -o $@ $^ $(LDLIBS)

$(BUILD)/build_engine: tools/build_engine.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< -L$(CUDA)/lib64 -lnvinfer -lnvonnxparser -lcudart

$(BUILD)/membw_hog: tools/membw_hog.c
	$(CC) -O2 -Wall -std=gnu11 -o $@ $< -lpthread

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/%.o: tests/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/%.o: src/%.cpp src/*.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(BUILD)/%.o: src/%.cu src/*.h | $(BUILD)
	$(NVCC) $(NVFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
