CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra

SRC_DIR = src
BUILD_DIR = build
PREFIX ?= /usr

PARSER_SRC = $(SRC_DIR)/leaf_parser.c
TEST_SRC = $(SRC_DIR)/test_leaf_parser.c
TEST_BIN = test_leaf_parser

LEAF_SRC = $(SRC_DIR)/main.c
LEAF_BIN = leaf
CURL_FLAGS = $(shell curl-config --cflags --libs 2>/dev/null || echo "-lcurl")

.PHONY: all test run-test leaf clean

all: $(TEST_BIN) $(LEAF_BIN)

$(TEST_BIN): $(PARSER_SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) $(PARSER_SRC) $(TEST_SRC) -o $(TEST_BIN)

test: $(TEST_BIN)

run-test: test
	./$(TEST_BIN) web/storage/example/example.leaf

$(LEAF_BIN): $(LEAF_SRC) $(PARSER_SRC)
	$(CC) $(CFLAGS) $(LEAF_SRC) $(PARSER_SRC) $(CURL_FLAGS) -o $(LEAF_BIN)

clean:
	rm -f $(TEST_BIN)
	rm -f $(LEAF_BIN)
	rm -rf $(BUILD_DIR)


install:
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 leaf $(DESTDIR)$(PREFIX)/bin/leaf


