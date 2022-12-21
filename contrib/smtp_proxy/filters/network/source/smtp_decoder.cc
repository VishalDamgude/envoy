
#include "contrib/smtp_proxy/filters/network/source/smtp_decoder.h"
#include "source/common/common/logger.h"

#include "contrib/smtp_proxy/filters/network/source/smtp_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

Decoder::Result DecoderImpl::onData(Buffer::Instance& data) {
  const std::string message = data.toString();
  ENVOY_LOG(debug, "smtp_proxy: received command: ", message);
  
  std::string command;
  DecodeStatus status = BufferHelper::readStringBySize(data, 4, command);
  if (!status)
  {
    ENVOY_LOG(debug, "smtp_proxy: decoded command: ", command);
  }
  // Skip other messages.
  if (StringUtil::trim(message) != BufferHelper::startTlsCommand) {
    return Decoder::Result::ReadyForNext;
  }

  return Decoder::Result::ReadyForNext;
  //return Decoder::Result::Stopped;
  // while (!BufferHelper::endOfBuffer(data) && decode(data)) {
  // }
}


bool DecoderImpl::decode(Buffer::Instance& data) {
  ENVOY_LOG(trace, "smtp_proxy: decoding {} bytes", data.length());

  if (session_.getState() == SmtpSession::State::STARTTLS_RECEIVED)
  {
    ENVOY_LOG(debug, "smtp_proxy: current_state");
  }
  return true;
}


} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy