#include "common/router/router.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/grpc/status.h"
#include "envoy/http/conn_pool.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/upstream.h"

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/grpc/common.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"
#include "common/router/config_impl.h"
#include "common/router/retry_state_impl.h"
#include "common/tracing/http_tracer_impl.h"

namespace Envoy {
namespace Router {
namespace {
uint32_t getLength(const Buffer::Instance* instance) { return instance ? instance->length() : 0; }

bool schemeIsHttp(const Http::HeaderMap& downstream_headers,
                  const Network::Connection& connection) {
  if (downstream_headers.ForwardedProto() &&
      downstream_headers.ForwardedProto()->value().getStringView() ==
          Http::Headers::get().SchemeValues.Http) {
    return true;
  }
  if (!connection.ssl()) {
    return true;
  }
  return false;
}

bool convertRequestHeadersForInternalRedirect(Http::HeaderMap& downstream_headers,
                                              const Http::HeaderEntry& internal_redirect,
                                              const Network::Connection& connection) {
  // Envoy does not currently support multiple rounds of redirects.
  if (downstream_headers.EnvoyOriginalUrl()) {
    return false;
  }
  // Make sure the redirect response contains a URL to redirect to.
  if (internal_redirect.value().getStringView().length() == 0) {
    return false;
  }

  Http::Utility::Url absolute_url;
  if (!absolute_url.initialize(internal_redirect.value().getStringView())) {
    return false;
  }

  bool scheme_is_http = schemeIsHttp(downstream_headers, connection);
  if (scheme_is_http && absolute_url.scheme() == Http::Headers::get().SchemeValues.Https) {
    // Don't allow serving TLS responses over plaintext.
    return false;
  }

  // Preserve the original request URL for the second pass.
  downstream_headers.insertEnvoyOriginalUrl().value(
      absl::StrCat(scheme_is_http ? Http::Headers::get().SchemeValues.Http
                                  : Http::Headers::get().SchemeValues.Https,
                   "://", downstream_headers.Host()->value().getStringView(),
                   downstream_headers.Path()->value().getStringView()));

  // Replace the original host, scheme and path.
  downstream_headers.insertScheme().value(std::string(absolute_url.scheme()));
  downstream_headers.insertHost().value(std::string(absolute_url.host_and_port()));
  downstream_headers.insertPath().value(std::string(absolute_url.path_and_query_params()));

  return true;
}

} // namespace

void FilterUtility::setUpstreamScheme(Http::HeaderMap& headers,
                                      const Upstream::ClusterInfo& cluster) {
  if (cluster.transportSocketFactory().implementsSecureTransport()) {
    headers.insertScheme().value().setReference(Http::Headers::get().SchemeValues.Https);
  } else {
    headers.insertScheme().value().setReference(Http::Headers::get().SchemeValues.Http);
  }
}

bool FilterUtility::shouldShadow(const ShadowPolicy& policy, Runtime::Loader& runtime,
                                 uint64_t stable_random) {
  if (policy.cluster().empty()) {
    return false;
  }

  if (policy.defaultValue().numerator() > 0) {
    return runtime.snapshot().featureEnabled(policy.runtimeKey(), policy.defaultValue(),
                                             stable_random);
  }

  if (!policy.runtimeKey().empty() &&
      !runtime.snapshot().featureEnabled(policy.runtimeKey(), 0, stable_random, 10000UL)) {
    return false;
  }

  return true;
}

FilterUtility::TimeoutData
FilterUtility::finalTimeout(const RouteEntry& route, Http::HeaderMap& request_headers,
                            bool insert_envoy_expected_request_timeout_ms, bool grpc_request) {
  // See if there is a user supplied timeout in a request header. If there is we take that.
  // Otherwise if the request is gRPC and a maximum gRPC timeout is configured we use the timeout
  // in the gRPC headers (or infinity when gRPC headers have no timeout), but cap that timeout to
  // the configured maximum gRPC timeout (which may also be infinity, represented by a 0 value),
  // or the default from the route config otherwise.
  TimeoutData timeout;
  if (grpc_request && route.maxGrpcTimeout()) {
    const std::chrono::milliseconds max_grpc_timeout = route.maxGrpcTimeout().value();
    std::chrono::milliseconds grpc_timeout = Grpc::Common::getGrpcTimeout(request_headers);
    if (route.grpcTimeoutOffset()) {
      // We only apply the offset if it won't result in grpc_timeout hitting 0 or below, as
      // setting it to 0 means infinity and a negative timeout makes no sense.
      const auto offset = *route.grpcTimeoutOffset();
      if (offset < grpc_timeout) {
        grpc_timeout -= offset;
      }
    }

    // Cap gRPC timeout to the configured maximum considering that 0 means infinity.
    if (max_grpc_timeout != std::chrono::milliseconds(0) &&
        (grpc_timeout == std::chrono::milliseconds(0) || grpc_timeout > max_grpc_timeout)) {
      grpc_timeout = max_grpc_timeout;
    }
    timeout.global_timeout_ = grpc_timeout;
  } else {
    timeout.global_timeout_ = route.timeout();
  }
  timeout.per_try_timeout_ = route.retryPolicy().perTryTimeout();

  Http::HeaderEntry* header_timeout_entry = request_headers.EnvoyUpstreamRequestTimeoutMs();
  uint64_t header_timeout;
  if (header_timeout_entry) {
    // TODO(dnoe): Migrate to pure string_view (#6580)
    if (StringUtil::atoull(std::string(header_timeout_entry->value().getStringView()).c_str(),
                           header_timeout)) {
      timeout.global_timeout_ = std::chrono::milliseconds(header_timeout);
    }
    request_headers.removeEnvoyUpstreamRequestTimeoutMs();
  }

  // See if there is a per try/retry timeout. If it's >= global we just ignore it.
  Http::HeaderEntry* per_try_timeout_entry = request_headers.EnvoyUpstreamRequestPerTryTimeoutMs();
  if (per_try_timeout_entry) {
    // TODO(dnoe): Migrate to pure string_view (#6580)
    if (StringUtil::atoull(std::string(per_try_timeout_entry->value().getStringView()).c_str(),
                           header_timeout)) {
      timeout.per_try_timeout_ = std::chrono::milliseconds(header_timeout);
    }
    request_headers.removeEnvoyUpstreamRequestPerTryTimeoutMs();
  }

  if (timeout.per_try_timeout_ >= timeout.global_timeout_) {
    timeout.per_try_timeout_ = std::chrono::milliseconds(0);
  }

  // See if there is any timeout to write in the expected timeout header.
  uint64_t expected_timeout = timeout.per_try_timeout_.count();
  if (expected_timeout == 0) {
    expected_timeout = timeout.global_timeout_.count();
  }

  if (insert_envoy_expected_request_timeout_ms && expected_timeout > 0) {
    request_headers.insertEnvoyExpectedRequestTimeoutMs().value(expected_timeout);
  }

  // If we've configured max_grpc_timeout, override the grpc-timeout header with
  // the expected timeout. This ensures that the optional per try timeout is reflected
  // in grpc-timeout, ensuring that the upstream gRPC server is aware of the actual timeout.
  // If the expected timeout is 0 set no timeout, as Envoy treats 0 as infinite timeout.
  if (grpc_request && route.maxGrpcTimeout() && expected_timeout != 0) {
    Grpc::Common::toGrpcTimeout(std::chrono::milliseconds(expected_timeout),
                                request_headers.insertGrpcTimeout().value());
  }

  return timeout;
}

Filter::~Filter() {
  // Upstream resources should already have been cleaned.
  ASSERT(upstream_requests_.empty());
  ASSERT(!retry_state_);
}

const std::string Filter::upstreamZone(Upstream::HostDescriptionConstSharedPtr upstream_host) {
  // TODO(PiotrSikora): Switch back to std::string& when string == std::string.
  return upstream_host ? upstream_host->locality().zone() : "";
}

void Filter::chargeUpstreamCode(uint64_t response_status_code,
                                const Http::HeaderMap& response_headers,
                                Upstream::HostDescriptionConstSharedPtr upstream_host,
                                bool dropped) {
  // Passing the response_status_code explicitly is an optimization to avoid
  // multiple calls to slow Http::Utility::getResponseStatus.
  ASSERT(response_status_code == Http::Utility::getResponseStatus(response_headers));
  if (config_.emit_dynamic_stats_ && !callbacks_->streamInfo().healthCheck()) {
    const Http::HeaderEntry* upstream_canary_header = response_headers.EnvoyUpstreamCanary();
    const Http::HeaderEntry* internal_request_header = downstream_headers_->EnvoyInternalRequest();

    const bool is_canary = (upstream_canary_header && upstream_canary_header->value() == "true") ||
                           (upstream_host ? upstream_host->canary() : false);
    const bool internal_request =
        internal_request_header && internal_request_header->value() == "true";

    // TODO(mattklein123): Remove copy when G string compat issues are fixed.
    const std::string zone_name = config_.local_info_.zoneName();
    const std::string upstream_zone = upstreamZone(upstream_host);

    Http::CodeStats::ResponseStatInfo info{config_.scope_,
                                           cluster_->statsScope(),
                                           EMPTY_STRING,
                                           response_status_code,
                                           internal_request,
                                           route_entry_->virtualHost().name(),
                                           request_vcluster_ ? request_vcluster_->name()
                                                             : EMPTY_STRING,
                                           zone_name,
                                           upstream_zone,
                                           is_canary};

    Http::CodeStats& code_stats = httpContext().codeStats();
    code_stats.chargeResponseStat(info);

    if (!alt_stat_prefix_.empty()) {
      Http::CodeStats::ResponseStatInfo info{config_.scope_,   cluster_->statsScope(),
                                             alt_stat_prefix_, response_status_code,
                                             internal_request, EMPTY_STRING,
                                             EMPTY_STRING,     zone_name,
                                             upstream_zone,    is_canary};

      code_stats.chargeResponseStat(info);
    }

    if (dropped) {
      cluster_->loadReportStats().upstream_rq_dropped_.inc();
    }
    if (upstream_host && Http::CodeUtility::is5xx(response_status_code)) {
      upstream_host->stats().rq_error_.inc();
    }
  }
}

void Filter::chargeUpstreamCode(Http::Code code,
                                Upstream::HostDescriptionConstSharedPtr upstream_host,
                                bool dropped) {
  const uint64_t response_status_code = enumToInt(code);
  Http::HeaderMapImpl fake_response_headers{
      {Http::Headers::get().Status, std::to_string(response_status_code)}};
  chargeUpstreamCode(response_status_code, fake_response_headers, upstream_host, dropped);
}

Http::FilterHeadersStatus Filter::decodeHeaders(Http::HeaderMap& headers, bool end_stream) {
  // Do a common header check. We make sure that all outgoing requests have all HTTP/2 headers.
  // These get stripped by HTTP/1 codec where applicable.
  ASSERT(headers.Path());
  ASSERT(headers.Method());
  ASSERT(headers.Host());

  downstream_headers_ = &headers;

  // TODO: Maybe add a filter API for this.
  grpc_request_ = Grpc::Common::hasGrpcContentType(headers);

  // Only increment rq total stat if we actually decode headers here. This does not count requests
  // that get handled by earlier filters.
  config_.stats_.rq_total_.inc();

  // Determine if there is a route entry or a direct response for the request.
  route_ = callbacks_->route();
  if (!route_) {
    config_.stats_.no_route_.inc();
    ENVOY_STREAM_LOG(debug, "no cluster match for URL '{}'", *callbacks_,
                     headers.Path()->value().getStringView());

    callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::NoRouteFound);
    callbacks_->sendLocalReply(Http::Code::NotFound, "", nullptr, absl::nullopt);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Determine if there is a direct response for the request.
  const auto* direct_response = route_->directResponseEntry();
  if (direct_response != nullptr) {
    config_.stats_.rq_direct_response_.inc();
    direct_response->rewritePathHeader(headers, !config_.suppress_envoy_headers_);
    callbacks_->sendLocalReply(
        direct_response->responseCode(), direct_response->responseBody(),
        [this, direct_response,
         &request_headers = headers](Http::HeaderMap& response_headers) -> void {
          const auto new_path = direct_response->newPath(request_headers);
          if (!new_path.empty()) {
            response_headers.addReferenceKey(Http::Headers::get().Location, new_path);
          }
          direct_response->finalizeResponseHeaders(response_headers, callbacks_->streamInfo());
        },
        absl::nullopt);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // A route entry matches for the request.
  route_entry_ = route_->routeEntry();
  Upstream::ThreadLocalCluster* cluster = config_.cm_.get(route_entry_->clusterName());
  if (!cluster) {
    config_.stats_.no_cluster_.inc();
    ENVOY_STREAM_LOG(debug, "unknown cluster '{}'", *callbacks_, route_entry_->clusterName());

    callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::NoRouteFound);
    callbacks_->sendLocalReply(route_entry_->clusterNotFoundResponseCode(), "", nullptr,
                               absl::nullopt);
    return Http::FilterHeadersStatus::StopIteration;
  }
  cluster_ = cluster->info();

  // Set up stat prefixes, etc.
  request_vcluster_ = route_entry_->virtualCluster(headers);
  ENVOY_STREAM_LOG(debug, "cluster '{}' match for URL '{}'", *callbacks_,
                   route_entry_->clusterName(), headers.Path()->value().getStringView());

  const Http::HeaderEntry* request_alt_name = headers.EnvoyUpstreamAltStatName();
  if (request_alt_name) {
    alt_stat_prefix_ = std::string(request_alt_name->value().getStringView()) + ".";
    headers.removeEnvoyUpstreamAltStatName();
  }

  // See if we are supposed to immediately kill some percentage of this cluster's traffic.
  if (cluster_->maintenanceMode()) {
    callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::UpstreamOverflow);
    chargeUpstreamCode(Http::Code::ServiceUnavailable, nullptr, true);
    callbacks_->sendLocalReply(
        Http::Code::ServiceUnavailable, "maintenance mode",
        [this](Http::HeaderMap& headers) {
          if (!config_.suppress_envoy_headers_) {
            headers.insertEnvoyOverloaded().value(Http::Headers::get().EnvoyOverloadedValues.True);
          }
        },
        absl::nullopt);
    cluster_->stats().upstream_rq_maintenance_mode_.inc();
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Fetch a connection pool for the upstream cluster.
  Http::ConnectionPool::Instance* conn_pool = getConnPool();
  if (!conn_pool) {
    sendNoHealthyUpstreamResponse();
    return Http::FilterHeadersStatus::StopIteration;
  }

  timeout_ = FilterUtility::finalTimeout(*route_entry_, headers, !config_.suppress_envoy_headers_,
                                         grpc_request_);

  // If this header is set with any value, use an alternate response code on timeout
  if (headers.EnvoyUpstreamRequestTimeoutAltResponse()) {
    timeout_response_code_ = Http::Code::NoContent;
    headers.removeEnvoyUpstreamRequestTimeoutAltResponse();
  }

  include_attempt_count_ = route_entry_->includeAttemptCount();
  if (include_attempt_count_) {
    headers.insertEnvoyAttemptCount().value(attempt_count_);
  }

  // Inject the active span's tracing context into the request headers.
  callbacks_->activeSpan().injectContext(headers);

  route_entry_->finalizeRequestHeaders(headers, callbacks_->streamInfo(),
                                       !config_.suppress_envoy_headers_);
  FilterUtility::setUpstreamScheme(headers, *cluster_);

  // Ensure an http transport scheme is selected before continuing with decoding.
  ASSERT(headers.Scheme());

  retry_state_ =
      createRetryState(route_entry_->retryPolicy(), headers, *cluster_, config_.runtime_,
                       config_.random_, callbacks_->dispatcher(), route_entry_->priority());
  do_shadowing_ = FilterUtility::shouldShadow(route_entry_->shadowPolicy(), config_.runtime_,
                                              callbacks_->streamId());

  ENVOY_STREAM_LOG(debug, "router decoding headers:\n{}", *callbacks_, headers);

  UpstreamRequestPtr upstream_request = std::make_unique<UpstreamRequest>(*this, *conn_pool);
  upstream_request->moveIntoList(std::move(upstream_request), upstream_requests_);
  upstream_requests_.front()->encodeHeaders(end_stream);
  if (end_stream) {
    onRequestComplete();
  }

  return Http::FilterHeadersStatus::StopIteration;
}

Http::ConnectionPool::Instance* Filter::getConnPool() {
  // Choose protocol based on cluster configuration and downstream connection
  // Note: Cluster may downgrade HTTP2 to HTTP1 based on runtime configuration.
  auto features = cluster_->features();

  Http::Protocol protocol;
  if (features & Upstream::ClusterInfo::Features::USE_DOWNSTREAM_PROTOCOL) {
    protocol = callbacks_->streamInfo().protocol().value();
  } else {
    protocol = (features & Upstream::ClusterInfo::Features::HTTP2) ? Http::Protocol::Http2
                                                                   : Http::Protocol::Http11;
  }
  return config_.cm_.httpConnPoolForCluster(route_entry_->clusterName(), route_entry_->priority(),
                                            protocol, this);
}

void Filter::sendNoHealthyUpstreamResponse() {
  callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::NoHealthyUpstream);
  chargeUpstreamCode(Http::Code::ServiceUnavailable, nullptr, false);
  callbacks_->sendLocalReply(Http::Code::ServiceUnavailable, "no healthy upstream", nullptr,
                             absl::nullopt);
}

Http::FilterDataStatus Filter::decodeData(Buffer::Instance& data, bool end_stream) {
  ASSERT(upstream_requests_.size() == 1);
  bool buffering = (retry_state_ && retry_state_->enabled()) || do_shadowing_;
  if (buffering && buffer_limit_ > 0 &&
      getLength(callbacks_->decodingBuffer()) + data.length() > buffer_limit_) {
    // The request is larger than we should buffer. Give up on the retry/shadow
    cluster_->stats().retry_or_shadow_abandoned_.inc();
    retry_state_.reset();
    buffering = false;
    do_shadowing_ = false;
  }

  if (buffering) {
    // If we are going to buffer for retries or shadowing, we need to make a copy before encoding
    // since it's all moves from here on.
    Buffer::OwnedImpl copy(data);
    upstream_requests_.front()->encodeData(copy, end_stream);

    // If we are potentially going to retry or shadow this request we need to buffer.
    // This will not cause the connection manager to 413 because before we hit the
    // buffer limit we give up on retries and buffering. We must buffer using addDecodedData()
    // so that all buffered data is available by the time we do request complete processing and
    // potentially shadow.
    callbacks_->addDecodedData(data, true);
  } else {
    upstream_requests_.front()->encodeData(data, end_stream);
  }

  if (end_stream) {
    onRequestComplete();
  }

  return Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus Filter::decodeTrailers(Http::HeaderMap& trailers) {
  ENVOY_STREAM_LOG(debug, "router decoding trailers:\n{}", *callbacks_, trailers);
  downstream_trailers_ = &trailers;
  ASSERT(upstream_requests_.size() == 1);
  upstream_requests_.front()->encodeTrailers(trailers);
  onRequestComplete();
  return Http::FilterTrailersStatus::StopIteration;
}

void Filter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;
  // As the decoder filter only pushes back via watermarks once data has reached
  // it, it can latch the current buffer limit and does not need to update the
  // limit if another filter increases it.
  buffer_limit_ = callbacks_->decoderBufferLimit();
}

void Filter::cleanup() {
  ASSERT(upstream_requests_.size() <= 1);
  // UpstreamRequests are only destroyed in this method (cleanup()) or when we
  // do a retry (setupRetry()). In the latter case we don't want to save the
  // upstream timings to the downstream info.
  if (upstream_requests_.size() == 1) {
    UpstreamRequestPtr upstream_request =
        upstream_requests_.back()->removeFromList(upstream_requests_);
    callbacks_->streamInfo().setUpstreamTiming(upstream_request->upstream_timing_);
  }
  retry_state_.reset();
  if (response_timeout_) {
    response_timeout_->disableTimer();
    response_timeout_.reset();
  }
}

void Filter::maybeDoShadowing() {
  if (!do_shadowing_) {
    return;
  }

  ASSERT(!route_entry_->shadowPolicy().cluster().empty());
  Http::MessagePtr request(new Http::RequestMessageImpl(
      Http::HeaderMapPtr{new Http::HeaderMapImpl(*downstream_headers_)}));
  if (callbacks_->decodingBuffer()) {
    request->body() = std::make_unique<Buffer::OwnedImpl>(*callbacks_->decodingBuffer());
  }
  if (downstream_trailers_) {
    request->trailers(Http::HeaderMapPtr{new Http::HeaderMapImpl(*downstream_trailers_)});
  }

  config_.shadowWriter().shadow(route_entry_->shadowPolicy().cluster(), std::move(request),
                                timeout_.global_timeout_);
}

void Filter::onRequestComplete() {
  // This should be called exactly once, when the downstream request has been received in full.
  ASSERT(!downstream_end_stream_);
  downstream_end_stream_ = true;
  Event::Dispatcher& dispatcher = callbacks_->dispatcher();
  downstream_request_complete_time_ = dispatcher.timeSource().monotonicTime();

  // Possible that we got an immediate reset.
  if (upstream_requests_.size() == 1) {
    // Even if we got an immediate reset, we could still shadow, but that is a riskier change and
    // seems unnecessary right now.
    maybeDoShadowing();

    if (timeout_.global_timeout_.count() > 0) {
      response_timeout_ = dispatcher.createTimer([this]() -> void { onResponseTimeout(); });
      response_timeout_->enableTimer(timeout_.global_timeout_);
    }

    for (auto& upstream_request : upstream_requests_) {
      if (upstream_request->create_per_try_timeout_on_request_complete_) {
        upstream_request->setupPerTryTimeout();
      }
    }
  }
}

void Filter::onDestroy() {
  if (upstream_requests_.size() == 1 && !attempting_internal_redirect_with_complete_stream_) {
    upstream_requests_.front()->resetStream();
  }
  cleanup();
}

void Filter::onResponseTimeout() {
  ENVOY_STREAM_LOG(debug, "upstream timeout", *callbacks_);
  cluster_->stats().upstream_rq_timeout_.inc();

  ASSERT(upstream_requests_.size() <= 1);
  if (upstream_requests_.size() == 1) {
    if (upstream_requests_.front()->upstream_host_) {
      upstream_requests_.front()->upstream_host_->stats().rq_timeout_.inc();
    }

    updateOutlierDetection(timeout_response_code_, *upstream_requests_.front().get());
    upstream_requests_.front()->resetStream();
  }

  onUpstreamTimeoutAbort(StreamInfo::ResponseFlag::UpstreamRequestTimeout);
}

void Filter::onPerTryTimeout(UpstreamRequest& upstream_request) {
  updateOutlierDetection(timeout_response_code_, upstream_request);

  if (maybeRetryReset(Http::StreamResetReason::LocalReset, upstream_request)) {
    return;
  }

  onUpstreamTimeoutAbort(StreamInfo::ResponseFlag::UpstreamRequestTimeout);
}

void Filter::updateOutlierDetection(Http::Code code, UpstreamRequest& upstream_request) {
  if (upstream_request.upstream_host_) {
    upstream_request.upstream_host_->outlierDetector().putHttpResponseCode(enumToInt(code));
  }
}

void Filter::onUpstreamTimeoutAbort(StreamInfo::ResponseFlag response_flags) {
  const absl::string_view body =
      timeout_response_code_ == Http::Code::GatewayTimeout ? "upstream request timeout" : "";
  onUpstreamAbort(timeout_response_code_, response_flags, body, false);
}

void Filter::onUpstreamAbort(Http::Code code, StreamInfo::ResponseFlag response_flags,
                             absl::string_view body, bool dropped) {
  ASSERT(upstream_requests_.size() <= 1);
  // If we have not yet sent anything downstream, send a response with an appropriate status code.
  // Otherwise just reset the ongoing response.
  if (downstream_response_started_) {
    if (upstream_requests_.size() == 1 && upstream_requests_.front()->grpc_rq_success_deferred_) {
      upstream_requests_.front()->upstream_host_->stats().rq_error_.inc();
      config_.stats_.rq_reset_after_downstream_response_started_.inc();
    }
    // This will destroy any created retry timers.
    cleanup();
    callbacks_->resetStream();
  } else {
    Upstream::HostDescriptionConstSharedPtr upstream_host;
    if (upstream_requests_.size() == 1) {
      upstream_host = upstream_requests_.front()->upstream_host_;
    }

    // This will destroy any created retry timers.
    cleanup();

    callbacks_->streamInfo().setResponseFlag(response_flags);

    chargeUpstreamCode(code, upstream_host, dropped);
    // If we had non-5xx but still have been reset by backend or timeout before
    // starting response, we treat this as an error. We only get non-5xx when
    // timeout_response_code_ is used for code above, where this member can
    // assume values such as 204 (NoContent).
    if (upstream_host != nullptr && !Http::CodeUtility::is5xx(enumToInt(code))) {
      upstream_host->stats().rq_error_.inc();
    }
    callbacks_->sendLocalReply(
        code, body,
        [dropped, this](Http::HeaderMap& headers) {
          if (dropped && !config_.suppress_envoy_headers_) {
            headers.insertEnvoyOverloaded().value(Http::Headers::get().EnvoyOverloadedValues.True);
          }
        },
        absl::nullopt);
  }
}

bool Filter::maybeRetryReset(Http::StreamResetReason reset_reason,
                             UpstreamRequest& upstream_request) {
  // We don't retry if we already started the response.
  if (downstream_response_started_ || !retry_state_) {
    return false;
  }

  Upstream::HostDescriptionConstSharedPtr upstream_host;
  upstream_host = upstream_request.upstream_host_;

  // Notify retry modifiers about the attempted host.
  if (upstream_host != nullptr) {
    retry_state_->onHostAttempted(upstream_host);
  }

  const RetryStatus retry_status =
      retry_state_->shouldRetryReset(reset_reason, [this]() -> void { doRetry(); });
  if (retry_status == RetryStatus::Yes && setupRetry(true)) {
    if (upstream_host) {
      upstream_host->stats().rq_error_.inc();
    }
    return true;
  } else if (retry_status == RetryStatus::NoOverflow) {
    callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::UpstreamOverflow);
  } else if (retry_status == RetryStatus::NoRetryLimitExceeded) {
    callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::UpstreamRetryLimitExceeded);
  }

  return false;
}

void Filter::onUpstreamReset(Http::StreamResetReason reset_reason,
                             absl::string_view transport_failure_reason,
                             UpstreamRequest& upstream_request) {
  ENVOY_STREAM_LOG(debug, "upstream reset: reset reason {}", *callbacks_,
                   Http::Utility::resetReasonToString(reset_reason));

  updateOutlierDetection(Http::Code::ServiceUnavailable, upstream_request);

  if (maybeRetryReset(reset_reason, upstream_request)) {
    return;
  }

  const StreamInfo::ResponseFlag response_flags = streamResetReasonToResponseFlag(reset_reason);
  const std::string body =
      absl::StrCat("upstream connect error or disconnect/reset before headers. reset reason: ",
                   Http::Utility::resetReasonToString(reset_reason));

  const bool dropped = reset_reason == Http::StreamResetReason::Overflow;
  callbacks_->streamInfo().setUpstreamTransportFailureReason(transport_failure_reason);
  onUpstreamAbort(Http::Code::ServiceUnavailable, response_flags, body, dropped);
}

StreamInfo::ResponseFlag
Filter::streamResetReasonToResponseFlag(Http::StreamResetReason reset_reason) {
  switch (reset_reason) {
  case Http::StreamResetReason::ConnectionFailure:
    return StreamInfo::ResponseFlag::UpstreamConnectionFailure;
  case Http::StreamResetReason::ConnectionTermination:
    return StreamInfo::ResponseFlag::UpstreamConnectionTermination;
  case Http::StreamResetReason::LocalReset:
  case Http::StreamResetReason::LocalRefusedStreamReset:
    return StreamInfo::ResponseFlag::LocalReset;
  case Http::StreamResetReason::Overflow:
    return StreamInfo::ResponseFlag::UpstreamOverflow;
  case Http::StreamResetReason::RemoteReset:
  case Http::StreamResetReason::RemoteRefusedStreamReset:
    return StreamInfo::ResponseFlag::UpstreamRemoteReset;
  }

  NOT_REACHED_GCOVR_EXCL_LINE;
}

