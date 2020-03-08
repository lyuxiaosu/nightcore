#include "gateway/message_connection.h"

#include "utils/uv_utils.h"
#include "gateway/server.h"

#define HLOG(l) LOG(l) << log_header_
#define HVLOG(l) VLOG(l) << log_header_

namespace faas {
namespace gateway {

using protocol::Message;
using protocol::Role;
using protocol::HandshakeMessage;
using protocol::HandshakeResponse;

MessageConnection::MessageConnection(Server* server)
    : Connection(Connection::Type::Message, server), io_worker_(nullptr), state_(kCreated),
      role_(Role::INVALID), func_id_(0), client_id_(0),
      log_header_("MessageConnection[Handshaking]: ") {
}

MessageConnection::~MessageConnection() {
    State state = state_.load();
    CHECK(state == kCreated || state == kClosed);
}

uv_stream_t* MessageConnection::InitUVHandle(uv_loop_t* uv_loop) {
    write_message_event_.data = this;
    UV_CHECK_OK(uv_async_init(uv_loop, &write_message_event_,
                              &MessageConnection::NewMessageForWriteCallback));
    UV_CHECK_OK(uv_pipe_init(uv_loop, &uv_pipe_handle_, 0));
    return reinterpret_cast<uv_stream_t*>(&uv_pipe_handle_);
}

void MessageConnection::Start(IOWorker* io_worker) {
    CHECK(state_.load() == kCreated);
    CHECK_IN_EVENT_LOOP_THREAD(uv_pipe_handle_.loop);
    io_worker_ = io_worker;
    uv_pipe_handle_.data = this;
    UV_CHECK_OK(uv_read_start(reinterpret_cast<uv_stream_t*>(&uv_pipe_handle_),
                              &MessageConnection::BufferAllocCallback,
                              &MessageConnection::ReadHandshakeCallback));
    state_.store(kHandshake);
}

void MessageConnection::ScheduleClose() {
    CHECK_IN_EVENT_LOOP_THREAD(uv_pipe_handle_.loop);
    State state = state_.load();
    if (state == kClosing) {
        HLOG(INFO) << "Already scheduled for closing";
        return;
    }
    CHECK(state == kHandshake || state == kRunning);
    closed_uv_handles_ = 0;
    uv_handles_is_closing_ = 2;
    uv_close(reinterpret_cast<uv_handle_t*>(&uv_pipe_handle_),
             &MessageConnection::CloseCallback);
    absl::MutexLock lk(&write_message_mu_);
    uv_close(reinterpret_cast<uv_handle_t*>(&write_message_event_),
             &MessageConnection::CloseCallback);
    state_.store(kClosing);
}

void MessageConnection::RecvHandshakeMessage() {
    CHECK_IN_EVENT_LOOP_THREAD(uv_pipe_handle_.loop);
    UV_CHECK_OK(uv_read_stop(reinterpret_cast<uv_stream_t*>(&uv_pipe_handle_)));
    HandshakeMessage* message = reinterpret_cast<HandshakeMessage*>(
        message_buffer_.data());
    server_->OnNewHandshake(this, *message, &handshake_response_);
    role_ = static_cast<Role>(message->role);
    func_id_ = message->func_id;
    client_id_ = handshake_response_.client_id;
    if (role_ == Role::WATCHDOG) {
        log_header_ = absl::StrFormat("WatchdogConnection[%d]: ", static_cast<int>(func_id_));
    } else if (role_ == Role::FUNC_WORKER) {
        log_header_ = absl::StrFormat("FuncWorkerConnection[%d-%d]: ",
                                      static_cast<int>(func_id_), static_cast<int>(client_id_));
    }
    uv_buf_t buf = {
        .base = reinterpret_cast<char*>(&handshake_response_),
        .len = sizeof(HandshakeResponse)
    };
    UV_CHECK_OK(uv_write(write_req_pool_.Get(), reinterpret_cast<uv_stream_t*>(&uv_pipe_handle_),
                         &buf, 1, &MessageConnection::WriteHandshakeResponseCallback));
    state_.store(kRunning);
}

void MessageConnection::WriteMessage(const Message& message) {
    absl::MutexLock lk(&write_message_mu_);
    if (state_.load() != kRunning) {
        HLOG(WARNING) << "MessageConnection is not in running state, cannot write message";
        return;
    }
    pending_messages_.push_back(message);
    UV_CHECK_OK(uv_async_send(&write_message_event_));
}

UV_ALLOC_CB_FOR_CLASS(MessageConnection, BufferAlloc) {
    io_worker_->NewReadBuffer(suggested_size, buf);
}

UV_READ_CB_FOR_CLASS(MessageConnection, ReadHandshake) {
    if (nread < 0) {
        HLOG(WARNING) << "Read error on handshake, will close this connection: "
                      << uv_strerror(nread);
        ScheduleClose();
    } else if (nread > 0) {
        message_buffer_.AppendData(buf->base, nread);
        CHECK_LE(message_buffer_.length(), sizeof(HandshakeMessage));
        if (message_buffer_.length() == sizeof(HandshakeMessage)) {
            RecvHandshakeMessage();
        }
    }
    if (buf->base != 0) {
        io_worker_->ReturnReadBuffer(buf);
    }
}

UV_WRITE_CB_FOR_CLASS(MessageConnection, WriteHandshakeResponse) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to write handshake response, will close this connection: "
                      << uv_strerror(status);
        ScheduleClose();
        return;
    }
    HLOG(INFO) << "Handshake done";
    message_buffer_.Reset();
    UV_CHECK_OK(uv_read_start(reinterpret_cast<uv_stream_t*>(&uv_pipe_handle_),
                              &MessageConnection::BufferAllocCallback,
                              &MessageConnection::ReadMessageCallback));
}

