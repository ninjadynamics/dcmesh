# dcmesh Makefile - PC-side offline converter
#
# Prerequisites:
#   git clone https://github.com/zeux/meshoptimizer
#
# Build:
#   make MESHOPT_DIR=./meshoptimizer

MESHOPT_DIR ?= ./meshoptimizer

CC  = gcc
CXX = g++
CFLAGS   = -O2 -Wall -I$(MESHOPT_DIR)/src -I$(MESHOPT_DIR)/extern -I.
CXXFLAGS = -O2 -Wall -I$(MESHOPT_DIR)/src

ifeq ($(OS),Windows_NT)
EXEEXT ?= .exe
endif

TARGET = dcmesh$(EXEEXT)
DCMESH_OBJ = dcmesh.o

# Only the two meshoptimizer modules we actually use
MESHOPT_OBJS = stripifier.o vcacheoptimizer.o
OBJS = $(DCMESH_OBJ) $(MESHOPT_OBJS)

all: $(TARGET)

# Compile our C converter
$(DCMESH_OBJ): src/dcmesh.c src/dcmesh.h
	$(CC) $(CFLAGS) -c src/dcmesh.c -o $@

# Compile meshoptimizer C++ sources separately
stripifier.o: $(MESHOPT_DIR)/src/stripifier.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

vcacheoptimizer.o: $(MESHOPT_DIR)/src/vcacheoptimizer.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link with C++ linker (needed for C++ standard library)
$(TARGET): $(OBJS)
	$(CXX) $^ -o $(TARGET) -lm

clean:
	rm -f dcmesh dcmesh.exe *.o

.INTERMEDIATE: $(OBJS)
.PHONY: all clean
