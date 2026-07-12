CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pthread

.PHONY: all test clean

all: multi_chat_server multi_client

multi_chat_server: multi_chat_server.cc chat_common.h
	$(CXX) $(CXXFLAGS) multi_chat_server.cc -o multi_chat_server

multi_client: multi_client.cc
	$(CXX) $(CXXFLAGS) multi_client.cc -o multi_client

chat_common_test: chat_common_test.cc chat_common.h
	$(CXX) $(CXXFLAGS) chat_common_test.cc -o chat_common_test

test: all chat_common_test
	./chat_common_test
	python3 chat_integration_test.py

clean:
	rm -f multi_chat_server multi_client chat_common_test