UV_READ_CB_FOR_CLASS(MessageConnection, ReadMessage) {
    if (nread < 0) {
        HLOG(WARNING) << "Read error, will close this connection: "
                      << uv_strerror(nread);
        ScheduleClose();
    } else if (nread > 0) {
        utils::ReadMessages<Message>(
            &message_buffer_, buf->base, nread,
            [this] (Message* message) {
                server_->OnRecvMessage(this, *message);
            });
    }
    if (buf->base != 0) {
        io_worker_->ReturnReadBuffer(buf);
    }
}

UV_WRITE_CB_FOR_CLASS(MessageConnection, WriteMessage) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to write response, will close this connection: "
                      << uv_strerror(status);
        ScheduleClose();
        return;
    }
    io_worker_->ReturnWriteBuffer(reinterpret_cast<char*>(req->data));
    write_req_pool_.Return(req);
}

UV_ASYNC_CB_FOR_CLASS(MessageConnection, NewMessageForWrite) {
    if (state_.load() != kRunning) {
        HLOG(WARNING) << "MessageConnection is closing or has closed, will not write pending messages";
        return;
    }
    size_t write_size = 0;
    char* data = nullptr;
    {
        absl::MutexLock lk(&write_message_mu_);
        write_size = pending_messages_.size() * sizeof(Message);
        if (write_size > 0) {
            data = reinterpret_cast<char*>(malloc(write_size));
            memcpy(data, pending_messages_.data(), write_size);
            pending_messages_.clear();
        }
    }
    if (write_size == 0) {
        return;
    }
    char* ptr = data;
    while (write_size > 0) {
        uv_buf_t buf;
        io_worker_->NewWriteBuffer(&buf);
        size_t copy_size = std::min(buf.len, write_size);
        memcpy(buf.base, ptr, copy_size);
        buf.len = write_size;
        uv_write_t* write_req = write_req_pool_.Get();
        write_req->data = buf.base;
        UV_CHECK_OK(uv_write(write_req, reinterpret_cast<uv_stream_t*>(&uv_pipe_handle_),
                             &buf, 1, &MessageConnection::WriteMessageCallback));
        write_size -= copy_size;
        ptr += copy_size;
    }
    free(data);
}

UV_CLOSE_CB_FOR_CLASS(MessageConnection, Close) {
    closed_uv_handles_++;
    if (closed_uv_handles_ == uv_handles_is_closing_) {
        state_.store(kClosed);
        io_worker_->OnConnectionClose(this);
    }
}

}  // namespace gateway
}  // namespace faas