// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

//
//  log_crypt.cc
//  mars-ext
//
//  Created by garry on 16/6/14.
//

#include "log_crypt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>
#include <iterator>

#include "comm/time_utils.h"
#include "log_magic_num.h"

#ifdef WIN32
#include <algorithm>
#endif  // WIN32

#ifndef XLOG_NO_CRYPT
#include "micro-ecc-master/uECC.h"
#endif

namespace mars {
namespace xlog {

using record::LoadLittleEndian;
using record::StoreLittleEndian;

constexpr std::size_t kTeaBlockSize = 2 * sizeof(uint32_t);

static auto HeaderBytes(char* data) {
    return std::as_writable_bytes(std::span<char, record::kHeaderSize>(data, record::kHeaderSize));
}

static void __TeaEncrypt(uint32_t* v, uint32_t* k) {
    uint32_t v0 = v[0], v1 = v[1], sum = 0, i;
    const static uint32_t delta = 0x9e3779b9;
    uint32_t k0 = k[0], k1 = k[1], k2 = k[2], k3 = k[3];
    for (i = 0; i < 16; i++) {
        sum += delta;
        v0 += ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
        v1 += ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
    }
    v[0] = v0;
    v[1] = v1;
}

static uint16_t __GetSeq(bool _is_async) {
    if (!_is_async) {
        return 0;
    }

    static std::atomic<uint16_t> sequence{0};
    auto previous = sequence.load(std::memory_order_relaxed);
    uint16_t next;
    do {
        next = static_cast<uint16_t>(previous + 1);
        if (next == 0)
            next = 1;
    } while (!sequence.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    return next;
}

#ifndef XLOG_NO_CRYPT
static bool Hex2Buffer(const char* _str, size_t _len, unsigned char* _buffer) {
    if (NULL == _str || _len == 0 || _len % 2 != 0) {
        return false;
    }

    char tmp[3] = {0};
    for (size_t i = 0; i < _len - 1; i += 2) {
        for (size_t j = 0; j < 2; ++j) {
            tmp[j] = _str[i + j];
            if (!(('0' <= tmp[j] && tmp[j] <= '9') || ('a' <= tmp[j] && tmp[j] <= 'f')
                  || ('A' <= tmp[j] && tmp[j] <= 'F'))) {
                return false;
            }
        }

        _buffer[i / 2] = (unsigned char)strtol(tmp, NULL, 16);
    }
    return true;
}
#endif

LogCrypt::LogCrypt(const char* _pubkey) : is_crypt_(false) {
#ifndef XLOG_NO_CRYPT
    constexpr auto PUB_KEY_LEN = record::kPublicKeySize;

    if (NULL == _pubkey || PUB_KEY_LEN * 2 != strnlen(_pubkey, 256)) {
        return;
    }

    unsigned char svr_pubkey[PUB_KEY_LEN] = {0};

    if (!Hex2Buffer(_pubkey, PUB_KEY_LEN * 2, svr_pubkey)) {
        return;
    }

    uint8_t client_pri[32] = {0};
    if (0 == uECC_make_key((uint8_t*)client_pubkey_, client_pri, uECC_secp256k1())) {
        return;
    }

    uint8_t ecdh_key[32] = {0};
    if (0 == uECC_shared_secret(svr_pubkey, client_pri, ecdh_key, uECC_secp256k1())) {
        return;
    }

    const auto key = std::as_bytes(std::span(ecdh_key));
    for (std::size_t i = 0; i < std::size(tea_key_); ++i) {
        tea_key_[i] = LoadLittleEndian<uint32_t>(
            std::span<const std::byte, sizeof(uint32_t)>(key.data() + i * sizeof(uint32_t), sizeof(uint32_t)));
    }

    is_crypt_ = true;

#endif
}

/*
 * |magic start(char)|seq(uint16_t)|begin hour(char)|end hour(char)|length(uint32_t)|crypt key(char*64)|
 */

uint32_t LogCrypt::GetHeaderLen() {
    return record::kHeaderSize;
}

uint32_t LogCrypt::GetTailerLen() {
    return sizeof(char);
}

void LogCrypt::UpdateLogHour(char* _data) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t sec = tv.tv_sec;
    struct tm tm_tmp = mars_local_time(sec);

    _data[record::kEndHourOffset] = static_cast<char>(tm_tmp.tm_hour);
}

uint32_t LogCrypt::GetLogLen(const char* const _data, size_t _len) {
    if (_len < GetHeaderLen())
        return 0;

    char start = _data[0];
    if (!LogMagicNum::MagicStartIsValid(start)) {
        return 0;
    }

    const auto bytes = std::as_bytes(std::span(_data, _len));
    return LoadLittleEndian<uint32_t>(bytes.subspan<record::kLengthOffset, sizeof(uint32_t)>());
}

void LogCrypt::UpdateLogLen(char* _data, uint32_t _add_len) {
    const auto length = GetLogLen(_data, GetHeaderLen()) + _add_len;
    StoreLittleEndian(HeaderBytes(_data).subspan<record::kLengthOffset, sizeof(uint32_t)>(), length);
}

void LogCrypt::SetHeaderInfo(char* _data, bool _is_async, char _magic_start) {
    _data[0] = _magic_start;
    auto bytes = HeaderBytes(_data);
    StoreLittleEndian(bytes.subspan<record::kSequenceOffset, sizeof(uint16_t)>(), __GetSeq(_is_async));

    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t sec = tv.tv_sec;
    tm tm_tmp = mars_local_time(sec);

    const auto hour = static_cast<char>(tm_tmp.tm_hour);
    _data[record::kBeginHourOffset] = hour;
    _data[record::kEndHourOffset] = hour;
    StoreLittleEndian<uint32_t>(bytes.subspan<record::kLengthOffset, sizeof(uint32_t)>(), 0);
    memcpy(_data + record::kPublicKeyOffset, client_pubkey_, sizeof(client_pubkey_));
}

void LogCrypt::SetTailerInfo(char* _data, char _magic_end) {
    memcpy(_data, &_magic_end, sizeof(_magic_end));
}

void LogCrypt::CryptSyncLog(const char* const _log_data,
                            size_t _input_len,
                            AutoBuffer& _out_buff,
                            char _magic_start,
                            char _magic_end) {
    _out_buff.AllocWrite(GetHeaderLen() + GetTailerLen() + _input_len);

    SetHeaderInfo((char*)_out_buff.Ptr(), false, _magic_start);

    uint32_t header_len = GetHeaderLen();

    UpdateLogLen((char*)_out_buff.Ptr(), (uint32_t)_input_len);

    SetTailerInfo((char*)_out_buff.Ptr() + _input_len + header_len, _magic_end);

    memcpy(static_cast<char*>(_out_buff.Ptr()) + header_len, _log_data, _input_len);
}

void LogCrypt::CryptAsyncLog(const char* const _log_data,
                             size_t _input_len,
                             std::string& _out_buff,
                             size_t& _remain_nocrypt_len) {
    if (!is_crypt_) {
        _out_buff.assign(_log_data, _input_len);
        _remain_nocrypt_len = 0;
        return;
    }
#ifndef XLOG_NO_CRYPT
    auto input = std::span(_log_data, _input_len);
    char encrypted[kTeaBlockSize];
    const auto output = std::as_writable_bytes(std::span(encrypted));

    while (input.size() >= kTeaBlockSize) {
        const auto block = std::as_bytes(input.first<kTeaBlockSize>());
        uint32_t words[] = {
            LoadLittleEndian<uint32_t>(block.first<sizeof(uint32_t)>()),
            LoadLittleEndian<uint32_t>(block.last<sizeof(uint32_t)>()),
        };
        __TeaEncrypt(words, tea_key_);
        StoreLittleEndian(output.first<sizeof(uint32_t)>(), words[0]);
        StoreLittleEndian(output.last<sizeof(uint32_t)>(), words[1]);
        _out_buff.append(encrypted, sizeof(encrypted));
        input = input.subspan(kTeaBlockSize);
    }
    _remain_nocrypt_len = input.size();
    _out_buff.append(input.data(), input.size());
#endif
}

bool LogCrypt::Fix(char* _data, size_t _data_len, uint32_t& _raw_log_len) {
    if (_data_len < GetHeaderLen()) {
        return false;
    }

    char start = _data[0];
    if (!LogMagicNum::MagicStartIsValid(start)) {
        return false;
    }

    _raw_log_len = GetLogLen(_data, _data_len);

    return true;
}

bool LogCrypt::IsCrypt() {
    return is_crypt_;
}

}  // namespace xlog
}  // namespace mars
