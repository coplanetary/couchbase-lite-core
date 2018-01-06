//
//  TokenizerTest.cc
//  LiteCoreCppTests
//
//  Created by Jim Borden on 2018/01/06.
//  Copyright © 2018 Couchbase. All rights reserved.
//

#include "LiteCoreTest.hh"
#include "argumentTokenizer.hh"
#include <deque>
using namespace std;

class TokenizerTestFixture {
protected:
    ArgumentTokenizer _tokenizer;
};

TEST_CASE_METHOD(TokenizerTestFixture, "Tokenizer Test", "[cblite][Tokenizer]") {
    deque<string> args;
    
    SECTION("Simple input") {
        REQUIRE(_tokenizer.tokenize("ls --limit 10", args));
        CHECK(args.size() == 3);
        CHECK(args[0] == "ls");
        CHECK(args[1] == "--limit");
        CHECK(args[2] == "10");
    }
    
    SECTION("Input with quoted argument") {
        REQUIRE(_tokenizer.tokenize("sql \"SELECT * FROM sqlite_master\"", args));
        CHECK(args.size() == 2);
        CHECK(args[0] == "sql");
        CHECK(args[1] == "SELECT * FROM sqlite_master");
    }
    
    SECTION("Input with quoted argument and escaped quotes inside") {
        REQUIRE(_tokenizer.tokenize("sql \"SELECT * FROM sqlite_master WHERE type = \\\"table\\\"\"", args));
        CHECK(args.size() == 2);
        CHECK(args[0] == "sql");
        CHECK(args[1] == "SELECT * FROM sqlite_master WHERE type = \"table\"");
    }
    
    SECTION("Input with escaped quotes") {
        REQUIRE(_tokenizer.tokenize("fetch \\\"with quotes\\\"", args));
        CHECK(args.size() == 3);
        CHECK(args[0] == "fetch");
        CHECK(args[1] == "\"with");
        CHECK(args[2] == "quotes\"");
    }
    
    SECTION("Empty Input") {
        REQUIRE(_tokenizer.tokenize("\"\"", args));
        CHECK(args.size() == 0);
    }
    
    SECTION("Input with quoted argument and escaped quotes separate") {
        REQUIRE(_tokenizer.tokenize("\\\" \"weird\"", args));
        CHECK(args.size() == 2);
        CHECK(args[0] == "\"");
        CHECK(args[1] == "weird");
    }
    
    SECTION("Just escaped quotes") {
        REQUIRE(_tokenizer.tokenize("\\\" \\\"", args));
        CHECK(args.size() == 2);
        CHECK(args[0] == "\"");
        CHECK(args[1] == "\"");
    }
    
    SECTION("Just whitespace") {
        REQUIRE(_tokenizer.tokenize("\" \"", args));
        CHECK(args.size() == 1);
        CHECK(args[0] == " ");
    }
    
    SECTION("Quotes concatenating arguments") {
        REQUIRE(_tokenizer.tokenize("connect\" \"me", args));
        CHECK(args.size() == 1);
        CHECK(args[0] == "connect me");
    }
    
    SECTION("Empty line") {
        REQUIRE(_tokenizer.tokenize("", args));
        CHECK(args.size() == 0);
    }
    
    SECTION("Null input") {
        REQUIRE(!_tokenizer.tokenize(nullptr, args));
    }
    
    SECTION("Unclosed quote") {
        REQUIRE(!_tokenizer.tokenize("\"I am incorrect!", args));
    }
    
    SECTION("Unterminated escape") {
        REQUIRE(!_tokenizer.tokenize("I am incorrect!\\", args));
    }
}
