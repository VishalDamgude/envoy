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
    SESSION_IN_PROGRESS = 1,
    SESSION_COMPLETED = 2,
    TRANSACTION_IN_PROGRESS = 3,
    TRANSACTION_ABORTED = 4,
    TRANSACTION_COMPLETED = 4,
    STARTTLS_RECEIVED = 5,
    ERROR = 6,
  };

  void setState(SmtpSession::State state) { state_ = state; }
  SmtpSession::State getState() { return state_; }
  bool inTransaction() { return in_transaction_; };
  void setInTransaction(bool in_transaction) { in_transaction_ = in_transaction; };

private:
  bool in_transaction_{false};
  SmtpSession::State state_{state::SESSION_INIT};
};

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
