
#include "contrib/smtp_proxy/filters/network/source/smtp_decoder.h"
#include "source/common/common/logger.h"

#include "contrib/smtp_proxy/filters/network/source/smtp_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

void DecoderImpl::onData(Buffer::Instance& data) {
  while (!BufferHelper::endOfBuffer(data) && decode(data)) {
  }
}


bool DecoderImpl::decode(Buffer::Instance& data) {
  ENVOY_LOG(trace, "smtp_proxy: decoding {} bytes", data.length());

  if (session_.getState() == SmtpSession::State::STARTTLS_RECEIVED)
  {
    
  }
}


} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy