CXX = clang++
CXXFLAGS = -std=c++17 -O2

all: drews_vocoder_toy

drews_vocoder_toy: drews_vocoder_toy.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -framework CoreMIDI -framework AudioUnit

clean:
	rm -f drews_vocoder_toy

.PHONY: all clean
