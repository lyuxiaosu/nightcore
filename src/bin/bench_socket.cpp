#include "base/init.h"
#include "base/common.h"

#include "common/time.h"
#include "common/stat.h"
#include "utils/io.h"
#include "utils/socket.h"

#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h> 

#include <absl/flags/flag.h>

ABSL_FLAG(std::string, socket_type, "unix", "tcp, unix, or pipe");
ABSL_FLAG(size_t, payload_bytesize, 16, "Byte size of each payload");
ABSL_FLAG(int, tcp_port, 32767, "Port for TCP socket type");
ABSL_FLAG(int, server_cpu, -1, "Bind server process to CPU");
ABSL_FLAG(int, client_cpu, -1, "Bind server process to CPU");
ABSL_FLAG(absl::Duration, duration, absl::Seconds(30), "Duration to run");
ABSL_FLAG(absl::Duration, stat_duration, absl::Seconds(10),
          "Duration for reporting statistics");

void BindToCpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    PCHECK(sched_setaffinity(0, sizeof(set), &set) == 0);
}

void Server(int infd, int outfd, size_t payload_bytesize,
            absl::Duration duration, absl::Duration stat_duration, int cpu) {
    faas::stat::StatisticsCollector<int32_t> msg_delay_stat(
        faas::stat::StatisticsCollector<int32_t>::StandardReportCallback("client_msg_delay"));
    faas::stat::Counter msg_counter(
        faas::stat::Counter::StandardReportCallback("client_msg_counter"));
    msg_delay_stat.set_report_interval_in_ms(
        gsl::narrow_cast<uint32_t>(absl::ToInt64Milliseconds(stat_duration)));
    msg_counter.set_report_interval_in_ms(
        gsl::narrow_cast<uint32_t>(absl::ToInt64Milliseconds(stat_duration)));
    if (cpu != -1) {
        BindToCpu(cpu);
    }
    int64_t start_timestamp = faas::GetMonotonicNanoTimestamp();
    int64_t stop_timestamp = start_timestamp + absl::ToInt64Nanoseconds(duration);
    char* payload_buffer = new char[payload_bytesize];
    while (true) {
        int64_t current_timestamp = faas::GetMonotonicNanoTimestamp();
        if (current_timestamp >= stop_timestamp) {
            current_timestamp = -1;
        }
        memcpy(payload_buffer, &current_timestamp, sizeof(int64_t));
        CHECK(faas::io_utils::SendData(outfd, payload_buffer, payload_bytesize));
        if (current_timestamp == -1) {
            break;
        }
        bool eof = false;
        CHECK(faas::io_utils::RecvData(infd, payload_buffer, payload_bytesize, &eof));
        msg_counter.Tick();
        current_timestamp = faas::GetMonotonicNanoTimestamp();
        int64_t send_timestamp;
        memcpy(&send_timestamp, payload_buffer, sizeof(int64_t));
        msg_delay_stat.AddSample(gsl::narrow_cast<int32_t>(current_timestamp - send_timestamp));
    }
    delete[] payload_buffer;
    LOG(INFO) << "Close server socket";
        PCHECK(close(infd) == 0);
    if (outfd != infd) {
        PCHECK(close(outfd) == 0);
    }
}

void Client(int infd, int outfd, size_t payload_bytesize, absl::Duration stat_duration, int cpu) {
    faas::stat::StatisticsCollector<int32_t> msg_delay_stat(
        faas::stat::StatisticsCollector<int32_t>::StandardReportCallback("server_msg_delay"));
    faas::stat::Counter msg_counter(
        faas::stat::Counter::StandardReportCallback("server_msg_counter"));
    msg_delay_stat.set_report_interval_in_ms(
        gsl::narrow_cast<uint32_t>(absl::ToInt64Milliseconds(stat_duration)));
    msg_counter.set_report_interval_in_ms(
        gsl::narrow_cast<uint32_t>(absl::ToInt64Milliseconds(stat_duration)));
    if (cpu != -1) {
        BindToCpu(cpu);
    }
    char* payload_buffer = new char[payload_bytesize];
    while (true) {
        bool eof = false;
        CHECK(faas::io_utils::RecvData(infd, payload_buffer, payload_bytesize, &eof));
        msg_counter.Tick();
        int64_t current_timestamp = faas::GetMonotonicNanoTimestamp();
        int64_t send_timestamp;
        memcpy(&send_timestamp, payload_buffer, sizeof(int64_t));
        if (send_timestamp == -1) {
            LOG(INFO) << "Server socket closed";
            break;
        }
        msg_delay_stat.AddSample(gsl::narrow_cast<int32_t>(current_timestamp - send_timestamp));
        current_timestamp = faas::GetMonotonicNanoTimestamp();
        memcpy(payload_buffer, &current_timestamp, sizeof(int64_t));
        CHECK(faas::io_utils::SendData(outfd, payload_buffer, payload_bytesize));
    }
    delete[] payload_buffer;
    PCHECK(close(infd) == 0);
    if (outfd != infd) {
        PCHECK(close(outfd) == 0);
    }
}

int main(int argc, char* argv[]) {
    faas::base::InitMain(argc, argv);

    int payload_bytesize = absl::GetFlag(FLAGS_payload_bytesize);
    CHECK_GE(payload_bytesize, 8) << "payload should be at least 8 bytes";

    std::string socket_type(absl::GetFlag(FLAGS_socket_type));
    int tcp_server_fd;
    int unix_fds[2];
    int pipe1_fds[2];
    int pipe2_fds[2];
    if (socket_type == "unix") {
        PCHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, unix_fds) == 0);
    } else if (socket_type == "pipe") {
        PCHECK(pipe(pipe1_fds) == 0);
        PCHECK(pipe(pipe2_fds) == 0);
    } else if (socket_type == "tcp") {
        tcp_server_fd = faas::utils::TcpSocketBindAndListen("127.0.0.1", absl::GetFlag(FLAGS_tcp_port));
    } else {
        LOG(FATAL) << "Unsupported socket type: " << socket_type;
    }

    pid_t child_pid = fork();
    if (child_pid == 0) {
        int infd = -1;
        int outfd = -1;
        if (socket_type == "unix") {
            infd = outfd = unix_fds[0];
        } else if (socket_type == "pipe") {
            infd = pipe1_fds[0];
            outfd = pipe2_fds[1];
        } else if (socket_type == "tcp") {
            infd = outfd = faas::utils::TcpSocketConnect("127.0.0.1", absl::GetFlag(FLAGS_tcp_port));
        }
        Client(infd, outfd, payload_bytesize, absl::GetFlag(FLAGS_stat_duration),
               absl::GetFlag(FLAGS_client_cpu));
        return 0;
    }

    PCHECK(child_pid != -1);
    int infd = -1;
    int outfd = -1;
    if (socket_type == "unix") {
        infd = outfd = unix_fds[1];
    } else if (socket_type == "pipe") {
        infd = pipe2_fds[0];
        outfd = pipe1_fds[1];
    } else if (socket_type == "tcp") {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(tcp_server_fd, (struct sockaddr*)&addr, &addr_len);
        PCHECK(fd != -1);
        infd = outfd = fd;
    }
    Server(infd, outfd, payload_bytesize, absl::GetFlag(FLAGS_duration),
           absl::GetFlag(FLAGS_stat_duration), absl::GetFlag(FLAGS_server_cpu));

    int wstatus;
    CHECK(wait(&wstatus) == child_pid);

    return 0;
}
