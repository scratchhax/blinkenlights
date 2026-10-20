BLINK := ../firmware/lib/blink
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17

preview: preview.cpp $(BLINK)/blink_renderer.cpp $(BLINK)/blink_renderer.h $(BLINK)/blink_protocol.h
	$(CXX) $(CXXFLAGS) -I$(BLINK) -o $@ preview.cpp $(BLINK)/blink_renderer.cpp -lm

clean:
	rm -f preview

.PHONY: clean
