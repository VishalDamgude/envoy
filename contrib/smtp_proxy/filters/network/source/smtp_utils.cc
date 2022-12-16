#include "contrib/smtp_proxy/filters/network/source/smtp_utils.h"

#include "source/common/buffer/buffer_impl.h"


namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

bool BufferHelper::endOfBuffer(Buffer::Instance& buffer) { return buffer.length() == 0; }

DecodeStatus BufferHelper::skipBytes(Buffer::Instance& buffer, size_t skip_bytes) {
  if (buffer.length() < skip_bytes) {
    return DecodeStatus::Failure;
  }
  buffer.drain(skip_bytes);
  return DecodeStatus::Success;
}

DecodeStatus BufferHelper::readString(Buffer::Instance& buffer, std::string& str) {
  char end = MYSQL_STR_END;
  ssize_t index = buffer.search(&end, sizeof(end), 0);
  if (index == -1) {
    return DecodeStatus::Failure;
  }
  str.assign(static_cast<char*>(buffer.linearize(index)), index);
  buffer.drain(index + 1);
  return DecodeStatus::Success;
}

DecodeStatus BufferHelper::readStringBySize(Buffer::Instance& buffer, size_t len,
                                            std::string& str) {
  if (buffer.length() < len) {
    return DecodeStatus::Failure;
  }
  str.assign(static_cast<char*>(buffer.linearize(len)), len);
  buffer.drain(len);
  return DecodeStatus::Success;
}


} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy