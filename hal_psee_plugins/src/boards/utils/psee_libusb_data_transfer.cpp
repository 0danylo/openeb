/**********************************************************************************************************************
 * Copyright (c) Prophesee S.A.                                                                                       *
 *                                                                                                                    *
 * Licensed under the Apache License, Version 2.0 (the "License");                                                    *
 * you may not use this file except in compliance with the License.                                                   *
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0                                 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed   *
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.                      *
 * See the License for the specific language governing permissions and limitations under the License.                 *
 **********************************************************************************************************************/

#include <assert.h>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#ifndef _MSC_VER
#define WIN_CALLBACK_DECL __stdcall
#else
#define WIN_CALLBACK_DECL WINAPI
#endif
#else
#define WIN_CALLBACK_DECL
#endif

#include "boards/utils/psee_libusb_data_transfer.h"
#include "boards/utils/psee_libusb_board_command.h"
#include "boards/utils/config_registers_map.h"

#include "metavision/hal/utils/hal_log.h"

const static int USB_TIME_OUT                = 100;
const static int N_ASYNC_TRANFERS_PER_DEVICE = 20;
const static int PACKET_SIZE                 = 128 * 1024;

namespace Metavision {

namespace {
size_t get_envar_or_default(const std::string &envvar, size_t default_val) {
    size_t val = default_val;
    try {
        auto *envar_value = getenv(envvar.c_str());
        if (envar_value) {
            std::stringstream ss(envar_value);
            ss >> val;
        }
    } catch (...) {}
    return val;
}

size_t get_async_transfer_number() {
    return get_envar_or_default("MV_PSEE_DEBUG_PLUGIN_USB_ASYNC_TRANSFER", N_ASYNC_TRANFERS_PER_DEVICE);
}

size_t get_packet_size() {
    return get_envar_or_default("MV_PSEE_DEBUG_PLUGIN_USB_PACKET_SIZE", PACKET_SIZE);
}

size_t get_time_out() {
    return get_envar_or_default("MV_PSEE_DEBUG_PLUGIN_USB_TIME_OUT", USB_TIME_OUT);
}

} // namespace

class PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback {
public:
    UserParamForAsyncBulkCallback(const std::shared_ptr<PseeLibUSBBoardCommand> &cmd,
                                  PseeLibUSBDataTransfer &libusb_data_transfer);
    ~UserParamForAsyncBulkCallback();
    static void WIN_CALLBACK_DECL async_bulk_cb(struct libusb_transfer *transfer);

    void start();
    void stop();

private:
    bool proceed_async_bulk(struct libusb_transfer *transfer);

    const static size_t timeout_;