void Filter::handleNon5xxResponseHeaders(const Http::HeaderMap& headers,
                                         UpstreamRequest& upstream_request, bool end_stream) {
  // We need to defer gRPC success until after we have processed grpc-status in
  // the trailers.
  if (grpc_request_) {
    if (end_stream) {
      absl::optional<Grpc::Status::GrpcStatus> grpc_status = Grpc::Common::getGrpcStatus(headers);
      if (grpc_status &&
          !Http::CodeUtility::is5xx(Grpc::Utility::grpcToHttpStatus(grpc_status.value()))) {
        upstream_request.upstream_host_->stats().rq_success_.inc();
      } else {
        upstream_request.upstream_host_->stats().rq_error_.inc();
      }
    } else {
      upstream_request.grpc_rq_success_deferred_ = true;
    }
  } else {
    upstream_request.upstream_host_->stats().rq_success_.inc();
  }
}

void Filter::onUpstream100ContinueHeaders(Http::HeaderMapPtr&& headers) {
  ENVOY_STREAM_LOG(debug, "upstream 100 continue", *callbacks_);

  downstream_response_started_ = true;
  // Don't send retries after 100-Continue has been sent on. Arguably we could attempt to do a
  // retry, assume the next upstream would also send an 100-Continue and swallow the second one
  // but it's sketchy (as the subsequent upstream might not send a 100-Continue) and not worth
  // the complexity until someone asks for it.
  retry_state_.reset();

  callbacks_->encode100ContinueHeaders(std::move(headers));
}

void Filter::onUpstreamHeaders(uint64_t response_code, Http::HeaderMapPtr&& headers,
                               UpstreamRequest& upstream_request, bool end_stream) {
  ASSERT(upstream_requests_.size() == 1);
  ENVOY_STREAM_LOG(debug, "upstream headers complete: end_stream={}", *callbacks_, end_stream);

  upstream_request.upstream_host_->outlierDetector().putHttpResponseCode(response_code);

  if (headers->EnvoyImmediateHealthCheckFail() != nullptr) {
    upstream_request.upstream_host_->healthChecker().setUnhealthy();
  }

  if (retry_state_) {
    // Notify retry modifiers about the attempted host.
    retry_state_->onHostAttempted(upstream_request.upstream_host_);

    // Capture upstream_host since setupRetry() in the following line will clear
    // upstream_request.
    const auto upstream_host = upstream_request.upstream_host_;
    const RetryStatus retry_status =
        retry_state_->shouldRetryHeaders(*headers, [this]() -> void { doRetry(); });
    if (retry_status == RetryStatus::Yes && setupRetry(end_stream)) {
      Http::CodeStats& code_stats = httpContext().codeStats();
      code_stats.chargeBasicResponseStat(cluster_->statsScope(), "retry.",
                                         static_cast<Http::Code>(response_code));
      upstream_host->stats().rq_error_.inc();
      return;
    } else if (retry_status == RetryStatus::NoOverflow) {
      callbacks_->streamInfo().setResponseFlag(StreamInfo::ResponseFlag::UpstreamOverflow);
    } else if (retry_status == RetryStatus::NoRetryLimitExceeded) {
      callbacks_->streamInfo().setResponseFlag(
          StreamInfo::ResponseFlag::UpstreamRetryLimitExceeded);
    }

    // Make sure any retry timers are destroyed since we may not call cleanup() if end_stream is
    // false.
    retry_state_.reset();
  }

  if (static_cast<Http::Code>(response_code) == Http::Code::Found &&
      route_entry_->internalRedirectAction() == InternalRedirectAction::Handle &&
      setupRedirect(*headers, upstream_request)) {
    return;
    // If the redirect could not be handled, fail open and let it pass to the
    // next downstream.
  }

  // Only send upstream service time if we received the complete request and this is not a
  // premature response.
  if (DateUtil::timePointValid(downstream_request_complete_time_)) {
    Event::Dispatcher& dispatcher = callbacks_->dispatcher();
    MonotonicTime response_received_time = dispatcher.timeSource().monotonicTime();
    std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        response_received_time - downstream_request_complete_time_);
    if (!config_.suppress_envoy_headers_) {
      headers->insertEnvoyUpstreamServiceTime().value(ms.count());
    }
  }

  upstream_request.upstream_canary_ =
      (headers->EnvoyUpstreamCanary() && headers->EnvoyUpstreamCanary()->value() == "true") ||
      upstream_request.upstream_host_->canary();
  chargeUpstreamCode(response_code, *headers, upstream_request.upstream_host_, false);
  if (!Http::CodeUtility::is5xx(response_code)) {
    handleNon5xxResponseHeaders(*headers, upstream_request, end_stream);
  }

  // Append routing cookies
  for (const auto& header_value : downstream_set_cookies_) {
    headers->addReferenceKey(Http::Headers::get().SetCookie, header_value);
  }

  // TODO(zuercher): If access to response_headers_to_add (at any level) is ever needed outside
  // Router::Filter we'll need to find a better location for this work. One possibility is to
  // provide finalizeResponseHeaders functions on the Router::Config and VirtualHost interfaces.
  route_entry_->finalizeResponseHeaders(*headers, callbacks_->streamInfo());

  downstream_response_started_ = true;
  if (end_stream) {
    onUpstreamComplete(upstream_request);
  }

  callbacks_->streamInfo().setResponseCodeDetails(
      StreamInfo::ResponseCodeDetails::get().ViaUpstream);
  callbacks_->encodeHeaders(std::move(headers), end_stream);
}

void Filter::onUpstreamData(Buffer::Instance& data, UpstreamRequest& upstream_request,
                            bool end_stream) {
  ASSERT(upstream_requests_.size() == 1);
  if (end_stream) {
    // gRPC request termination without trailers is an error.
    if (upstream_request.grpc_rq_success_deferred_) {
      upstream_request.upstream_host_->stats().rq_error_.inc();
    }
    onUpstreamComplete(upstream_request);
  }

  callbacks_->encodeData(data, end_stream);
}

void Filter::onUpstreamTrailers(Http::HeaderMapPtr&& trailers, UpstreamRequest& upstream_request) {
  ASSERT(upstream_requests_.size() == 1);
  if (upstream_request.grpc_rq_success_deferred_) {
    absl::optional<Grpc::Status::GrpcStatus> grpc_status = Grpc::Common::getGrpcStatus(*trailers);
    if (grpc_status &&
        !Http::CodeUtility::is5xx(Grpc::Utility::grpcToHttpStatus(grpc_status.value()))) {
      upstream_request.upstream_host_->stats().rq_success_.inc();
    } else {
      upstream_request.upstream_host_->stats().rq_error_.inc();
    }
  }
  onUpstreamComplete(upstream_request);
  callbacks_->encodeTrailers(std::move(trailers));
}

void Filter::onUpstreamMetadata(Http::MetadataMapPtr&& metadata_map) {
  callbacks_->encodeMetadata(std::move(metadata_map));
}

void Filter::onUpstreamComplete(UpstreamRequest& upstream_request) {
  if (!downstream_end_stream_) {
    upstream_request.resetStream();
  }

  if (config_.emit_dynamic_stats_ && !callbacks_->streamInfo().healthCheck() &&
      DateUtil::timePointValid(downstream_request_complete_time_)) {
    Event::Dispatcher& dispatcher = callbacks_->dispatcher();
    std::chrono::milliseconds response_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        dispatcher.timeSource().monotonicTime() - downstream_request_complete_time_);

    upstream_request.upstream_host_->outlierDetector().putResponseTime(response_time);

    const Http::HeaderEntry* internal_request_header = downstream_headers_->EnvoyInternalRequest();
    const bool internal_request =
        internal_request_header && internal_request_header->value() == "true";

    // TODO(mattklein123): Remove copy when G string compat issues are fixed.
    const std::string zone_name = config_.local_info_.zoneName();

    Http::CodeStats& code_stats = httpContext().codeStats();
    Http::CodeStats::ResponseTimingInfo info{config_.scope_,
                                             cluster_->statsScope(),
                                             EMPTY_STRING,
                                             response_time,
                                             upstream_request.upstream_canary_,
                                             internal_request,
                                             route_entry_->virtualHost().name(),
                                             request_vcluster_ ? request_vcluster_->name()
                                                               : EMPTY_STRING,
                                             zone_name,
                                             upstreamZone(upstream_request.upstream_host_)};

    code_stats.chargeResponseTiming(info);

    if (!alt_stat_prefix_.empty()) {
      Http::CodeStats::ResponseTimingInfo info{config_.scope_,
                                               cluster_->statsScope(),
                                               alt_stat_prefix_,
                                               response_time,
                                               upstream_request.upstream_canary_,
                                               internal_request,
                                               EMPTY_STRING,
                                               EMPTY_STRING,
                                               zone_name,
                                               upstreamZone(upstream_request.upstream_host_)};

      code_stats.chargeResponseTiming(info);
    }
  }

  cleanup();
}

bool Filter::setupRetry(bool end_stream) {
  // If we responded before the request was complete we don't bother doing a retry. This may not
  // catch certain cases where we are in full streaming mode and we have a connect timeout or an
  // overflow of some kind. However, in many cases deployments will use the buffer filter before
  // this filter which will make this a non-issue. The implementation of supporting retry in cases
  // where the request is not complete is more complicated so we will start with this for now.
  if (!downstream_end_stream_) {
    return false;
  }

  ASSERT(upstream_requests_.size() == 1);
  ENVOY_STREAM_LOG(debug, "performing retry", *callbacks_);
  if (!end_stream) {
    upstream_requests_.front()->resetStream();
  }

  upstream_requests_.front()->removeFromList(upstream_requests_);

  return true;
}

bool Filter::setupRedirect(const Http::HeaderMap& headers, UpstreamRequest& upstream_request) {
  ENVOY_STREAM_LOG(debug, "attempting internal redirect", *callbacks_);
  const Http::HeaderEntry* location = headers.Location();

  // If the internal redirect succeeds, callbacks_->recreateStream() will result in the destruction
  // of this filter before the stream is marked as complete, and onDestroy will reset the stream.
  //
  // Normally when a stream is complete we signal this by resetting the upstream but this cam not be
  // done in this case because if recreateStream fails, the "failure" path continues to call code
  // in onUpstreamHeaders which requires the upstream *not* be reset. To avoid onDestroy performing
  // a spurious stream reset in the case recreateStream() succeeds, we explicitly track stream
  // completion here and check it in onDestroy. This is annoyingly complicated but is better than
  // needlessly resetting streams.
  attempting_internal_redirect_with_complete_stream_ =
      upstream_request.upstream_timing_.last_upstream_rx_byte_received_ && downstream_end_stream_;

  // As with setupRetry, redirects are not supported for streaming requests yet.
  if (downstream_end_stream_ &&
      !callbacks_->decodingBuffer() && // Redirects with body not yet supported.
      location != nullptr &&
      convertRequestHeadersForInternalRedirect(*downstream_headers_, *location,
                                               *callbacks_->connection()) &&
      callbacks_->recreateStream()) {
    cluster_->stats().upstream_internal_redirect_succeeded_total_.inc();
    return true;
  }

  attempting_internal_redirect_with_complete_stream_ = false;

  ENVOY_STREAM_LOG(debug, "Internal redirect failed", *callbacks_);
  cluster_->stats().upstream_internal_redirect_failed_total_.inc();
  return false;
}

void Filter::doRetry() {
  is_retry_ = true;
  attempt_count_++;
  Http::ConnectionPool::Instance* conn_pool = getConnPool();
  if (!conn_pool) {
    sendNoHealthyUpstreamResponse();
    cleanup();
    return;
  }

  if (include_attempt_count_) {
    downstream_headers_->insertEnvoyAttemptCount().value(attempt_count_);
  }

  ASSERT(response_timeout_ || timeout_.global_timeout_.count() == 0);
  UpstreamRequestPtr upstream_request = std::make_unique<UpstreamRequest>(*this, *conn_pool);
  upstream_request->moveIntoList(std::move(upstream_request), upstream_requests_);
  upstream_requests_.front()->encodeHeaders(!callbacks_->decodingBuffer() && !downstream_trailers_);
  // It's possible we got immediately reset.
  if (upstream_requests_.size() == 1) {
    if (callbacks_->decodingBuffer()) {
      // If we are doing a retry we need to make a copy.
      Buffer::OwnedImpl copy(*callbacks_->decodingBuffer());
      upstream_requests_.front()->encodeData(copy, !downstream_trailers_);
    }

    if (downstream_trailers_) {
      upstream_requests_.front()->encodeTrailers(*downstream_trailers_);
    }
  }
}

