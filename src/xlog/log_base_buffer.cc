// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

/*
 * log_base_buffer.cpp
 *
 *  Created on: 2015-7-28
 *      Author: yanguoyue
 */

#include "log_base_buffer.h"

#include <assert.h>
#include <string.h>
#include <algorithm>

#include "xlog/crypt/log_crypt.h"
#include "xlog/crypt/log_magic_num.h"

namespace mars {
namespace xlog {

LogBaseBuffer::LogBaseBuffer(void* _pbuffer, size_t _len, bool _isCompress, const char* _pubkey)
: is_compress_(_isCompress), log_crypt_(std::make_unique<LogCrypt>(_pubkey)), is_crypt_(log_crypt_->IsCrypt()) {
    buff_.Attach(_pbuffer, _len);
    __Fix();
}

LogBaseBuffer::~LogBaseBuffer() = default;

PtrBuffer& LogBaseBuffer::GetData() {
    return buff_;
}

void LogBaseBuffer::Flush(AutoBuffer& _buff) {
    if (log_crypt_->GetLogLen((char*)buff_.Ptr(), buff_.Length()) == 0) {
        __Clear();
        return;
    }

    __Flush();
    _buff.Write(buff_.Ptr(), buff_.Length());

    __Clear();
}

bool LogBaseBuffer::Write(const void* _data, size_t _inputlen, AutoBuffer& _out_buff) {
    if (NULL == _data || 0 == _inputlen) {
        return false;
    }

    log_crypt_->CryptSyncLog((char*)_data, _inputlen, _out_buff, __GetMagicSyncStart(), __GetMagicEnd());

    return true;
}

bool LogBaseBuffer::Write(const void* _data, size_t _length) {
    if (NULL == _data || 0 == _length) {
        return false;
    }
    if (buff_.Length() == 0) {
        if (!__Reset())
            return false;
    }

    size_t before_len = buff_.Length();
    size_t write_len = _length;

    if (is_compress_) {
        auto avail_out = buff_.MaxLength() - buff_.Length() - log_crypt_->GetTailerLen();
        write_len = Compress(_data, _length, buff_.PosPtr(), avail_out);
        if (write_len == (size_t)-1) {
            return false;
        }

    } else {
        buff_.Write(_data, _length);
    }

    before_len -= remain_nocrypt_len_;

    std::string out_buffer;
    size_t last_remain_len = remain_nocrypt_len_;

    log_crypt_->CryptAsyncLog((char*)buff_.Ptr() + before_len,
                              write_len + remain_nocrypt_len_,
                              out_buffer,
                              remain_nocrypt_len_);

    buff_.Write(out_buffer.data(), out_buffer.size(), before_len);

    before_len += out_buffer.size();
    buff_.Length(before_len, before_len);

    log_crypt_->UpdateLogLen((char*)buff_.Ptr(), (uint32_t)(out_buffer.size() - last_remain_len));

    return true;
}

bool LogBaseBuffer::__Reset() {
    __Clear();

    log_crypt_->SetHeaderInfo((char*)buff_.Ptr(), is_compress_, __GetMagicAsyncStart());
    buff_.Length(log_crypt_->GetHeaderLen(), log_crypt_->GetHeaderLen());
    return true;
}

void LogBaseBuffer::__Flush() {
    assert(buff_.Length() >= log_crypt_->GetHeaderLen());

    log_crypt_->UpdateLogHour((char*)buff_.Ptr());
    log_crypt_->SetTailerInfo((char*)buff_.Ptr() + buff_.Length(), __GetMagicEnd());
    buff_.Length(buff_.Length() + log_crypt_->GetTailerLen(), buff_.Length() + log_crypt_->GetTailerLen());
}

void LogBaseBuffer::__Clear() {
    memset(buff_.Ptr(), 0, buff_.MaxLength());
    buff_.Length(0, 0);
    remain_nocrypt_len_ = 0;
}

void LogBaseBuffer::__Fix() {
    uint32_t raw_log_len = 0;
    const auto overhead = log_crypt_->GetHeaderLen() + log_crypt_->GetTailerLen();
    if (buff_.MaxLength() >= overhead && log_crypt_->Fix(static_cast<char*>(buff_.Ptr()), buff_.Length(), raw_log_len)) {
        const auto max_payload = buff_.MaxLength() - overhead;
        const auto length = log_crypt_->GetHeaderLen() + std::min<std::size_t>(raw_log_len, max_payload);
        buff_.Length(length, length);
    } else {
        buff_.Length(0, 0);
    }
}

char LogBaseBuffer::__GetMagicEnd() {
    return LogMagicNum::kMagicEnd;
}

}  // namespace xlog
}  // namespace mars
