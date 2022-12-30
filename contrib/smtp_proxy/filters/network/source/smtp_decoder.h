#pragma once
#include <cstdint>

#include "envoy/common/platform.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/logger.h"

#include "absl/container/flat_hash_map.h"
#include "contrib/smtp_proxy/filters/network/source/smtp_session.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SmtpProxy {

// General callbacks for dispatching decoded SMTP messages to a sink.
class DecoderCallbacks {
public:
  virtual ~DecoderCallbacks() = default;

//   virtual void incMessagesFrontend() PURE;
//   virtual void incMessagesUnknown() PURE;

  virtual void incTlsTerminatedSessions() PURE;
  virtual void incSmtpTransactions() PURE;
  virtual void incSmtpSessions() PURE;
  // virtual void incSessionsUnencrypted() PURE;

//   enum class StatementType { Insert, Delete, Select, Update, Other, Noop };
//   virtual void incStatements(StatementType) PURE;

//   virtual void incTransactions() PURE;
//   virtual void incTransactionsCommit() PURE;
//   virtual void incTransactionsRollback() PURE;

//   enum class NoticeType { Warning, Notice, Debug, Info, Log, Unknown };
//   virtual void incNotices(NoticeType) PURE;

  enum class ErrorType { Error, Fatal, Panic, Unknown };
  // virtual void incErrors(ErrorType) PURE;

  // virtual void processQuery(const std::string&) PURE;

  virtual bool onStartTlsCommand(Buffer::Instance& buf) PURE;
};

// SMTP message decoder.
class Decoder {
public:
  virtual ~Decoder() = default;

  // The following values are returned by the decoder, when filter
  // passes bytes of data via onData method:
  enum class Result {
    ReadyForNext, // Decoder processed previous message and is ready for the next message.
    NeedMoreData, // Decoder needs more data to reconstruct the message.
    Stopped // Received and processed message disrupts the current flow. Decoder stopped accepting
            // data. This happens when decoder wants filter to perform some action, for example to
            // call starttls transport socket to enable TLS.
  };
  virtual Result onData(Buffer::Instance& data) PURE;
  virtual SmtpSession& getSession() PURE;


};

using DecoderPtr = std::unique_ptr<Decoder>;

class DecoderImpl : public Decoder, Logger::Loggable<Logger::Id::filter> {
public:
  DecoderImpl(DecoderCallbacks* callbacks) : callbacks_(callbacks) { }

  Result onData(Buffer::Instance& data) override;
  SmtpSession& getSession() override { return session_; }

  std::string getMessage() { return message_; }

  bool encrypted() const { return encrypted_; }


protected:

  Decoder::Result parseCommand(Buffer::Instance& data);
  bool parseResponse (Buffer::Instance& data);
  void parseMessage(Buffer::Instance& message, uint8_t seq, uint32_t len);

  DecoderCallbacks* callbacks_{};
  SmtpSession session_{};

//   // The following fields store result of message parsing.
//   char command_{'-'};
   std::string message_;
   uint32_t message_len_{};

  bool encrypted_{false}; // tells if exchange is encrypted

};

} // namespace SmtpProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
