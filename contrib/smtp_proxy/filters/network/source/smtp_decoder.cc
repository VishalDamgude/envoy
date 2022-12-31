
#include "contrib/smtp_proxy/filters/network/source/smtp_decoder.h"
#include "source/common/common/logger.h"

#include "contrib/smtp_proxy/filters/network/source/smtp_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

Decoder::Result DecoderImpl::onData(Buffer::Instance& data, bool upstream) {
  const std::string message = data.toString();
  ENVOY_LOG(debug, "smtp_proxy: received message: ", message);
  ENVOY_LOG(debug, "upstream: ", upstream);
  // ENVOY_LOG(debug, "current state: ", static_cast<int32_t>(session_.getState()));

  std::cout << message << "\n";
  std::cout << "upstream: " <<  upstream << "\n";
  std::cout << "current state: " << StateStrings[static_cast<int>(session_.getState())] << "\n";
  Decoder::Result result = Decoder::Result::ReadyForNext;
  // std::string command;
  // DecodeStatus status = BufferHelper::readStringBySize(data, 4, command);
  // if (!status)
  // {
  //   ENVOY_LOG(debug, "smtp_proxy: decoded command: ", command);
  // }
  if(upstream)
  {
    std::cout << "received response from upstream" << "\n";
    result = parseResponse(data);
    data.drain(data.length());
    return result;
  }
  result = parseCommand(data);
  data.drain(data.length());
  return result;
}


Decoder::Result DecoderImpl::parseCommand(Buffer::Instance& data) {
  ENVOY_LOG(trace, "smtp_proxy: decoding {} bytes", data.length());

  std::string command;
  Buffer::OwnedImpl message;
  message.add(data);
  uint32_t message_len = message.length();
  absl::string_view cmd_str = StringUtil::trim(message.toString());
  // cmd_str = StringUtil::trim(cmd_str);
  std::cout << "command string : " << cmd_str << " length: " << cmd_str.length() << "\n"; 
  std::cout << "message len: " << message_len << "\n";
  switch (session_.getState()) {
    case SmtpSession::State::SESSION_INIT: {
      if(message.startsWith("EHLO")) {
        session_.setState(SmtpSession::State::SESSION_REQUEST);
      }
      break;
    }
    case SmtpSession::State::SESSION_IN_PROGRESS: {

      if(!session_encrypted_ && message.startsWith(BufferHelper::startTlsCommand))
      {
        if(!callbacks_->onStartTlsCommand()) {
          // callback returns false if connection is switched to tls i.e. tls termination is successful.
          session_encrypted_ = true;
          return Decoder::Result::Stopped;
        } else {
          return Decoder::Result::ReadyForNext;
        } 
      } else if(message.startsWith("MAIL")) {
        session_.setState(SmtpSession::State::TRANSACTION_REQUEST);
      } else if(message.startsWith("QUIT")) {
        session_.setState(SmtpSession::State::SESSION_TERMINATION_REQUEST);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_IN_PROGRESS: {
      if(message.startsWith("DATA")) {
        session_.setState(SmtpSession::State::MAIL_DATA_REQUEST);
      } else if(message.startsWith("RSET")) {
        session_.setState(SmtpSession::State::TRANSACTION_ABORT_REQUEST);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_COMPLETED: {
      if(message.startsWith("QUIT")) {
        session_.setState(SmtpSession::State::SESSION_TERMINATION_REQUEST);
      } else if(message.startsWith("MAIL")) {
        session_.setState(SmtpSession::State::TRANSACTION_REQUEST);
      }
      break;
    }
    default:
      break;
  }
  if(session_encrypted_ && message.startsWith(BufferHelper::startTlsCommand)) {
      ENVOY_LOG(error, "smtp_proxy: received starttls when session is already encrypted.");
      callbacks_->rejectOutOfOrderCommand();
      return Decoder::Result::Stopped;
  }
  return Decoder::Result::ReadyForNext;
}


Decoder::Result DecoderImpl::parseResponse(Buffer::Instance& data) {
  ENVOY_LOG(debug, "smtp_proxy: decoding response {} bytes", data.length());

  // uint32_t response_code = 0;
  Decoder::Result result = Decoder::Result::ReadyForNext;
  
  std::string response;
  response.assign(std::string(static_cast<char*>(data.linearize(3)), 3));

  // const char* response_code_str = static_cast<const char*>(mem2);
  // uint16_t* response_code = static_cast<uint16_t*>(data.linearize(3));
  // if (BufferHelper::readUint24(data, response_code) != DecodeStatus::Success) {
  //   std::cout<< "response code 3 bytes: " << response_code << "\n";
  //   ENVOY_LOG(debug, "error parsing response code");
  //   // return DecodeStatus::Failure;
  //   result = Decoder::Result::ReadyForNext;
  //   return result;
  // }
  uint16_t response_code = stoi(response);
  std::cout<< "response code 3 bytes: " << response << "\n";
  std::cout<< "response code 3 bytes: " << response_code << "\n";
  
  switch (session_.getState()) {

    case SmtpSession::State::SESSION_REQUEST: {
      if(response_code >= 200 && response_code <= 299) {
        session_.setState(SmtpSession::State::SESSION_IN_PROGRESS);
      }
      break;
    }

    case SmtpSession::State::TRANSACTION_REQUEST: {
      if(response_code >= 200 && response_code <= 299) {
        session_.setState(SmtpSession::State::TRANSACTION_IN_PROGRESS);
      }
      break;
    }
    case SmtpSession::State::MAIL_DATA_REQUEST: {
      if(response_code >= 200 && response_code <= 299) {
        callbacks_->incSmtpTransactions();
        session_.setState(SmtpSession::State::TRANSACTION_COMPLETED);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_ABORT_REQUEST: {
      if(response_code >= 200 && response_code <= 299) {
        callbacks_->incSmtpTransactionsAborted();
        session_.setState(SmtpSession::State::SESSION_IN_PROGRESS);
      }
      break;
    }
    case SmtpSession::State::SESSION_TERMINATION_REQUEST: {
      if(response_code >= 200 && response_code <= 299) {
        session_.setState(SmtpSession::State::SESSION_TERMINATED);
        callbacks_->incSmtpSessions();
      }
      break;
    }
    default:
      result = Decoder::Result::ReadyForNext;
  }
  // std::cout<< "draining data from response \n";
  // data.drain(data.length());
  return result;
}

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy