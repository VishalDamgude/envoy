#pragma once
#include <cstdint>

#include "source/common/common/logger.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

// Class stores data about the current state of a transaction between SMTP client and server.
class SmtpSession {
public:
  enum class State {
    SESSION_INIT = 0,
    SESSION_REQUEST = 1,
    SESSION_IN_PROGRESS = 2,
    SESSION_TERMINATION_REQUEST = 3,
    SESSION_TERMINATED = 4,
    TRANSACTION_REQUEST = 5,
    TRANSACTION_IN_PROGRESS = 6,
    TRANSACTION_ABORT_REQUEST = 7,
    TRANSACTION_ABORTED = 8,
    END_OF_MAIL_INDICATION_RECEIVECD = 9,
    TRANSACTION_COMPLETED = 10,
    STARTTLS_REQ_RECEIVED = 11,
    SESSION_ENCRYPTED = 12,
    ERROR = 13,
  };

  void setState(SmtpSession::State state) { state_ = state; }
  SmtpSession::State getState() { return state_; }
  bool inTransaction() { return in_transaction_; };
  void setInTransaction(bool in_transaction) { in_transaction_ = in_transaction; };

private:
  bool in_transaction_{false};
  SmtpSession::State state_{State::SESSION_INIT};
};

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
