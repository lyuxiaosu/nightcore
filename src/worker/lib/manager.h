#pragma once

#include "base/common.h"
#include "common/func_config.h"
#include "common/protocol.h"
#include "utils/appendable_buffer.h"
#include "utils/shared_memory.h"

namespace faas {
namespace worker_lib {

class Manager {
public:
    Manager();
    ~Manager();

    // All callbacks have to be set before calling Start()
    void Start();

    void OnGatewayIOError(int errnum);
    void OnGatewayIOError(std::string_view message);
    void OnWatchdogIOError(int errnum);
    void OnWatchdogIOError(std::string_view message);

    int watchdog_input_pipe_fd() const { return watchdog_input_pipe_fd_; }
    int watchdog_output_pipe_fd() const { return watchdog_output_pipe_fd_; }
    std::string_view gateway_ipc_path() const { return gateway_ipc_path_; }

    typedef std::function<void(std::span<const char> /* data */)> SendDataCallback;
    typedef std::function<void(uint32_t /* handle */, std::span<const char> /* input */)>
            IncomingFuncCallCallback;
    typedef std::function<void(uint32_t /* handle */, bool /* success */,
                               std::span<const char> /* output */)>
            OutcomingFuncCallCompleteCallback;

    void SetSendGatewayDataCallback(SendDataCallback callback) {
        send_gateway_data_callback_set_ = true;
        send_gateway_data_callback_ = callback;
    }
    void SetSendWatchdogDataCallback(SendDataCallback callback) {
        send_watchdog_data_callback_set_ = true;
        send_watchdog_data_callback_ = callback;
    }
    void SetIncomingFuncCallCallback(IncomingFuncCallCallback callback) {
        incoming_func_call_callback_set_ = true;
        incoming_func_call_callback_ = callback;
    }
    void SetOutcomingFuncCallCompleteCallback(OutcomingFuncCallCompleteCallback callback) {
        outcoming_func_call_complete_callback_set_ = true;
        outcoming_func_call_complete_callback_ = callback;
    }

    void OnRecvGatewayData(std::span<const char> data);
    void OnRecvWatchdogData(std::span<const char> data);
    bool OnOutcomingFuncCall(std::string_view func_name, std::span<const char> input, uint32_t* handle);
    void OnIncomingFuncCallComplete(uint32_t handle, bool success, std::span<const char> output);

private:
    bool started_;
    FuncConfig func_config_;
    int func_id_;
    int client_id_;
    int watchdog_input_pipe_fd_;
    int watchdog_output_pipe_fd_;
    std::string gateway_ipc_path_;
    utils::SharedMemory shared_memory_;
    uint32_t next_handle_value_;

    bool send_gateway_data_callback_set_;
    SendDataCallback send_gateway_data_callback_;
    bool send_watchdog_data_callback_set_;
    SendDataCallback send_watchdog_data_callback_;
    bool incoming_func_call_callback_set_;
    IncomingFuncCallCallback incoming_func_call_callback_;
    bool outcoming_func_call_complete_callback_set_;
    OutcomingFuncCallCompleteCallback outcoming_func_call_complete_callback_;

    utils::AppendableBuffer gateway_recv_buffer_;
    utils::AppendableBuffer watchdog_recv_buffer_;

    struct OutcomingFuncCallContext {
        protocol::FuncCall func_call;
        utils::SharedMemory::Region* input_region;
        utils::SharedMemory::Region* output_region;
    };
    std::unordered_map<uint32_t, std::unique_ptr<OutcomingFuncCallContext>>
        outcoming_func_calls_;

    struct IncomingFuncCallContext {
        protocol::FuncCall func_call;
        uint64_t start_timestamp;
    };
    std::unordered_map<uint32_t, std::unique_ptr<IncomingFuncCallContext>>
        incoming_func_calls_;

    stat::StatisticsCollector<uint32_t> processing_delay_stat_;

    void OnRecvGatewayMessage(const protocol::Message& message);
    void OnRecvWatchdogMessage(const protocol::Message& message);
    void OnOutcomingFuncCallComplete(protocol::FuncCall func_call, bool success);
    void OnIncomingFuncCall(protocol::FuncCall func_call);

    DISALLOW_COPY_AND_ASSIGN(Manager);
};

}  // namespace worker_lib
}  // namespace faas