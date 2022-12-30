
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

  // std::string command;
  // DecodeStatus status = BufferHelper::readStringBySize(data, 4, command);
  // if (!status)
  // {
  //   ENVOY_LOG(debug, "smtp_proxy: decoded command: ", command);
  // }
  Decoder::Result result = Decoder::Result::ReadyForNext;
  switch (session_.getState()) {
    case SmtpSession::State::SESSION_INIT: {
      //Expect EHLO command
      //parseMessage and set the new state;
      result = parseCommand(data);
      break;
    }

    case SmtpSession::State::SESSION_REQUEST: {
      if(parseResponse(data) == DecodeStatus::Success) {
        session_.setState(SmtpSession::State::SESSION_IN_PROGRESS);
      }
      break;
    }
    case SmtpSession::State::SESSION_IN_PROGRESS: {
      result = parseCommand(data);
      break;
    }
    case SmtpSession::State::SESSION_COMPLETED: {
      break;
    }
    case SmtpSession::State::SESSION_TERMINATED: {
      break;
    }
    case SmtpSession::State::TRANSACTION_REQUEST: {
      if(parseResponse(data) == DecodeStatus::Success) {
        session_.setState(SmtpSession::State::TRANSACTION_IN_PROGRESS);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_IN_PROGRESS: {
      result = parseCommand(data);
      break;
    }
    case SmtpSession::State::TRANSACTION_ABORTED: {
      break;
    }
    case SmtpSession::State::TRANSACTION_COMPLETED: {
      break;
    }
    default:
      return Decoder::Result::ReadyForNext;
  }
  // Skip other messages.
  // if (StringUtil::trim(message) != BufferHelper::startTlsCommand) {
  //   return Decoder::Result::ReadyForNext;
  // }

  data.drain(data.length());
  return result;
  //return Decoder::Result::Stopped;
  // while (!BufferHelper::endOfBuffer(data) && decode(data)) {
  // }
}


Decoder::Result DecoderImpl::parseCommand(Buffer::Instance& data) {
  ENVOY_LOG(trace, "smtp_proxy: decoding {} bytes", data.length());

  std::string command;
  if(session_.getState() == SmtpSession::State::SESSION_IN_PROGRESS)
  {
    if(data.startsWith(BufferHelper::startTlsCommand))
    {
      if(!callbacks_->onStartTlsCommand(data)) {
        // callback return false if connection is switched to tls i.e. tls termination is successful.
        //session_.setState(SmtpSession::State::SESSION_ENCRYPTED);
        return Decoder::Result::Stopped;
      } else {
        return Decoder::Result::ReadyForNext;
      }
    }
  }
  DecodeStatus status = BufferHelper::readStringBySize(data, 4, command);
  if (!status)
  {
    ENVOY_LOG(debug, "smtp_proxy: decoded command: ", command);
    if( command == "EHLO")
    {
      session_.setState(SmtpSession::State::SESSION_REQUEST);
    } else if (command == "MAIL") {
      session_.setState(SmtpSession::State::TRANSACTION_REQUEST);
    }
  }
  // if (session_.getState() == SmtpSession::State::STARTTLS_REQ_RECEIVED)
  // {
  //   ENVOY_LOG(debug, "smtp_proxy: current_state: ", session_.getState());
  // }

  // 
  //data.drain(data.length());
  return DecodeStatus::Success;
}


bool DecoderImpl::parseResponse(Buffer::Instance& data) {
  ENVOY_LOG(trace, "smtp_proxy: decoding {} bytes", data.length());

  uint32_t response_code;
  if (BufferHelper::readUint24(data, response_code) != DecodeStatus::Success) {

    ENVOY_LOG(debug, "error when parsing response code");
    return DecodeStatus::Failure;
  }

  if( response_code >= 200 && response_code <= 299)
  {
    return DecodeStatus::Success;
  }  
  return DecodeStatus::Success;
}

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy