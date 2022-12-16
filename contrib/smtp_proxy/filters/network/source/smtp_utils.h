#pragma once

#include "envoy/buffer/buffer.h"
#include "envoy/common/platform.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/byte_order.h"
#include "source/common/common/logger.h"


namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

/**
 * IO helpers for reading/writing SMTP data from/to a buffer.
 */
class BufferHelper : public Logger::Loggable<Logger::Id::filter> {
public:
  static bool endOfBuffer(Buffer::Instance& buffer);
  static DecodeStatus skipBytes(Buffer::Instance& buffer, size_t skip_bytes);
  static DecodeStatus readString(Buffer::Instance& buffer, std::string& str);
  static DecodeStatus readStringBySize(Buffer::Instance& buffer, size_t len, std::string& str);
 
};

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
