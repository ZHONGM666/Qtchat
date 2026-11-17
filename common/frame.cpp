#include "frame.h"
#include <algorithm>
#include <cstdint>

namespace qtchat {
std::string encodeFrame(std::string_view payload) {
  if (payload.empty() || payload.size() > MaxFrameSize)
    throw ProtocolError("frame length must be 1..65536 bytes");
  const auto size = static_cast<std::uint32_t>(payload.size());
  std::string frame(4, '\0');
  for (unsigned i = 0; i < 4; ++i)
    frame[i] = static_cast<char>((size >> (24 - 8 * i)) & 0xff);
  frame.append(payload);
  return frame;
}

std::vector<std::string> FrameDecoder::feed(std::string_view bytes) {
  std::vector<std::string> frames;
  while (!bytes.empty()) {
    if (buffer_.size() < 4) {
      const auto count = std::min(4 - buffer_.size(), bytes.size());
      buffer_.append(bytes.substr(0, count));
      bytes.remove_prefix(count);
      if (buffer_.size() < 4)
        break;
    }
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 4; ++i)
      length = (length << 8) | static_cast<unsigned char>(buffer_[i]);
    if (length == 0 || length > MaxFrameSize) {
      reset();
      throw ProtocolError("frame length must be 1..65536 bytes");
    }
    const auto count = std::min(4 + length - buffer_.size(), bytes.size());
    buffer_.append(bytes.substr(0, count));
    bytes.remove_prefix(count);
    if (buffer_.size() == 4 + length) {
      frames.emplace_back(buffer_.substr(4));
      buffer_.clear();
    }
  }
  return frames;
}
void FrameDecoder::reset() { buffer_.clear(); }
} // namespace qtchat
