#pragma once

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <vector>

extern char** environ;

// The preview owns its X11 selection and GPU resources in a separate process.
class LivePreviewLauncher {
public:
    void open(const std::string& binary, const char* sourceFlag, const std::string& model,
              const std::string& kernels, int fps, bool frameGen, const char* backend, int multiplier) {
        if (pid_ > 0) return;
        std::vector<std::string> args{binary, sourceFlag, "pick", "--model", model, "--kernels", kernels,
                                      "--fps", std::to_string(fps), "--frame-gen", frameGen ? "1" : "0",
                                      "--fg-backend", backend, "--fg-multiplier", std::to_string(multiplier)};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        message_.clear();
        pid_t child;
        const int error = posix_spawn(&child, binary.c_str(), nullptr, nullptr, argv.data(), environ);
        if (error) message_ = std::string("Could not open live preview: ") + std::strerror(error);
        else pid_ = child;
    }

    void poll() {
        if (pid_ <= 0) return;
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return;
        pid_ = -1;
        if (result > 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
            message_ = "Live capture stopped. See the terminal for the source or GPU error.";
    }

    bool active() const { return pid_ > 0; }
    const std::string& message() const { return message_; }

private:
    pid_t pid_ = -1;
    std::string message_;
};
