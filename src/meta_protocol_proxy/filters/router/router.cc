#include "src/meta_protocol_proxy/filters/router/router.h"

#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "src/meta_protocol_proxy/app_exception.h"
#include "src/meta_protocol_proxy/codec/codec.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MetaProtocolProxy {
namespace Router {

void Router::onDestroy() {
  if (upstream_request_) {
    upstream_request_->resetStream();
  }
  cleanup();
}

void Router::setDecoderFilterCallbacks(DecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;
}

FilterStatus Router::onMessageDecoded(MetadataSharedPtr metadata, MutationSharedPtr) {
  route_ = callbacks_->route();
  if (!route_) {
    ENVOY_STREAM_LOG(debug, "meta protocol router: no cluster match for request '{}'", *callbacks_,
                     metadata->getRequestId());
    callbacks_->sendLocalReply(
        AppException(Error{ErrorType::RouteNotFound,
                           fmt::format("meta protocol router: no cluster match for request '{}'",
                                       metadata->getRequestId())}),
        false);
    return FilterStatus::StopIteration;
  }

  route_entry_ = route_->routeEntry();

  Upstream::ThreadLocalCluster* cluster =
      cluster_manager_.getThreadLocalCluster(route_entry_->clusterName());
  if (!cluster) {
    ENVOY_STREAM_LOG(debug, "meta protocol router: unknown cluster '{}'", *callbacks_,
                     route_entry_->clusterName());
    callbacks_->sendLocalReply(
        AppException(Error{ErrorType::ClusterNotFound,
                           fmt::format("meta protocol router: unknown cluster '{}'",
                                       route_entry_->clusterName())}),
        false);
    return FilterStatus::StopIteration;
  }

  cluster_ = cluster->info();
  ENVOY_STREAM_LOG(debug, "meta protocol router: cluster {} match for request '{}'", *callbacks_,
                   cluster_->name(), metadata->getRequestId());

  if (cluster_->maintenanceMode()) {
    callbacks_->sendLocalReply(
        AppException(Error{ErrorType::Unspecified,
                           fmt::format("meta protocol router: maintenance mode for cluster '{}'",
                                       route_entry_->clusterName())}),
        false);
    return FilterStatus::StopIteration;
  }

  auto conn_pool = cluster->tcpConnPool(Upstream::ResourcePriority::Default, this);
  if (!conn_pool) {
    callbacks_->sendLocalReply(
        AppException(Error{ErrorType::NoHealthyUpstream,
                           fmt::format("meta protocol router: no healthy upstream for '{}'",
                                       route_entry_->clusterName())}),
        false);
    return FilterStatus::StopIteration;
  }

  ENVOY_STREAM_LOG(debug, "meta protocol router: decoding request", *callbacks_);

  // TODO encode mutation into the outgoing request
  upstream_request_buffer_.move(metadata->getOriginMessage(),
                                metadata->getOriginMessage().length());
  upstream_request_ = std::make_unique<UpstreamRequest>(*this, *conn_pool, metadata);
  return upstream_request_->start();
}

void Router::setEncoderFilterCallbacks(EncoderFilterCallbacks& callbacks) {
  encoder_callbacks_ = &callbacks;
}

FilterStatus Router::onMessageEncoded(MetadataSharedPtr metadata, MutationSharedPtr) {
  if (upstream_request_ == nullptr) {
    return FilterStatus::Continue;
  }

  ENVOY_STREAM_LOG(trace, "meta protocol router: response status: {}", *encoder_callbacks_,
                   metadata->getResponseStatus());

  switch (metadata->getResponseStatus()) {
  case ResponseStatus::Ok:
    if (metadata->getMessageType() == MessageType::Error) {
      upstream_request_->upstream_host_->outlierDetector().putResult(
          Upstream::Outlier::Result::ExtOriginRequestFailed);
    } else {
      upstream_request_->upstream_host_->outlierDetector().putResult(
          Upstream::Outlier::Result::ExtOriginRequestSuccess);
    }
    break;
  case ResponseStatus::Error:
    upstream_request_->upstream_host_->outlierDetector().putResult(
        Upstream::Outlier::Result::ExtOriginRequestFailed);
    break;
  default:
    break;
  }

  return FilterStatus::Continue;
}

void Router::onUpstreamData(Buffer::Instance& data, bool end_stream) {
  ASSERT(!upstream_request_->response_complete_);

  ENVOY_STREAM_LOG(trace, "meta protocol router: reading response: {} bytes", *callbacks_,
                   data.length());

  // Handle normal response.
  if (!upstream_request_->response_started_) {
    callbacks_->startUpstreamResponse();
    upstream_request_->response_started_ = true;
  }

  UpstreamResponseStatus status = callbacks_->upstreamData(data);
  if (status == UpstreamResponseStatus::Complete) {
    ENVOY_STREAM_LOG(debug, "meta protocol router: response complete", *callbacks_);
    upstream_request_->onResponseComplete();
    cleanup();
    return;
  } else if (status == UpstreamResponseStatus::Reset) {
    ENVOY_STREAM_LOG(debug, "meta protocol router: upstream reset", *callbacks_);
    // When the upstreamData function returns Reset,
    // the current stream is already released from the upper layer,
    // so there is no need to call callbacks_->resetStream() to notify
    // the upper layer to release the stream.
    upstream_request_->resetStream();
    return;
  }

  if (end_stream) {
    // Response is incomplete, but no more data is coming.
    ENVOY_STREAM_LOG(debug, "meta protocol router: response underflow", *callbacks_);
    upstream_request_->onResetStream(ConnectionPool::PoolFailureReason::RemoteConnectionFailure);
    upstream_request_->onResponseComplete();
    cleanup();
  }
}

void Router::onEvent(Network::ConnectionEvent event) {
  if (!upstream_request_ || upstream_request_->response_complete_) {
    // Client closed connection after completing response.
    ENVOY_LOG(debug, "meta protocol upstream request: the upstream request had completed");
    return;
  }

  if (upstream_request_->stream_reset_ && event == Network::ConnectionEvent::LocalClose) {
    ENVOY_LOG(debug, "meta protocol upstream request: the stream reset");
    return;
  }

  switch (event) {
  case Network::ConnectionEvent::RemoteClose:
    upstream_request_->onResetStream(ConnectionPool::PoolFailureReason::RemoteConnectionFailure);
    upstream_request_->upstream_host_->outlierDetector().putResult(
        Upstream::Outlier::Result::LocalOriginConnectFailed);
    break;
  case Network::ConnectionEvent::LocalClose:
    upstream_request_->onResetStream(ConnectionPool::PoolFailureReason::LocalConnectionFailure);
    break;
  default:
    // Connected is consumed by the connection pool.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

const Network::Connection* Router::downstreamConnection() const {
  return callbacks_ != nullptr ? callbacks_->connection() : nullptr;
}

void Router::cleanup() {
  if (upstream_request_) {
    upstream_request_.reset();
  }
}

Router::UpstreamRequest::UpstreamRequest(Router& parent, Tcp::ConnectionPool::Instance& pool,
                                         MetadataSharedPtr& metadata)
    : parent_(parent), conn_pool_(pool), metadata_(metadata), request_complete_(false),
      response_started_(false), response_complete_(false), stream_reset_(false) {}

Router::UpstreamRequest::~UpstreamRequest() = default;

FilterStatus Router::UpstreamRequest::start() {
  Tcp::ConnectionPool::Cancellable* handle = conn_pool_.newConnection(*this);
  if (handle) {
    // Pause while we wait for a connection.
    conn_pool_handle_ = handle;
    return FilterStatus::StopIteration;
  }

  return FilterStatus::Continue;
}

void Router::UpstreamRequest::resetStream() {
  stream_reset_ = true;

  if (conn_pool_handle_) {
    ASSERT(!conn_data_);
    conn_pool_handle_->cancel(Tcp::ConnectionPool::CancelPolicy::Default);
    conn_pool_handle_ = nullptr;
    ENVOY_LOG(debug, "meta protocol upstream request: reset connection pool handler");
  }

  if (conn_data_) {
    ASSERT(!conn_pool_handle_);
    conn_data_->connection().close(Network::ConnectionCloseType::NoFlush);
    conn_data_.reset();
    ENVOY_LOG(debug, "meta protocol upstream request: reset connection data");
  }
}

void Router::UpstreamRequest::encodeData(Buffer::Instance& data) {
  ASSERT(conn_data_);
  ASSERT(!conn_pool_handle_);

  ENVOY_STREAM_LOG(trace, "proxying {} bytes", *parent_.callbacks_, data.length());
  conn_data_->connection().write(data, false);
}

void Router::UpstreamRequest::onPoolFailure(ConnectionPool::PoolFailureReason reason,
                                            Upstream::HostDescriptionConstSharedPtr host) {
  conn_pool_handle_ = nullptr;

  // Mimic an upstream reset.
  onUpstreamHostSelected(host);
  onResetStream(reason);

  parent_.upstream_request_buffer_.drain(parent_.upstream_request_buffer_.length());

  // If it is a connection error, it means that the connection pool returned
  // the error asynchronously and the upper layer needs to be notified to continue decoding.
  // If it is a non-connection error, it is returned synchronously from the connection pool
  // and is still in the callback at the current Filter, nothing to do.
  if (reason == ConnectionPool::PoolFailureReason::Timeout ||
      reason == ConnectionPool::PoolFailureReason::LocalConnectionFailure ||
      reason == ConnectionPool::PoolFailureReason::RemoteConnectionFailure) {
    if (reason == ConnectionPool::PoolFailureReason::Timeout) {
      host->outlierDetector().putResult(Upstream::Outlier::Result::LocalOriginTimeout);
    } else if (reason == ConnectionPool::PoolFailureReason::RemoteConnectionFailure) {
      host->outlierDetector().putResult(Upstream::Outlier::Result::LocalOriginConnectFailed);
    }
    parent_.callbacks_->continueDecoding();
  }
}

void Router::UpstreamRequest::onPoolReady(Tcp::ConnectionPool::ConnectionDataPtr&& conn_data,
                                          Upstream::HostDescriptionConstSharedPtr host) {
  ENVOY_LOG(debug, "meta protocol upstream request: tcp connection has ready");

  // Only invoke continueDecoding if we'd previously stopped the filter chain.
  bool continue_decoding = conn_pool_handle_ != nullptr;

  onUpstreamHostSelected(host);
  host->outlierDetector().putResult(Upstream::Outlier::Result::LocalOriginConnectSuccess);

  conn_data_ = std::move(conn_data);
  conn_data_->addUpstreamCallbacks(parent_);
  conn_pool_handle_ = nullptr;

  onRequestStart(continue_decoding);
  encodeData(parent_.upstream_request_buffer_);
}

void Router::UpstreamRequest::onRequestStart(bool continue_decoding) {
  ENVOY_LOG(debug, "meta protocol upstream request: start sending data to the server {}",
            upstream_host_->address()->asString());

  if (continue_decoding) {
    parent_.callbacks_->continueDecoding();
  }
  onRequestComplete();
}

void Router::UpstreamRequest::onRequestComplete() { request_complete_ = true; }

void Router::UpstreamRequest::onResponseComplete() {
  response_complete_ = true;
  conn_data_.reset();
}

void Router::UpstreamRequest::onUpstreamHostSelected(Upstream::HostDescriptionConstSharedPtr host) {
  ENVOY_LOG(debug, "meta protocol upstream request: selected upstream {}",
            host->address()->asString());
  upstream_host_ = host;
}

void Router::UpstreamRequest::onResetStream(ConnectionPool::PoolFailureReason reason) {
  if (metadata_->getMessageType() == MessageType::Oneway) {
    // For oneway requests, we should not attempt a response. Reset the downstream to signal
    // an error.
    ENVOY_LOG(debug,
              "meta protocol upstream request: the request is oneway, reset downstream stream");
    parent_.callbacks_->resetStream();
    return;
  }

  // When the filter's callback does not end, the sendLocalReply function call
  // triggers the release of the current stream at the end of the filter's callback.
  switch (reason) {
  case ConnectionPool::PoolFailureReason::Overflow:
    parent_.callbacks_->sendLocalReply(
        AppException(Error{ErrorType::Unspecified,
                           fmt::format("meta protocol upstream request: too many connections")}),
        false);
    break;
  case ConnectionPool::PoolFailureReason::LocalConnectionFailure:
    // Should only happen if we closed the connection, due to an error condition, in which case
    // we've already handled any possible downstream response.
    parent_.callbacks_->sendLocalReply(
        AppException(
            Error{ErrorType::Unspecified,
                  fmt::format("meta protocol upstream request: local connection failure '{}'",
                              upstream_host_->address()->asString())}),
        false);
    break;
  case ConnectionPool::PoolFailureReason::RemoteConnectionFailure:
    parent_.callbacks_->sendLocalReply(
        AppException(
            Error{ErrorType::Unspecified,
                  fmt::format("meta protocol upstream request: remote connection failure '{}'",
                              upstream_host_->address()->asString())}),
        false);
    break;
  case ConnectionPool::PoolFailureReason::Timeout:
    parent_.callbacks_->sendLocalReply(
        AppException(Error{
            ErrorType::Unspecified,
            fmt::format("meta protocol upstream request: connection failure '{}' due to timeout",
                        upstream_host_->address()->asString())}),
        false);
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }

  if (parent_.filter_complete_ && !response_complete_) {
    // When the filter's callback has ended and the reply message has not been processed,
    // call resetStream to release the current stream.
    // the resetStream eventually triggers the onDestroy function call.
    parent_.callbacks_->resetStream();
  }
}

} // namespace Router
} // namespace  MetaProtocolProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
