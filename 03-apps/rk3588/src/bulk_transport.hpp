#pragma once
#include <cstddef>
#include <memory>
#include <string>

// One session, owned FunctionFS instance. The launcher owns gadget binding.
class BulkTransport {
public:
    BulkTransport(const std::string& mount, size_t depth, double timeout);
    ~BulkTransport();
    void waitStart();
    void write(const void* data, size_t size);
    void flush();
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
