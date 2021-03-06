#pragma once

#include <IO/WriteBuffer.h>
#include <IO/BufferWithOwnMemory.h>

namespace DB
{

class BrotliWriteBuffer : public BufferWithOwnMemory<WriteBuffer>
{
public:
    BrotliWriteBuffer(
            WriteBuffer & out_,
            int compression_level,
            size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE,
            char * existing_memory = nullptr,
            size_t alignment = 0);

    ~BrotliWriteBuffer() override;

    void finish();

private:
    void nextImpl() override;

    class BrotliStateWrapper;
    std::unique_ptr<BrotliStateWrapper> brotli;

    size_t in_available;
    const uint8_t * in_data;

    size_t out_capacity;
    uint8_t  * out_data;

    WriteBuffer & out;

    bool finished = false;
};

}