Filter::UpstreamRequest::UpstreamRequest(Filter& parent, Http::ConnectionPool::Instance& pool)
    : parent_(parent), conn_pool_(pool), grpc_rq_success_deferred_(false),
      stream_info_(pool.protocol(), parent_.callbacks_->dispatcher().timeSource()),
      calling_encode_headers_(false), upstream_canary_(false), encode_complete_(false),
      encode_trailers_(false), create_per_try_timeout_on_request_complete_(false) {

  if (parent_.config_.start_child_span_) {
    span_ = parent_.callbacks_->activeSpan().spawnChild(
        parent_.callbacks_->tracingConfig(), "router " + parent.cluster_->name() + " egress",
        parent.timeSource().systemTime());
    span_->setTag(Tracing::Tags::get().Component, Tracing::Tags::get().Proxy);
  }

  stream_info_.healthCheck(parent_.callbacks_->streamInfo().healthCheck());
}

Filter::UpstreamRequest::~UpstreamRequest() {
  if (span_ != nullptr) {
    // TODO(mattklein123): Add tags based on what happened to this request (retries, reset, etc.).
    span_->finishSpan();
  }
  if (per_try_timeout_ != nullptr) {
    // Allows for testing.
    per_try_timeout_->disableTimer();
  }
  clearRequestEncoder();

  stream_info_.setUpstreamTiming(upstream_timing_);
  stream_info_.onRequestComplete();
  for (const auto& upstream_log : parent_.config_.upstream_logs_) {
    upstream_log->log(parent_.downstream_headers_, upstream_headers_, upstream_trailers_,
                      stream_info_);
  }
}

void Filter::UpstreamRequest::decode100ContinueHeaders(Http::HeaderMapPtr&& headers) {
  ASSERT(100 == Http::Utility::getResponseStatus(*headers));
  parent_.onUpstream100ContinueHeaders(std::move(headers));
}

void Filter::UpstreamRequest::decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) {
  // TODO(rodaine): This is actually measuring after the headers are parsed and not the first byte.
  upstream_timing_.onFirstUpstreamRxByteReceived(parent_.callbacks_->dispatcher().timeSource());
  maybeEndDecode(end_stream);

  upstream_headers_ = headers.get();
  const uint64_t response_code = Http::Utility::getResponseStatus(*headers);
  stream_info_.response_code_ = static_cast<uint32_t>(response_code);
  parent_.onUpstreamHeaders(response_code, std::move(headers), *this, end_stream);
}

void Filter::UpstreamRequest::decodeData(Buffer::Instance& data, bool end_stream) {
  maybeEndDecode(end_stream);
  stream_info_.addBytesReceived(data.length());
  parent_.onUpstreamData(data, *this, end_stream);
}

void Filter::UpstreamRequest::decodeTrailers(Http::HeaderMapPtr&& trailers) {
  maybeEndDecode(true);
  upstream_trailers_ = trailers.get();
  parent_.onUpstreamTrailers(std::move(trailers), *this);
}

void Filter::UpstreamRequest::decodeMetadata(Http::MetadataMapPtr&& metadata_map) {
  parent_.onUpstreamMetadata(std::move(metadata_map));
}

void Filter::UpstreamRequest::maybeEndDecode(bool end_stream) {
  if (end_stream) {
    upstream_timing_.onLastUpstreamRxByteReceived(parent_.callbacks_->dispatcher().timeSource());
  }
}

void Filter::UpstreamRequest::encodeHeaders(bool end_stream) {
  ASSERT(!encode_complete_);
  encode_complete_ = end_stream;

  // It's possible for a reset to happen inline within the newStream() call. In this case, we might
  // get deleted inline as well. Only write the returned handle out if it is not nullptr to deal
  // with this case.
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(*this, *this);
  if (handle) {
    conn_pool_stream_handle_ = handle;
  }
}

void Filter::UpstreamRequest::encodeData(Buffer::Instance& data, bool end_stream) {
  ASSERT(!encode_complete_);
  encode_complete_ = end_stream;

  if (!request_encoder_) {
    ENVOY_STREAM_LOG(trace, "buffering {} bytes", *parent_.callbacks_, data.length());
    if (!buffered_request_body_) {
      buffered_request_body_ = std::make_unique<Buffer::WatermarkBuffer>(
          [this]() -> void { this->enableDataFromDownstream(); },
          [this]() -> void { this->disableDataFromDownstream(); });
      buffered_request_body_->setWatermarks(parent_.buffer_limit_);
    }

    buffered_request_body_->move(data);
  } else {
    ENVOY_STREAM_LOG(trace, "proxying {} bytes", *parent_.callbacks_, data.length());
    stream_info_.addBytesSent(data.length());
    request_encoder_->encodeData(data, end_stream);
    if (end_stream) {
      upstream_timing_.onLastUpstreamTxByteSent(parent_.callbacks_->dispatcher().timeSource());
    }
  }
}

void Filter::UpstreamRequest::encodeTrailers(const Http::HeaderMap& trailers) {
  ASSERT(!encode_complete_);
  encode_complete_ = true;
  encode_trailers_ = true;

  if (!request_encoder_) {
    ENVOY_STREAM_LOG(trace, "buffering trailers", *parent_.callbacks_);
  } else {
    ENVOY_STREAM_LOG(trace, "proxying trailers", *parent_.callbacks_);
    request_encoder_->encodeTrailers(trailers);
    upstream_timing_.onLastUpstreamTxByteSent(parent_.callbacks_->dispatcher().timeSource());
  }
}

void Filter::UpstreamRequest::onResetStream(Http::StreamResetReason reason,
                                            absl::string_view transport_failure_reason) {
  clearRequestEncoder();
  if (!calling_encode_headers_) {
    stream_info_.setResponseFlag(parent_.streamResetReasonToResponseFlag(reason));
    parent_.onUpstreamReset(reason, transport_failure_reason, *this);
  } else {
    deferred_reset_reason_ = reason;
  }
}

void Filter::UpstreamRequest::resetStream() {
  if (conn_pool_stream_handle_) {
    ENVOY_STREAM_LOG(debug, "cancelling pool request", *parent_.callbacks_);
    ASSERT(!request_encoder_);
    conn_pool_stream_handle_->cancel();
    conn_pool_stream_handle_ = nullptr;
  }

  if (request_encoder_) {
    ENVOY_STREAM_LOG(debug, "resetting pool request", *parent_.callbacks_);
    request_encoder_->getStream().removeCallbacks(*this);
    request_encoder_->getStream().resetStream(Http::StreamResetReason::LocalReset);
  }
}

