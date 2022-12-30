
#include "contrib/smtp_proxy/filters/network/source/smtp_decoder.h"
#include "source/common/common/logger.h"

#include "contrib/smtp_proxy/filters/network/source/smtp_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

Decoder::Result DecoderImpl::onData(Buffer::Instance& data, bool upstream) {
  const std::string message = data.toString();
  ENVOY_LOG(debug, "smtp_proxy: received command: ", message);

  // std::string command;
  // DecodeStatus status = BufferHelper::readStringBySize(data, 4, command);
  // if (!status)
  // {
  //   ENVOY_LOG(debug, "smtp_proxy: decoded command: ", command);
  // }
  if(upstream)
  {
    return parseResponse(data);
  }


  Decoder::Result result = Decoder::Result::ReadyForNext;
  switch (session_.getState()) {
    case SmtpSession::State::SESSION_INIT: {
      //Expect EHLO command
      //parseMessage and set the new state;
      result = parseCommand(data);
      break;
    }

    case SmtpSession::State::SESSION_IN_PROGRESS: {
      result = parseCommand(data);
      break;
    }
    case SmtpSession::State::SESSION_TERMINATED: {
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
  Buffer::OwnedImpl buf;
  data.copyOut(0, data.length(), &buf);
  if(session_.getState() == SmtpSession::State::SESSION_IN_PROGRESS)
  {
    if(data.startsWith(BufferHelper::startTlsCommand))
    {
      if(!callbacks_->onStartTlsCommand(data)) {
        // callback returns false if connection is switched to tls i.e. tls termination is successful.
        //session_.setState(SmtpSession::State::SESSION_ENCRYPTED);
        return Decoder::Result::Stopped;
      } else {
        return Decoder::Result::ReadyForNext;
      }
    } else {
      DecodeStatus status = BufferHelper::readStringBySize(data, 4, command);
      if (command == "MAIL") {
        session_.setState(SmtpSession::State::TRANSACTION_REQUEST);
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
    }
  }
  return Decoder::Result::ReadyForNext;
}


Decoder::Result DecoderImpl::parseResponse(Buffer::Instance& data) {
  ENVOY_LOG(trace, "smtp_proxy: decoding response {} bytes", data.length());

  uint32_t response_code;
  Decoder::Result result = Decoder::Result::ReadyForNext;

  if (BufferHelper::readUint24(data, response_code) != DecodeStatus::Success) {

    ENVOY_LOG(debug, "error parsing response code");
    // return DecodeStatus::Failure;
    result = Decoder::Result::ReadyForNext;
    return result;
  }

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
    default:
      return Decoder::Result::ReadyForNext;
  }

  data.drain(data.length());
  return result;
}

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy