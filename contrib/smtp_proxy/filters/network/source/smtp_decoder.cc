
#include "contrib/smtp_proxy/filters/network/source/smtp_decoder.h"
#include "source/common/common/logger.h"

#include "contrib/smtp_proxy/filters/network/source/smtp_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

Decoder::Result DecoderImpl::onData(Buffer::Instance& data, bool upstream) {
  const std::string message = data.toString();
  ENVOY_LOG(info, "smtp_proxy: received message: ", message);
  std::cout << message << "\n";
  std::cout << "current state: " << StateStrings[static_cast<int>(session_.getState())] << "\n";
  Decoder::Result result = Decoder::Result::ReadyForNext;
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
  ENVOY_LOG(debug, "smtp_proxy: decoding {} bytes", data.length());

  // std::string command;
  Buffer::OwnedImpl message;
  message.add(data);

  if(message.length() < 4) {
    //Message size is not sufficient to parse.
    return Decoder::Result::ReadyForNext;
  }
  
  // absl::string_view cmd_str = StringUtil::trim(message.toString());
  // std::cout << "command string : " << cmd_str << " length: " << cmd_str.length() << "\n"; 

  switch (session_.getState()) {
    case SmtpSession::State::SESSION_INIT: {
      if( message.startsWith(smtpEhloCommand) || message.startsWith(smtpHeloCommand)) {
        session_.setState(SmtpSession::State::SESSION_REQUEST);
      }
      break;
    }
    case SmtpSession::State::SESSION_IN_PROGRESS: {

      if(message.startsWith(startTlsCommand))
      {
        if(session_encrypted_)
          break;
        
        if(!callbacks_->onStartTlsCommand(readyToStartTlsResponse)) {
          // callback returns false if connection is switched to tls i.e. tls termination is successful.
          session_encrypted_ = true;
        } else {
          //error while switching transport socket to tls.
          callbacks_->sendReplyDownstream(failedToStartTlsResponse);
        } 
        return Decoder::Result::Stopped;  
      } else if(message.startsWith(smtpMailCommand)) {
        session_.setState(SmtpSession::State::TRANSACTION_REQUEST);
      } else if(message.startsWith(smtpQuitCommand)) {
        session_.setState(SmtpSession::State::SESSION_TERMINATION_REQUEST);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_IN_PROGRESS: {
      if(message.startsWith(smtpDataCommand)) {
        session_.setState(SmtpSession::State::MAIL_DATA_REQUEST);
      } else if(message.startsWith(smtpRsetCommand) || message.startsWith(smtpEhloCommand) || message.startsWith(smtpHeloCommand)) {
        session_.setState(SmtpSession::State::TRANSACTION_ABORT_REQUEST);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_COMPLETED: {
      if(message.startsWith(smtpQuitCommand)) {
        session_.setState(SmtpSession::State::SESSION_TERMINATION_REQUEST);
      } else if(message.startsWith(smtpMailCommand)) {
        session_.setState(SmtpSession::State::TRANSACTION_REQUEST);
      }
      break;
    }
    default:
      break;
  }

  //Handle duplicate/out-of-order SMTP commands
  if(session_encrypted_ && message.startsWith(startTlsCommand)) {
    ENVOY_LOG(error, "smtp_proxy: received starttls when session is already encrypted.");
    callbacks_->sendReplyDownstream(outOfOrderCommandResponse);
    return Decoder::Result::Stopped;
  }
  return Decoder::Result::ReadyForNext;
}


Decoder::Result DecoderImpl::parseResponse(Buffer::Instance& data) {
  ENVOY_LOG(debug, "smtp_proxy: decoding response {} bytes", data.length());

  Decoder::Result result = Decoder::Result::ReadyForNext;

  if(data.length() < 3) {
    //Minimum 3 byte response code needed to parse response from server.
    return result;
  }
  std::string response;
  response.assign(std::string(static_cast<char*>(data.linearize(3)), 3));

  uint16_t response_code = stoi(response);
  std::cout<< "response code 3 bytes: " << response_code << "\n";
  
  switch (session_.getState()) {

    case SmtpSession::State::SESSION_INIT: {
      if(response_code == 554) {
        callbacks_->incSmtpConnectionEstablishmentErrors();
      }
      break;
    }

    case SmtpSession::State::SESSION_REQUEST: {
      if(response_code == 250) {
        session_.setState(SmtpSession::State::SESSION_IN_PROGRESS);
      }
      break;
    }

    case SmtpSession::State::TRANSACTION_REQUEST: {
      if(response_code == 250) {
        session_.setState(SmtpSession::State::TRANSACTION_IN_PROGRESS);
      }
      break;
    }
    case SmtpSession::State::MAIL_DATA_REQUEST: {
      if(response_code == 250) {
        callbacks_->incSmtpTransactions();
        session_.setState(SmtpSession::State::TRANSACTION_COMPLETED);
      }
      break;
    }
    case SmtpSession::State::TRANSACTION_ABORT_REQUEST: {
      if(response_code == 250) {
        callbacks_->incSmtpTransactionsAborted();
        session_.setState(SmtpSession::State::SESSION_IN_PROGRESS);
      }
      break;
    }
    case SmtpSession::State::SESSION_TERMINATION_REQUEST: {
      if(response_code == 221) {
        session_.setState(SmtpSession::State::SESSION_TERMINATED);
        callbacks_->incSmtpSessionsCompleted();
      }
      break;
    }
    default:
      result = Decoder::Result::ReadyForNext;
  }
  if(response_code >= 400 && response_code <= 499) {
    callbacks_->incSmtp4xxErrors();
  } else if(response_code >= 500 && response_code <= 599) {
    callbacks_->incSmtp5xxErrors();
  }
  return result;
}

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy