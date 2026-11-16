CXX := x86_64-w64-mingw32-g++
WINDRES := x86_64-w64-mingw32-windres

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601
LDFLAGS := -municode -mwindows -static -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32

TARGET := xcaler-disk-analyzer.exe
BUILD_DIR := build

OBJS := $(BUILD_DIR)/main.o $(BUILD_DIR)/scanner.o $(BUILD_DIR)/resource.o

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/main.o: src/main.cpp src/scanner.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/scanner.o: src/scanner.cpp src/scanner.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/resource.o: src/resource.rc src/app.manifest | $(BUILD_DIR)
	$(WINDRES) $< -O coff -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
