//
// Copyright 2018 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/subchannel_stream_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <cstdint>
#include <string>
#include <utility>

#include <grpc/slice_buffer.h>
#include <grpc/status.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/lib/gpr/time_precise.h"
#include "src/core/lib/gprpp/debug_location.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/resource_quota/api.h"
#include "src/core/lib/resource_quota/resource_quota.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/error_utils.h"

#define SUBCHANNEL_STREAM_INITIAL_CONNECT_BACKOFF_SECONDS 1
#define SUBCHANNEL_STREAM_RECONNECT_BACKOFF_MULTIPLIER 1.6
#define SUBCHANNEL_STREAM_RECONNECT_MAX_BACKOFF_SECONDS 120
#define SUBCHANNEL_STREAM_RECONNECT_JITTER 0.2

namespace grpc_core {

//
// SubchannelStreamClient
//

SubchannelStreamClient::SubchannelStreamClient(
    RefCountedPtr<ConnectedSubchannel> connected_subchannel,
    grpc_pollset_set* interested_parties,
    std::unique_ptr<CallEventHandler> event_handler, const char* tracer)
    : InternallyRefCounted<SubchannelStreamClient>(tracer),
      connected_subchannel_(std::move(connected_subchannel)),
      interested_parties_(interested_parties),
      tracer_(tracer),
      call_allocator_(
          ResourceQuotaFromChannelArgs(connected_subchannel_->args())
              ->memory_quota()
              ->CreateMemoryAllocator(
                  (tracer != nullptr) ? tracer : "SubchannelStreamClient")),
      event_handler_(std::move(event_handler)),
      retry_backoff_(
          BackOff::Options()
              .set_initial_backoff(Duration::Seconds(
                  SUBCHANNEL_STREAM_INITIAL_CONNECT_BACKOFF_SECONDS))
              .set_multiplier(SUBCHANNEL_STREAM_RECONNECT_BACKOFF_MULTIPLIER)
              .set_jitter(SUBCHANNEL_STREAM_RECONNECT_JITTER)
              .set_max_backoff(Duration::Seconds(
                  SUBCHANNEL_STREAM_RECONNECT_MAX_BACKOFF_SECONDS))) {
  if (GPR_UNLIKELY(tracer_ != nullptr)) {
    gpr_log(GPR_INFO, "%s %p: created SubchannelStreamClient", tracer_, this);
  }
  GRPC_CLOSURE_INIT(&retry_timer_callback_, OnRetryTimer, this,
                    grpc_schedule_on_exec_ctx);
  StartCall();
}

SubchannelStreamClient::~SubchannelStreamClient() {
  if (GPR_UNLIKELY(tracer_ != nullptr)) {
    gpr_log(GPR_INFO, "%s %p: destroying SubchannelStreamClient", tracer_,
            this);
  }
}

void SubchannelStreamClient::Orphan() {
  if (GPR_UNLIKELY(tracer_ != nullptr)) {
    gpr_log(GPR_INFO, "%s %p: SubchannelStreamClient shutting down", tracer_,
            this);
  }
  {
    MutexLock lock(&mu_);
    event_handler_.reset();
    call_state_.reset();
    if (retry_timer_callback_pending_) {
      grpc_timer_cancel(&retry_timer_);
    }
  }
  Unref(DEBUG_LOCATION, "orphan");
}

void SubchannelStreamClient::StartCall() {
  MutexLock lock(&mu_);
  StartCallLocked();
}

void SubchannelStreamClient::StartCallLocked() {
  if (event_handler_ == nullptr) return;
  GPR_ASSERT(call_state_ == nullptr);
  if (event_handler_ != nullptr) {
    event_handler_->OnCallStartLocked(this);
  }
  call_state_ = MakeOrphanable<CallState>(Ref(), interested_parties_);
  if (GPR_UNLIKELY(tracer_ != nullptr)) {
    gpr_log(GPR_INFO, "%s %p: SubchannelStreamClient created CallState %p",
            tracer_, this, call_state_.get());
  }
  call_state_->StartCallLocked();
}

void SubchannelStreamClient::StartRetryTimerLocked() {
  if (event_handler_ != nullptr) {
    event_handler_->OnRetryTimerStartLocked(this);
  }
  Timestamp next_try = retry_backoff_.NextAttemptTime();
  if (GPR_UNLIKELY(tracer_ != nullptr)) {
    gpr_log(GPR_INFO, "%s %p: SubchannelStreamClient health check call lost...",
            tracer_, this);
    Duration timeout = next_try - ExecCtx::Get()->Now();
    if (timeout > Duration::Zero()) {
      gpr_log(GPR_INFO, "%s %p: ... will retry in %" PRId64 "ms.", tracer_,
              this, timeout.millis());
    } else {
      gpr_log(GPR_INFO, "%s %p: ... retrying immediately.", tracer_, this);
    }
  }
  // Ref for callback, tracked manually.
  Ref(DEBUG_LOCATION, "health_retry_timer").release();
  retry_timer_callback_pending_ = true;
  grpc_timer_init(&retry_timer_, next_try, &retry_timer_callback_);
}

void SubchannelStreamClient::OnRetryTimer(void* arg, grpc_error_handle error) {
  auto* self = static_cast<SubchannelStreamClient*>(arg);
  {
    MutexLock lock(&self->mu_);
    self->retry_timer_callback_pending_ = false;
    if (self->event_handler_ != nullptr && error == GRPC_ERROR_NONE &&
        self->call_state_ == nullptr) {
      if (GPR_UNLIKELY(self->tracer_ != nullptr)) {
        gpr_log(GPR_INFO,
                "%s %p: SubchannelStreamClient restarting health check call",
                self->tracer_, self);
      }
      self->StartCallLocked();
    }
  }
  self->Unref(DEBUG_LOCATION, "health_retry_timer");
}

//
// SubchannelStreamClient::CallState
//

SubchannelStreamClient::CallState::CallState(
    RefCountedPtr<SubchannelStreamClient> health_check_client,
    grpc_pollset_set* interested_parties)
    : subchannel_stream_client_(std::move(health_check_client)),
      pollent_(grpc_polling_entity_create_from_pollset_set(interested_parties)),
      arena_(Arena::Create(subchannel_stream_client_->connected_subchannel_
                               ->GetInitialCallSizeEstimate(),
                           &subchannel_stream_client_->call_allocator_)),
      payload_(context_),
      send_initial_metadata_(arena_.get()),
      send_trailing_metadata_(arena_.get()),
      recv_initial_metadata_(arena_.get()),
      recv_trailing_metadata_(arena_.get()) {}

SubchannelStreamClient::CallState::~CallState() {
  if (GPR_UNLIKELY(subchannel_stream_client_->tracer_ != nullptr)) {
    gpr_log(GPR_INFO, "%s %p: SubchannelStreamClient destroying CallState %p",
            subchannel_stream_client_->tracer_, subchannel_stream_client_.get(),
            this);
  }
  for (size_t i = 0; i < GRPC_CONTEXT_COUNT; ++i) {
    if (context_[i].destroy != nullptr) {
      context_[i].destroy(context_[i].value);
    }
  }
  // Unset the call combiner cancellation closure.  This has the
  // effect of scheduling the previously set cancellation closure, if
  // any, so that it can release any internal references it may be
  // holding to the call stack.
  call_combiner_.SetNotifyOnCancel(nullptr);
}

void SubchannelStreamClient::CallState::Orphan() {
  call_combiner_.Cancel(GRPC_ERROR_CANCELLED);
  Cancel();
}

void SubchannelStreamClient::CallState::StartCallLocked() {
  SubchannelCall::Args args = {
      subchannel_stream_client_->connected_subchannel_,
      &pollent_,
      Slice::FromStaticString("/grpc.health.v1.Health/Watch"),
      gpr_get_cycle_counter(),  // start_time
      Timestamp::InfFuture(),   // deadline
      arena_.get(),
      context_,
      &call_combiner_,
  };
  grpc_error_handle error = GRPC_ERROR_NONE;
  call_ = SubchannelCall::Create(std::move(args), &error).release();
  // Register after-destruction callback.
  GRPC_CLOSURE_INIT(&after_call_stack_destruction_, AfterCallStackDestruction,
                    this, grpc_schedule_on_exec_ctx);
  call_->SetAfterCallStackDestroy(&after_call_stack_destruction_);
  // Check if creation failed.
  if (error != GRPC_ERROR_NONE ||
      subchannel_stream_client_->event_handler_ == nullptr) {
    gpr_log(GPR_ERROR,
            "SubchannelStreamClient %p CallState %p: error creating "
            "stream on subchannel (%s); will retry",
            subchannel_stream_client_.get(), this,
            grpc_error_std_string(error).c_str());
    GRPC_ERROR_UNREF(error);
    CallEndedLocked(/*retry=*/true);
    return;
  }
  // Initialize payload and batch.
  payload_.context = context_;
  batch_.payload = &payload_;
  // on_complete callback takes ref, handled manually.
  call_->Ref(DEBUG_LOCATION, "on_complete").release();
  batch_.on_complete = GRPC_CLOSURE_INIT(&on_complete_, OnComplete, this,
                                         grpc_schedule_on_exec_ctx);
  // Add send_initial_metadata op.
  send_initial_metadata_.Set(
      HttpPathMetadata(),
      subchannel_stream_client_->event_handler_->GetPathLocked());
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  payload_.send_initial_metadata.send_initial_metadata =
      &send_initial_metadata_;
  payload_.send_initial_metadata.send_initial_metadata_flags = 0;
  payload_.send_initial_metadata.peer_string = nullptr;
  batch_.send_initial_metadata = true;
  // Add send_message op.
  grpc_slice request_slice =
      subchannel_stream_client_->event_handler_->EncodeSendMessageLocked();
  grpc_slice_buffer slice_buffer;
  grpc_slice_buffer_init(&slice_buffer);
  grpc_slice_buffer_add(&slice_buffer, request_slice);
  send_message_.emplace(&slice_buffer, 0);
  grpc_slice_buffer_destroy_internal(&slice_buffer);
  payload_.send_message.send_message.reset(&*send_message_);
  batch_.send_message = true;
  // Add send_trailing_metadata op.
  payload_.send_trailing_metadata.send_trailing_metadata =
      &send_trailing_metadata_;
  batch_.send_trailing_metadata = true;
  // Add recv_initial_metadata op.
  payload_.recv_initial_metadata.recv_initial_metadata =
      &recv_initial_metadata_;
  payload_.recv_initial_metadata.recv_flags = nullptr;
  payload_.recv_initial_metadata.trailing_metadata_available = nullptr;
  payload_.recv_initial_metadata.peer_string = nullptr;
  // recv_initial_metadata_ready callback takes ref, handled manually.
  call_->Ref(DEBUG_LOCATION, "recv_initial_metadata_ready").release();
  payload_.recv_initial_metadata.recv_initial_metadata_ready =
      GRPC_CLOSURE_INIT(&recv_initial_metadata_ready_, RecvInitialMetadataReady,
                        this, grpc_schedule_on_exec_ctx);
  batch_.recv_initial_metadata = true;
  // Add recv_message op.
  payload_.recv_message.recv_message = &recv_message_;
  payload_.recv_message.call_failed_before_recv_message = nullptr;
  // recv_message callback takes ref, handled manually.
  call_->Ref(DEBUG_LOCATION, "recv_message_ready").release();
  payload_.recv_message.recv_message_ready = GRPC_CLOSURE_INIT(
      &recv_message_ready_, RecvMessageReady, this, grpc_schedule_on_exec_ctx);
  batch_.recv_message = true;
  // Start batch.
  StartBatch(&batch_);
  // Initialize recv_trailing_metadata batch.
  recv_trailing_metadata_batch_.payload = &payload_;
  // Add recv_trailing_metadata op.
  payload_.recv_trailing_metadata.recv_trailing_metadata =
      &recv_trailing_metadata_;
  payload_.recv_trailing_metadata.collect_stats = &collect_stats_;
  // This callback signals the end of the call, so it relies on the
  // initial ref instead of taking a new ref.  When it's invoked, the
  // initial ref is released.
  payload_.recv_trailing_metadata.recv_trailing_metadata_ready =
      GRPC_CLOSURE_INIT(&recv_trailing_metadata_ready_,
                        RecvTrailingMetadataReady, this,
                        grpc_schedule_on_exec_ctx);
  recv_trailing_metadata_batch_.recv_trailing_metadata = true;
  // Start recv_trailing_metadata batch.
  StartBatch(&recv_trailing_metadata_batch_);
}

void SubchannelStreamClient::CallState::StartBatchInCallCombiner(
    void* arg, grpc_error_handle /*error*/) {
  auto* batch = static_cast<grpc_transport_stream_op_batch*>(arg);
  auto* call = static_cast<SubchannelCall*>(batch->handler_private.extra_arg);
  call->StartTransportStreamOpBatch(batch);
}

void SubchannelStreamClient::CallState::StartBatch(
    grpc_transport_stream_op_batch* batch) {
  batch->handler_private.extra_arg = call_;
  GRPC_CLOSURE_INIT(&batch->handler_private.closure, StartBatchInCallCombiner,
                    batch, grpc_schedule_on_exec_ctx);
  GRPC_CALL_COMBINER_START(&call_combiner_, &batch->handler_private.closure,
                           GRPC_ERROR_NONE, "start_subchannel_batch");
}

void SubchannelStreamClient::CallState::AfterCallStackDestruction(
    void* arg, grpc_error_handle /*error*/) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  delete self;
}

void SubchannelStreamClient::CallState::OnCancelComplete(
    void* arg, grpc_error_handle /*error*/) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  GRPC_CALL_COMBINER_STOP(&self->call_combiner_, "health_cancel");
  self->call_->Unref(DEBUG_LOCATION, "cancel");
}

void SubchannelStreamClient::CallState::StartCancel(
    void* arg, grpc_error_handle /*error*/) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  auto* batch = grpc_make_transport_stream_op(
      GRPC_CLOSURE_CREATE(OnCancelComplete, self, grpc_schedule_on_exec_ctx));
  batch->cancel_stream = true;
  batch->payload->cancel_stream.cancel_error = GRPC_ERROR_CANCELLED;
  self->call_->StartTransportStreamOpBatch(batch);
}

void SubchannelStreamClient::CallState::Cancel() {
  bool expected = false;
  if (cancelled_.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
    call_->Ref(DEBUG_LOCATION, "cancel").release();
    GRPC_CALL_COMBINER_START(
        &call_combiner_,
        GRPC_CLOSURE_CREATE(StartCancel, this, grpc_schedule_on_exec_ctx),
        GRPC_ERROR_NONE, "health_cancel");
  }
}

void SubchannelStreamClient::CallState::OnComplete(
    void* arg, grpc_error_handle /*error*/) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  GRPC_CALL_COMBINER_STOP(&self->call_combiner_, "on_complete");
  self->send_initial_metadata_.Clear();
  self->send_trailing_metadata_.Clear();
  self->call_->Unref(DEBUG_LOCATION, "on_complete");
}

void SubchannelStreamClient::CallState::RecvInitialMetadataReady(
    void* arg, grpc_error_handle /*error*/) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  GRPC_CALL_COMBINER_STOP(&self->call_combiner_, "recv_initial_metadata_ready");
  self->recv_initial_metadata_.Clear();
  self->call_->Unref(DEBUG_LOCATION, "recv_initial_metadata_ready");
}

void SubchannelStreamClient::CallState::DoneReadingRecvMessage(
    grpc_error_handle error) {
  recv_message_.reset();
  if (error != GRPC_ERROR_NONE) {
    GRPC_ERROR_UNREF(error);
    Cancel();
    grpc_slice_buffer_destroy_internal(&recv_message_buffer_);
    call_->Unref(DEBUG_LOCATION, "recv_message_ready");
    return;
  }
  // Concatenate the slices to form a single string.
  std::unique_ptr<uint8_t> recv_message_deleter;
  uint8_t* recv_message;
  if (recv_message_buffer_.count == 1) {
    recv_message = GRPC_SLICE_START_PTR(recv_message_buffer_.slices[0]);
  } else {
    recv_message =
        static_cast<uint8_t*>(gpr_malloc(recv_message_buffer_.length));
    recv_message_deleter.reset(recv_message);
    size_t offset = 0;
    for (size_t i = 0; i < recv_message_buffer_.count; ++i) {
      memcpy(recv_message + offset,
             GRPC_SLICE_START_PTR(recv_message_buffer_.slices[i]),
             GRPC_SLICE_LENGTH(recv_message_buffer_.slices[i]));
      offset += GRPC_SLICE_LENGTH(recv_message_buffer_.slices[i]);
    }
  }
  // Report payload.
  {
    MutexLock lock(&subchannel_stream_client_->mu_);
    if (subchannel_stream_client_->event_handler_ != nullptr) {
      absl::string_view serialized_message(
          reinterpret_cast<char*>(recv_message), recv_message_buffer_.length);
      absl::Status status =
          subchannel_stream_client_->event_handler_->RecvMessageReadyLocked(
              subchannel_stream_client_.get(), serialized_message);
      if (!status.ok()) {
        if (GPR_UNLIKELY(subchannel_stream_client_->tracer_ != nullptr)) {
          gpr_log(GPR_INFO,
                  "%s %p: SubchannelStreamClient CallState %p: failed to "
                  "parse response message: %s",
                  subchannel_stream_client_->tracer_,
                  subchannel_stream_client_.get(), this,
                  status.ToString().c_str());
        }
        Cancel();
      }
    }
  }
  seen_response_.store(true, std::memory_order_release);
  grpc_slice_buffer_destroy_internal(&recv_message_buffer_);
  // Start another recv_message batch.
  // This re-uses the ref we're holding.
  // Note: Can't just reuse batch_ here, since we don't know that all
  // callbacks from the original batch have completed yet.
  recv_message_batch_.payload = &payload_;
  payload_.recv_message.recv_message = &recv_message_;
  payload_.recv_message.call_failed_before_recv_message = nullptr;
  payload_.recv_message.recv_message_ready = GRPC_CLOSURE_INIT(
      &recv_message_ready_, RecvMessageReady, this, grpc_schedule_on_exec_ctx);
  recv_message_batch_.recv_message = true;
  StartBatch(&recv_message_batch_);
}

grpc_error_handle
SubchannelStreamClient::CallState::PullSliceFromRecvMessage() {
  grpc_slice slice;
  grpc_error_handle error = recv_message_->Pull(&slice);
  if (error == GRPC_ERROR_NONE) {
    grpc_slice_buffer_add(&recv_message_buffer_, slice);
  }
  return error;
}

void SubchannelStreamClient::CallState::ContinueReadingRecvMessage() {
  while (recv_message_->Next(SIZE_MAX, &recv_message_ready_)) {
    grpc_error_handle error = PullSliceFromRecvMessage();
    if (error != GRPC_ERROR_NONE) {
      DoneReadingRecvMessage(error);
      return;
    }
    if (recv_message_buffer_.length == recv_message_->length()) {
      DoneReadingRecvMessage(GRPC_ERROR_NONE);
      break;
    }
  }
}

void SubchannelStreamClient::CallState::OnByteStreamNext(
    void* arg, grpc_error_handle error) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  if (error != GRPC_ERROR_NONE) {
    self->DoneReadingRecvMessage(GRPC_ERROR_REF(error));
    return;
  }
  error = self->PullSliceFromRecvMessage();
  if (error != GRPC_ERROR_NONE) {
    self->DoneReadingRecvMessage(error);
    return;
  }
  if (self->recv_message_buffer_.length == self->recv_message_->length()) {
    self->DoneReadingRecvMessage(GRPC_ERROR_NONE);
  } else {
    self->ContinueReadingRecvMessage();
  }
}

void SubchannelStreamClient::CallState::RecvMessageReady(
    void* arg, grpc_error_handle /*error*/) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  GRPC_CALL_COMBINER_STOP(&self->call_combiner_, "recv_message_ready");
  if (self->recv_message_ == nullptr) {
    self->call_->Unref(DEBUG_LOCATION, "recv_message_ready");
    return;
  }
  grpc_slice_buffer_init(&self->recv_message_buffer_);
  GRPC_CLOSURE_INIT(&self->recv_message_ready_, OnByteStreamNext, self,
                    grpc_schedule_on_exec_ctx);
  self->ContinueReadingRecvMessage();
  // Ref will continue to be held until we finish draining the byte stream.
}

void SubchannelStreamClient::CallState::RecvTrailingMetadataReady(
    void* arg, grpc_error_handle error) {
  auto* self = static_cast<SubchannelStreamClient::CallState*>(arg);
  GRPC_CALL_COMBINER_STOP(&self->call_combiner_,
                          "recv_trailing_metadata_ready");
  // Get call status.
  grpc_status_code status =
      self->recv_trailing_metadata_.get(GrpcStatusMetadata())
          .value_or(GRPC_STATUS_UNKNOWN);
  if (error != GRPC_ERROR_NONE) {
    grpc_error_get_status(error, Timestamp::InfFuture(), &status,
                          nullptr /* slice */, nullptr /* http_error */,
                          nullptr /* error_string */);
  }
  if (GPR_UNLIKELY(self->subchannel_stream_client_->tracer_ != nullptr)) {
    gpr_log(GPR_INFO,
            "%s %p: SubchannelStreamClient CallState %p: health watch failed "
            "with status %d",
            self->subchannel_stream_client_->tracer_,
            self->subchannel_stream_client_.get(), self, status);
  }
  // Clean up.
  self->recv_trailing_metadata_.Clear();
  // Report call end.
  MutexLock lock(&self->subchannel_stream_client_->mu_);
  if (self->subchannel_stream_client_->event_handler_ != nullptr) {
    self->subchannel_stream_client_->event_handler_
        ->RecvTrailingMetadataReadyLocked(self->subchannel_stream_client_.get(),
                                          status);
  }
  // For status UNIMPLEMENTED, give up and assume always healthy.
  self->CallEndedLocked(/*retry=*/status != GRPC_STATUS_UNIMPLEMENTED);
}

void SubchannelStreamClient::CallState::CallEndedLocked(bool retry) {
  // If this CallState is still in use, this call ended because of a failure,
  // so we need to stop using it and optionally create a new one.
  // Otherwise, we have deliberately ended this call, and no further action
  // is required.
  if (this == subchannel_stream_client_->call_state_.get()) {
    subchannel_stream_client_->call_state_.reset();
    if (retry) {
      GPR_ASSERT(subchannel_stream_client_->event_handler_ != nullptr);
      if (seen_response_.load(std::memory_order_acquire)) {
        // If the call fails after we've gotten a successful response, reset
        // the backoff and restart the call immediately.
        subchannel_stream_client_->retry_backoff_.Reset();
        subchannel_stream_client_->StartCallLocked();
      } else {
        // If the call failed without receiving any messages, retry later.
        subchannel_stream_client_->StartRetryTimerLocked();
      }
    }
  }
  // When the last ref to the call stack goes away, the CallState object
  // will be automatically destroyed.
  call_->Unref(DEBUG_LOCATION, "call_ended");
}

}  // namespace grpc_core
