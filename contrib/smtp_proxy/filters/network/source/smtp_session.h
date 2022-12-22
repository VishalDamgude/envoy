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
    SESSION_COMPLETED = 3,
    SESSION_TERMINATED = 4,
    TRANSACTION_REQUEST = 5,
    TRANSACTION_IN_PROGRESS = 6,
    TRANSACTION_ABORTED = 7,
    TRANSACTION_COMPLETED = 8,
    STARTTLS_REQ_RECEIVED = 9,
    SESSION_ENCRYPTED = 10,
    ERROR = 11,
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