void Filter::UpstreamRequest::setupPerTryTimeout() {
  ASSERT(!per_try_timeout_);
  if (parent_.timeout_.per_try_timeout_.count() > 0) {
    per_try_timeout_ =
        parent_.callbacks_->dispatcher().createTimer([this]() -> void { onPerTryTimeout(); });
    per_try_timeout_->enableTimer(parent_.timeout_.per_try_timeout_);
  }
}

void Filter::UpstreamRequest::onPerTryTimeout() {
  // If we've sent anything downstream, ignore the per try timeout and let the response continue up
  // to the global timeout
  if (!parent_.downstream_response_started_) {
    ENVOY_STREAM_LOG(debug, "upstream per try timeout", *parent_.callbacks_);
    parent_.cluster_->stats().upstream_rq_per_try_timeout_.inc();
    if (upstream_host_) {
      upstream_host_->stats().rq_timeout_.inc();
    }
    resetStream();
    stream_info_.setResponseFlag(StreamInfo::ResponseFlag::UpstreamRequestTimeout);
    parent_.onPerTryTimeout(*this);
  } else {
    ENVOY_STREAM_LOG(debug,
                     "ignored upstream per try timeout due to already started downstream response",
                     *parent_.callbacks_);
  }
}

void Filter::UpstreamRequest::onPoolFailure(Http::ConnectionPool::PoolFailureReason reason,
                                            absl::string_view transport_failure_reason,
                                            Upstream::HostDescriptionConstSharedPtr host) {
  Http::StreamResetReason reset_reason = Http::StreamResetReason::ConnectionFailure;
  switch (reason) {
  case Http::ConnectionPool::PoolFailureReason::Overflow:
    reset_reason = Http::StreamResetReason::Overflow;
    break;
  case Http::ConnectionPool::PoolFailureReason::ConnectionFailure:
    reset_reason = Http::StreamResetReason::ConnectionFailure;
    break;
  }

  // Mimic an upstream reset.
  onUpstreamHostSelected(host);
  onResetStream(reset_reason, transport_failure_reason);
}

void Filter::UpstreamRequest::onPoolReady(Http::StreamEncoder& request_encoder,
                                          Upstream::HostDescriptionConstSharedPtr host) {
  ENVOY_STREAM_LOG(debug, "pool ready", *parent_.callbacks_);

  // TODO(ggreenway): set upstream local address in the StreamInfo.
  onUpstreamHostSelected(host);
  request_encoder.getStream().addCallbacks(*this);

  if (parent_.downstream_end_stream_) {
    setupPerTryTimeout();
  } else {
    create_per_try_timeout_on_request_complete_ = true;
  }

  conn_pool_stream_handle_ = nullptr;
  setRequestEncoder(request_encoder);
  calling_encode_headers_ = true;
  if (parent_.route_entry_->autoHostRewrite() && !host->hostname().empty()) {
    parent_.downstream_headers_->Host()->value(host->hostname());
  }

  if (span_ != nullptr) {
    span_->injectContext(*parent_.downstream_headers_);
  }

  upstream_timing_.onFirstUpstreamTxByteSent(parent_.callbacks_->dispatcher().timeSource());
  request_encoder.encodeHeaders(*parent_.downstream_headers_,
                                !buffered_request_body_ && encode_complete_ && !encode_trailers_);
  calling_encode_headers_ = false;

  // It is possible to get reset in the middle of an encodeHeaders() call. This happens for example
  // in the HTTP/2 codec if the frame cannot be encoded for some reason. This should never happen
  // but it's unclear if we have covered all cases so protect against it and test for it. One
  // specific example of a case where this happens is if we try to encode a total header size that
  // is too big in HTTP/2 (64K currently).
  if (deferred_reset_reason_) {
    onResetStream(deferred_reset_reason_.value(), absl::string_view());
  } else {
    if (buffered_request_body_) {
      stream_info_.addBytesSent(buffered_request_body_->length());
      request_encoder.encodeData(*buffered_request_body_, encode_complete_ && !encode_trailers_);
    }

    if (encode_trailers_) {
      request_encoder.encodeTrailers(*parent_.downstream_trailers_);
    }

    if (encode_complete_) {
      upstream_timing_.onLastUpstreamTxByteSent(parent_.callbacks_->dispatcher().timeSource());
    }
  }
}

RetryStatePtr
ProdFilter::createRetryState(const RetryPolicy& policy, Http::HeaderMap& request_headers,
                             const Upstream::ClusterInfo& cluster, Runtime::Loader& runtime,
                             Runtime::RandomGenerator& random, Event::Dispatcher& dispatcher,
                             Upstream::ResourcePriority priority) {
  return RetryStateImpl::create(policy, request_headers, cluster, runtime, random, dispatcher,
                                priority);
}

void Filter::UpstreamRequest::setRequestEncoder(Http::StreamEncoder& request_encoder) {
  request_encoder_ = &request_encoder;
  // Now that there is an encoder, have the connection manager inform the manager when the
  // downstream buffers are overrun. This may result in immediate watermark callbacks referencing
  // the encoder.
  parent_.callbacks_->addDownstreamWatermarkCallbacks(downstream_watermark_manager_);
}

void Filter::UpstreamRequest::clearRequestEncoder() {
  // Before clearing the encoder, unsubscribe from callbacks.
  if (request_encoder_) {
    parent_.callbacks_->removeDownstreamWatermarkCallbacks(downstream_watermark_manager_);
  }
  request_encoder_ = nullptr;
}

void Filter::UpstreamRequest::DownstreamWatermarkManager::onAboveWriteBufferHighWatermark() {
  ASSERT(parent_.request_encoder_);
  ASSERT(parent_.parent_.upstream_requests_.size() == 1);
  // The downstream connection is overrun. Pause reads from upstream.
  // If there are multiple calls to readDisable either the codec (H2) or the underlying
  // Network::Connection (H1) will handle reference counting.
  parent_.parent_.cluster_->stats().upstream_flow_control_paused_reading_total_.inc();
  parent_.request_encoder_->getStream().readDisable(true);
}

void Filter::UpstreamRequest::DownstreamWatermarkManager::onBelowWriteBufferLowWatermark() {
  ASSERT(parent_.request_encoder_);
  ASSERT(parent_.parent_.upstream_requests_.size() == 1);
  // One source of connection blockage has buffer available. Pass this on to the stream, which
  // will resume reads if this was the last remaining high watermark.
  parent_.parent_.cluster_->stats().upstream_flow_control_resumed_reading_total_.inc();
  parent_.request_encoder_->getStream().readDisable(false);
}

} // namespace Router
} // namespace Envoy
