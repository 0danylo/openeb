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

#include "metavision/hal/utils/hal_exception.h"
#include "metavision/hal/utils/data_transfer.h"

namespace Metavision {

DataTransfer::DataTransfer(uint32_t raw_event_size_bytes) : raw_event_size_bytes_(raw_event_size_bytes) {}

DataTransfer::DataTransfer(uint32_t raw_event_size_bytes, const BufferPool &buffer_pool, bool allow_buffer_drop) :
    raw_event_size_bytes_(raw_event_size_bytes), buffer_pool_(buffer_pool), allow_buffer_drop_(allow_buffer_drop) {
    if (buffer_pool_.is_bounded() && buffer_pool_.size() < 3) {
        throw HalException(HalErrorCode::InvalidArgument,
                           "A DataTransfer can not be initialized with a bounded object pool of size < 3 (got size " +
                               std::to_string(buffer_pool_.size()) + ").");
    }
}

DataTransfer::~DataTransfer() {
    // not calling stop here, rather leaving this "burden" to the user of the data transfer
    // will will most probably be in the destructor of I_EventStream
    // if done from this destructor, it will be too late to call a derived version of stop_impl
    // if one was defined
}

void DataTransfer::start() {
    if (run_transfers_thread_.joinable()) {
        return;
    }

    stop_ = false;
    start_impl(get_buffer());

    run_transfers_thread_ = std::thread([this]() {
        for (auto cb : status_change_cbs_) {
            cb.second(Status::Started);
        }

        run_impl();

        stop_ = true;
        for (auto cb : status_change_cbs_) {
            cb.second(Status::Stopped);
        }
    });

    while (!run_transfers_thread_.joinable()) {}
}

void DataTransfer::stop() {
    if (!run_transfers_thread_.joinable()) {
        return;
    }

    stop_impl();
    stop_ = true;

    run_transfers_thread_.join();
}

size_t DataTransfer::add_status_changed_callback(StatusChangeCallback_t cb) {
    status_change_cbs_[cb_index_] = cb;
    auto ret                      = cb_index_;
    ++cb_index_;
    return ret;
}

size_t DataTransfer::add_new_buffer_callback(NewBufferCallback_t cb) {
    new_buffer_cbs_[cb_index_] = cb;
    auto ret                   = cb_index_;
    ++cb_index_;
    return ret;
}

void DataTransfer::remove_callback(size_t cb_id) {
    status_change_cbs_.erase(cb_id);
    new_buffer_cbs_.erase(cb_id);
}

uint32_t DataTransfer::get_raw_event_size_bytes() const {
    return raw_event_size_bytes_;
}

bool DataTransfer::should_stop() {
    return stop_;
}

void DataTransfer::start_impl(BufferPtr buffer) {}

void DataTransfer::stop_impl() {}

void DataTransfer::fire_callbacks(const BufferPtr buffer) const {
    for (auto &cb : new_buffer_cbs_) {
        cb.second(buffer);
    }
}

} // namespace Metavision
