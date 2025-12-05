CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall
WEBKIT_FLAGS = $(shell pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0)

all: agent agent-view code-agent

agent: agent.cpp
	$(CXX) $(CXXFLAGS) -o agent agent.cpp -lcurl -lreadline

agent-view: agent-view.cpp
	$(CXX) $(CXXFLAGS) -o agent-view agent-view.cpp $(WEBKIT_FLAGS)

code-agent: code-agent.cpp
	$(CXX) $(CXXFLAGS) -o code-agent code-agent.cpp -lcurl	

install: agent agent-view
	cp agent /usr/local/bin/
	cp agent-view /usr/local/bin/
	cp code-agent /usr/local/bin/

clean:
	rm -f agent agent-view code-agent

.PHONY: all install clean