    DataTransfer::BufferPtr buf_;
    std::mutex transfer_mutex_;
    libusb_transfer *transfer_{nullptr};
    bool stop_ = true;
    std::atomic<bool> submitted_transfer_{false};
    std::shared_ptr<PseeLibUSBBoardCommand> cmd_;
    PseeLibUSBDataTransfer &libusb_data_transfer_;
};

const size_t PseeLibUSBDataTransfer::packet_size_                            = get_packet_size();
const size_t PseeLibUSBDataTransfer::async_transfer_num_                     = get_async_transfer_number();
const size_t PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::timeout_ = get_time_out();

DataTransfer::BufferPool PseeLibUSBDataTransfer::make_buffer_pool(size_t default_pool_byte_size) {
    DataTransfer::BufferPool pool =
        DataTransfer::BufferPool::make_unbounded(PseeLibUSBDataTransfer::async_transfer_num_, get_packet_size());
    auto buffer_pool_byte_size =
        get_envar_or_default("MV_PSEE_PLUGIN_DATA_TRANSFER_BUFFER_POOL_BYTE_SIZE", default_pool_byte_size);
    if (buffer_pool_byte_size) {
        auto num_obj_pool = buffer_pool_byte_size / packet_size_;
        MV_HAL_LOG_INFO() << "Creating Fixed size data pool of : " << num_obj_pool << "x" << packet_size_ << "B";
        pool = DataTransfer::BufferPool::make_bounded(num_obj_pool, packet_size_);
    }

    return pool;
}

PseeLibUSBDataTransfer::PseeLibUSBDataTransfer(const std::shared_ptr<PseeLibUSBBoardCommand> &cmd,
                                               uint32_t raw_event_size_bytes,
                                               const DataTransfer::BufferPool &buffer_pool) :
    DataTransfer(raw_event_size_bytes, buffer_pool, true), cmd_(cmd) {}

PseeLibUSBDataTransfer::~PseeLibUSBDataTransfer() {
    stop_impl();
    cmd_->try_to_flush();
}

void PseeLibUSBDataTransfer::start_impl(BufferPtr buffer) {
    buffer.reset(); // we don't use the buffer here... let's put it back in the pool
    initiate_async_transfers();
}

void PseeLibUSBDataTransfer::run_impl() {
    MV_HAL_LOG_TRACE() << "poll thread running";
    while (!should_stop() && active_bulks_transfers_ > 0) {
        // Ensure we'll have sufficient space to handle all transfers on the next iteration
        get_buffer_pool().arrange(async_transfer_num_, packet_size_);

        struct timeval tv = {0, 1};
        libusb_handle_events_timeout_completed(nullptr, &tv, nullptr);
    }
    MV_HAL_LOG_TRACE() << "poll thread shutting down";

    release_async_transfers();
}

void PseeLibUSBDataTransfer::stop_impl() {
    for (auto &transfer : vtransfer_) {
        transfer->stop();
    }
}

void PseeLibUSBDataTransfer::initiate_async_transfers() {
    std::generate_n(std::back_inserter(vtransfer_), async_transfer_num_, [this]() {
        auto user_param = std::make_unique<UserParamForAsyncBulkCallback>(cmd_, *this);
        user_param->start();
        return user_param;
    });
}

void PseeLibUSBDataTransfer::release_async_transfers() {
    vtransfer_.clear();
}

PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::UserParamForAsyncBulkCallback(
    const std::shared_ptr<PseeLibUSBBoardCommand> &cmd, PseeLibUSBDataTransfer &libusb_data_transfer) :
    cmd_(cmd), libusb_data_transfer_(libusb_data_transfer) {
    buf_      = libusb_data_transfer.get_buffer();
    transfer_ = cmd->contruct_async_bulk_transfer(buf_->data(), libusb_data_transfer.packet_size_, async_bulk_cb, this,
                                                  timeout_);
}

PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::~UserParamForAsyncBulkCallback() {
    stop();

    // Wait for submitted transfer to be processed before releasing this object.
    while (submitted_transfer_) {
        struct timeval tv = {0, 1};
        libusb_handle_events_timeout(nullptr, &tv);
    };

    if (transfer_) {
        cmd_->free_async_bulk_transfer(transfer_);
        transfer_ = nullptr;
    }
}

void WIN_CALLBACK_DECL
    PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::async_bulk_cb(struct libusb_transfer *transfer) {
    if (!transfer->user_data)
        return;

    UserParamForAsyncBulkCallback *param = static_cast<UserParamForAsyncBulkCallback *>(transfer->user_data);
    const bool ret                       = param->proceed_async_bulk(transfer);
    if (!ret) {
        param->stop();
    }
    param->submitted_transfer_ = ret;
}

void PseeLibUSBDataTransfer::preprocess_transfer(libusb_transfer *) {}

bool PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::proceed_async_bulk(libusb_transfer *transfer) {
    std::lock_guard<std::mutex> lock(transfer_mutex_);

    assert(transfer == transfer_);
    assert(transfer->buffer == buf_->data());

    if (stop_) {
        return false;
    }

    libusb_data_transfer_.preprocess_transfer(transfer);

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
        {
            MV_HAL_LOG_ERROR() << "ErrTransfert";
            MV_HAL_LOG_ERROR() << libusb_error_name(transfer->status);
            if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
                MV_HAL_LOG_ERROR() << "LIBUSB_TRANSFER_NO_DEVICE";
                return false;
            }
        }
        int r = cmd_->submit_transfer(transfer);
        if (r != 0) {
            MV_HAL_LOG_ERROR() << "Resubmit Error after Error";
            MV_HAL_LOG_ERROR() << libusb_error_name(r);
            return false;
        }
        return true;
    }

    const auto remainder = transfer->actual_length % libusb_data_transfer_.get_raw_event_size_bytes();
    if (remainder) {
        MV_HAL_LOG_WARNING() << "Buffer is not a multiple of a RAW events byte size ("
                             << libusb_data_transfer_.get_raw_event_size_bytes() << "). A RAW event has been dropped.";
    }

    buf_->resize(transfer->actual_length - remainder);

    auto transfered_data = libusb_data_transfer_.transfer_data(buf_);

    buf_ = transfered_data;
    buf_->resize(libusb_data_transfer_.packet_size_);

    transfer->buffer = buf_->data();
    int r            = cmd_->submit_transfer(transfer);
    if (r != 0) {
        MV_HAL_LOG_ERROR() << "Resubmit error after transfer OK";
        MV_HAL_LOG_ERROR() << libusb_error_name(r);
        return false;
    }

    return true;
}

void PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::start() {
    std::lock_guard<std::mutex> lock(transfer_mutex_); // avoid concurrent access to stop

    submitted_transfer_ = true;
    int r               = cmd_->submit_transfer(transfer_);
    if (r != 0) {
        MV_HAL_LOG_ERROR() << "Submit error in start";
        MV_HAL_LOG_ERROR() << libusb_error_name(r);
        return;
    }

    stop_ = false;
    ++libusb_data_transfer_.active_bulks_transfers_;
}

void PseeLibUSBDataTransfer::UserParamForAsyncBulkCallback::stop() {
    std::lock_guard<std::mutex> lock(transfer_mutex_);
    if (stop_) {
        return;
    }
    stop_ = true;
    --libusb_data_transfer_.active_bulks_transfers_;
}

} // namespace Metavision
