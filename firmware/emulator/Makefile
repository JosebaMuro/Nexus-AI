CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

nexusled-emu: nexusled_emu.cpp ../nexusled/*.h
	$(CXX) $(CXXFLAGS) -o $@ nexusled_emu.cpp

clean:
	rm -f nexusled-emu
