CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread

SRC_DIR  = src
OBJ_DIR  = build
TEST_DIR = tests

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = tsdb

.PHONY: all clean tests

all: $(OBJ_DIR) $(TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_DIR)/test_bitstream: all
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) \
	    $(TEST_DIR)/test_bitstream.cpp \
	    $(OBJ_DIR)/bitstream.o \
	    -o $(TEST_DIR)/test_bitstream

$(TEST_DIR)/test_compress: all
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) \
	    $(TEST_DIR)/test_compress.cpp \
	    $(OBJ_DIR)/bitstream.o $(OBJ_DIR)/compress.o \
	    -o $(TEST_DIR)/test_compress

$(TEST_DIR)/test_chunk: all
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) \
	    $(TEST_DIR)/test_chunk.cpp \
	    $(OBJ_DIR)/bitstream.o $(OBJ_DIR)/compress.o $(OBJ_DIR)/chunk.o \
	    -o $(TEST_DIR)/test_chunk

tests: $(TEST_DIR)/test_bitstream $(TEST_DIR)/test_compress $(TEST_DIR)/test_chunk
	@echo "--- Running bitstream test ---"
	./$(TEST_DIR)/test_bitstream
	@echo "--- Running compress test ---"
	./$(TEST_DIR)/test_compress
	@echo "--- Running chunk test ---"
	./$(TEST_DIR)/test_chunk

clean:
	rm -rf $(OBJ_DIR) $(TARGET) \
	    $(TEST_DIR)/test_bitstream $(TEST_DIR)/test_compress $(TEST_DIR)/test_chunk
