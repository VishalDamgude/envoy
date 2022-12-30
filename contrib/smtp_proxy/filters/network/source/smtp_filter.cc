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

bool SmtpFilter::onStartTlsCommand(Buffer::Instance& buf) {

  if(!config_->terminate_tls_) {
    // Signal to the decoder to continue.
    return true;
  }
  read_callbacks_->connection().addBytesSentCallback([=](uint64_t bytes) -> bool {
    // Wait until 6 bytes long "220 OK" has been sent.
    if (bytes >= 6) {
      if (!read_callbacks_->connection().startSecureTransport()) {
        ENVOY_CONN_LOG(trace, "smtp_proxy filter: cannot switch to tls", read_callbacks_->connection(), bytes);
      }

      // Switch to TLS has been completed.
      // Signal to the decoder to stop processing the current message (SSLRequest).
      // Because Envoy terminates SSL, the message was consumed and should not be
      // passed to other filters in the chain.
      incTlsTerminatedSessions();
      ENVOY_CONN_LOG(trace, "smtp_proxy filter: switched to tls", read_callbacks_->connection(), bytes);
      return false;
    }
    return true;
  });

  buf.drain(buf.length());
  buf.add("220 OK\n");

  read_callbacks_->connection().write(buf, false);
  return false;
}

void SmtpFilter::incTlsTerminatedSessions() {
  config_->stats_.tls_terminated_sessions_.inc();
}
void SmtpFilter::incSmtpTransactions() {
  config_->stats_.smtp_transactions_.inc();
}

void SmtpFilter::incSmtpSessions() {
  config_->stats_.smtp_sessions_.inc();
  
}
// Network::FilterStatus SmtpFilter::onCommand(Buffer::Instance& buf) {
//   const std::string message = buf.toString();

//   // Skip other messages.
//   if (StringUtil::trim(message) != startTls) {
//     return Network::FilterStatus::Continue;
//   }

//   read_callbacks_->connection().addBytesSentCallback([=](uint64_t bytes) -> bool {
//     // Wait until 6 bytes long "220 OK" has been sent.
//     if (bytes >= 6) {
//       if (!read_callbacks_->connection().startSecureTransport()) {
//         // TODO: switch to Logs
//         std::cout << "cannot switch to tls\n";
//       }

//       // Unsubscribe the callback.
//       // Switch to tls has been completed.
//       std::cout << "[SMTP_FILTER] Switched to tls\n";
//       return false;
//     }
//     return true;
//   });

//   buf.drain(buf.length());
//   buf.add("220 OK\n");

//   read_callbacks_->connection().write(buf, false);
//   return Network::FilterStatus::StopIteration;
// }

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
Network::FilterStatus SmtpFilter::onWrite(Buffer::Instance& data, bool) {
  return doDecode(data); //Network::FilterStatus::Continue; //onCommand(buf, false);
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