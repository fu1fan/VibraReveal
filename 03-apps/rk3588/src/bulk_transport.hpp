#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <functional>

// One session, owned FunctionFS instance. The launcher owns gadget binding.
class BulkTransport {
public:
    BulkTransport(const std::string& mount, size_t depth, double timeout,
                  std::function<bool()> cancelled = {});
    ~BulkTransport();
    void waitStart();
    void write(const void* data, size_t size);
    void flush();
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
