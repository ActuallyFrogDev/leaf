CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra

SRC_DIR = src
BUILD_DIR = build

PARSER_SRC = $(SRC_DIR)/leaf_parser.c
TEST_SRC = $(SRC_DIR)/test_leaf_parser.c
TEST_BIN = test_leaf_parser

.PHONY: all test run-test clean

all: $(TEST_BIN)

$(TEST_BIN): $(PARSER_SRC) $(TEST_SRC)
	$(CC) $(CFLAGS) $(PARSER_SRC) $(TEST_SRC) -o $(TEST_BIN)

test: $(TEST_BIN)

run-test: test
	./$(TEST_BIN) web/storage/example/example.leaf

clean:
	rm -f $(TEST_BIN)
	rm -rf $(BUILD_DIR)

