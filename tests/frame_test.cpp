#include "check.h"
#include "common/frame.h"
using namespace qtchat;

int main() {
  Checks tests;
  tests.run("encoding uses a four byte big endian length", [] {
    check(encodeFrame("abc") == std::string("\0\0\0\x03"
                                            "abc",
                                            7),
          "wire bytes differ");
  });
  tests.run("fragmented UTF-8 frame waits for the last byte", [] {
    const std::string text = u8"你好，QtChat";
    const std::string wire = std::string("\0\0\0\x0f", 4) + text;
    FrameDecoder decoder;
    check(decoder.feed(wire.substr(0, 2)).empty(), "partial prefix emitted");
    check(decoder.feed(wire.substr(2, 4)).empty(), "partial body emitted");
    check(decoder.feed(wire.substr(6)) == std::vector<std::string>{text},
          "UTF-8 body missing");
  });
  tests.run("coalesced frames preserve ordering", [] {
    FrameDecoder decoder;
    check(decoder.feed(std::string("\0\0\0\x01"
                                   "a\0\0\0\x02"
                                   "bc",
                                   11)) == std::vector<std::string>{"a", "bc"},
          "coalesced frames differ");
  });
  tests.run("incomplete next frame survives feed calls", [] {
    FrameDecoder decoder;
    check(decoder.feed(std::string("\0\0\0\x01"
                                   "a\0\0",
                                   7)) == std::vector<std::string>{"a"},
          "first frame missing");
    check(decoder.feed(std::string("\0\x01"
                                   "b",
                                   3)) == std::vector<std::string>{"b"},
          "partial next prefix lost");
  });
  tests.run("zero length input frame is rejected", [] {
    FrameDecoder decoder;
    throws<ProtocolError>([&] { decoder.feed(std::string(4, '\0')); });
  });
  tests.run("oversize input rejected from prefix alone", [] {
    FrameDecoder decoder;
    throws<ProtocolError>(
        [&] { decoder.feed(std::string("\0\x01\0\x01", 4)); });
  });
  tests.run("encoder rejects empty and oversize bodies", [] {
    throws<ProtocolError>([] { encodeFrame(""); });
    throws<ProtocolError>([] { encodeFrame(std::string(65537, 'x')); });
  });
  tests.run("maximum size frame and large coalesced batch are valid", [] {
    const std::string body(65536, 'x');
    const std::string wire = std::string("\0\x01\0\0", 4) + body;
    check(encodeFrame(body) == wire, "maximum encoded size wrong");
    FrameDecoder decoder;
    check(decoder.feed(wire + wire) == std::vector<std::string>{body, body},
          "batch incorrectly limited");
  });
  tests.run("reset discards the previous connection fragment", [] {
    FrameDecoder decoder;
    decoder.feed(std::string("\0\0\0\x02"
                             "a",
                             5));
    decoder.reset();
    check(decoder.feed(std::string("\0\0\0\x01"
                                   "b",
                                   5)) == std::vector<std::string>{"b"},
          "old fragment leaked");
  });
  return tests.result();
}
