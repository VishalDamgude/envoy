#include "contrib/smtp_proxy/filters/network/source/smtp_filter.h"
#include "envoy/network/connection.h"
#include "envoy/buffer/buffer.h"
#include <iostream>
#include <string>
#include "smtp_filter.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

SmtpFilterConfig::SmtpFilterConfig(const SmtpFilterConfigOptions& config_options, Stats::Scope& scope)
  : terminate_tls_(config_options.terminate_tls_), scope_{scope}, 
    stats_(generateStats(config_options.stats_prefix_, scope)) {
   std::cout << config_options.stats_prefix_ << "\n";
} 
SmtpFilter::SmtpFilter(SmtpFilterConfigSharedPtr config) : config_{config} {
  if (!decoder_) {
    decoder_ = createDecoder(this);
  } 
}

Network::FilterStatus SmtpFilter::onNewConnection() {
  return Network::FilterStatus::Continue;
}

Network::FilterStatus SmtpFilter::onCommand(Buffer::Instance& buf) {
  const std::string message = buf.toString();

  // Skip other messages.
  if (StringUtil::trim(message) != startTls) {
    return Network::FilterStatus::Continue;
  }

  read_callbacks_->connection().addBytesSentCallback([=](uint64_t bytes) -> bool {
    // Wait until 6 bytes long "220 OK" has been sent.
    if (bytes >= 6) {
      if (!read_callbacks_->connection().startSecureTransport()) {
        // TODO: switch to Logs
        std::cout << "cannot switch to tls\n";
      }

      // Unsubscribe the callback.
      // Switch to tls has been completed.
      std::cout << "[SMTP_FILTER] Switched to tls\n";
      return false;
    }
    return true;
  });

  buf.drain(buf.length());
  buf.add("220 OK\n");

  read_callbacks_->connection().write(buf, false);
  return Network::FilterStatus::StopIteration;
}

// onData method processes payloads sent by downstream client.
Network::FilterStatus SmtpFilter::onData(Buffer::Instance& data, bool) {
  ENVOY_CONN_LOG(trace, "smtp_proxy: got {} bytes", read_callbacks_->connection(),
                 data.length());
  read_buffer_.add(data);
  // return onCommand(buf);
  Network::FilterStatus result = doDecode(read_buffer_);
  if (result == Network::FilterStatus::StopIteration) {
    ASSERT(read_buffer_.length() == 0);
    data.drain(data.length());
  }
  return result;
}

// onWrite method processes payloads sent by upstream to the client.
Network::FilterStatus SmtpFilter::onWrite(Buffer::Instance&, bool) {
  return Network::FilterStatus::Continue; //onCommand(buf, false);
}

Network::FilterStatus SmtpFilter::doDecode(Buffer::Instance& data) {

  while (0 < data.length()) {
    switch (decoder_->onData(data)) {
    case Decoder::Result::NeedMoreData:
      return Network::FilterStatus::Continue;
    case Decoder::Result::ReadyForNext:
      continue;
    case Decoder::Result::Stopped:
      return Network::FilterStatus::StopIteration;
    }
  }
  return Network::FilterStatus::Continue;
// try {
//     decoder_->onData(buffer);
//   } catch (EnvoyException& e) {
//     ENVOY_LOG(info, "smtp_proxy: decoding error: {}", e.what());
//     config_->stats_.decoder_errors_.inc();
//     read_buffer_.drain(read_buffer_.length());
//     write_buffer_.drain(write_buffer_.length());
//   }
//   return Network::FilterStatus::Continue;
  
}

DecoderPtr SmtpFilter::createDecoder(DecoderCallbacks* callbacks) {
  return std::make_unique<DecoderImpl>(callbacks);
}

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy